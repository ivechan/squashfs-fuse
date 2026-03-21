/*
 * SquashFS-FUSE - Compressor Implementation
 *
 * Copyright (c) 2024 SquashFS-FUSE Authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file implements the compressor abstraction layer for SquashFS.
 * Supported compressors: zlib (gzip), zstd
 *
 * Thread Safety:
 * - Zstd uses Thread-Local Storage (TLS) for decompression contexts
 * - Each thread gets its own ZSTD_DStream, avoiding lock contention
 */

#include "compressor.h"

#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include <zlib.h>
#include <zstd.h>

/*
 * Zlib compressor implementation
 */

static int zlib_decompress(const void *src, size_t src_size,
                           void *dst, size_t dst_size, size_t *out_size)
{
    z_stream strm;
    int ret;

    if (src == NULL || dst == NULL) {
        return SQFS_COMP_ERROR;
    }

    memset(&strm, 0, sizeof(strm));

    /* SquashFS typically uses raw deflate, but some implementations
     * use zlib format with headers. We detect the format by checking
     * the first byte:
     * - 0x78 indicates zlib format (0x78 = CMF for deflate with window size 15)
     * - Other values indicate raw deflate
     */
    const unsigned char *src_bytes = (const unsigned char *)src;
    int window_bits;

    if (src_size >= 2 && src_bytes[0] == 0x78 &&
        (src_bytes[1] == 0x01 || src_bytes[1] == 0x5e ||
         src_bytes[1] == 0x9c || src_bytes[1] == 0xda)) {
        /* zlib format with header detected */
        window_bits = 15;  /* zlib auto-detect */
    } else {
        /* raw deflate without header */
        window_bits = -15;
    }

    ret = inflateInit2(&strm, window_bits);
    if (ret != Z_OK) {
        return SQFS_COMP_ERROR;
    }

    strm.next_in = (Bytef *)src;
    strm.avail_in = src_size;
    strm.next_out = (Bytef *)dst;
    strm.avail_out = dst_size;

    ret = inflate(&strm, Z_FINISH);

    if (ret != Z_STREAM_END) {
        inflateEnd(&strm);

        if (ret == Z_DATA_ERROR) {
            return SQFS_COMP_CORRUPT;
        }
        if (ret == Z_BUF_ERROR || strm.avail_out > 0) {
            return SQFS_COMP_OVERFLOW;
        }
        return SQFS_COMP_ERROR;
    }

    if (out_size != NULL) {
        *out_size = strm.total_out;
    }

    inflateEnd(&strm);
    return SQFS_COMP_OK;
}

typedef struct {
    ZSTD_DStream *dstream;
} zlib_ctx_t;

static int zlib_init(void **ctx)
{
    zlib_ctx_t *c;

    c = calloc(1, sizeof(*c));
    if (c == NULL) {
        return SQFS_COMP_NO_MEMORY;
    }

    c->dstream = NULL;  /* zlib doesn't need persistent context for inflate */
    *ctx = c;
    return SQFS_COMP_OK;
}

static void zlib_destroy(void *ctx)
{
    if (ctx != NULL) {
        free(ctx);
    }
}

sqfs_compressor_t *sqfs_compressor_zlib_new(void)
{
    sqfs_compressor_t *comp;

    comp = calloc(1, sizeof(*comp));
    if (comp == NULL) {
        return NULL;
    }

    comp->name = "gzip";
    comp->id = SQFS_COMP_GZIP;
    comp->decompress = zlib_decompress;
    comp->init = zlib_init;
    comp->destroy = zlib_destroy;
    comp->priv = NULL;

    return comp;
}

/*
 * Zstd compressor implementation
 *
 * Uses Thread-Local Storage (TLS) for decompression contexts.
 * Each thread gets its own ZSTD_DStream to avoid lock contention
 * in multi-threaded FUSE mode.
 */

/* Thread-local storage for ZSTD_DStream */
static pthread_key_t zstd_dstream_key;
static pthread_once_t zstd_key_once = PTHREAD_ONCE_INIT;

/* Free a thread-local ZSTD_DStream when thread exits */
static void zstd_dstream_free(void *ptr)
{
    ZSTD_DStream *dstream = (ZSTD_DStream *)ptr;
    if (dstream != NULL) {
        ZSTD_freeDStream(dstream);
    }
}

/* Create the TLS key (called once per process) */
static void zstd_create_key(void)
{
    pthread_key_create(&zstd_dstream_key, zstd_dstream_free);
}

/* Get or create the current thread's ZSTD_DStream */
static ZSTD_DStream *get_or_create_dstream(void)
{
    ZSTD_DStream *dstream;

    /* Ensure TLS key is created */
    pthread_once(&zstd_key_once, zstd_create_key);

    /* Get existing context for this thread */
    dstream = (ZSTD_DStream *)pthread_getspecific(zstd_dstream_key);
    if (dstream != NULL) {
        return dstream;
    }

    /* Create new context for this thread */
    dstream = ZSTD_createDStream();
    if (dstream == NULL) {
        return NULL;
    }

    /* Initialize the decompression context */
    if (ZSTD_isError(ZSTD_initDStream(dstream))) {
        ZSTD_freeDStream(dstream);
        return NULL;
    }

    /* Store in TLS */
    pthread_setspecific(zstd_dstream_key, dstream);
    return dstream;
}

static int zstd_decompress(const void *src, size_t src_size,
                           void *dst, size_t dst_size, size_t *out_size)
{
    ZSTD_DStream *dstream;
    ZSTD_inBuffer input;
    ZSTD_outBuffer output;
    size_t ret;

    if (src == NULL || dst == NULL) {
        return SQFS_COMP_ERROR;
    }

    /* Get thread-local decompression context */
    dstream = get_or_create_dstream();
    if (dstream == NULL) {
        return SQFS_COMP_NO_MEMORY;
    }

    /* Reset the context for a new decompression session */
    ret = ZSTD_DCtx_reset(dstream, ZSTD_reset_session_only);
    if (ZSTD_isError(ret)) {
        return SQFS_COMP_ERROR;
    }

    input.src = src;
    input.size = src_size;
    input.pos = 0;

    output.dst = dst;
    output.size = dst_size;
    output.pos = 0;

    ret = ZSTD_decompressStream(dstream, &output, &input);

    if (ZSTD_isError(ret)) {
        return SQFS_COMP_ERROR;
    }

    /* Check if all input was consumed */
    if (input.pos < input.size) {
        return SQFS_COMP_CORRUPT;
    }

    if (out_size != NULL) {
        *out_size = output.pos;
    }

    return SQFS_COMP_OK;
}

/* No-op init/destroy for TLS-based decompression */
static int zstd_init(void **ctx)
{
    /* TLS handles context creation, no per-instance context needed */
    *ctx = NULL;
    return SQFS_COMP_OK;
}

static void zstd_destroy(void *ctx)
{
    /* TLS handles cleanup via pthread_key_create destructor */
    (void)ctx;
}

sqfs_compressor_t *sqfs_compressor_zstd_new(void)
{
    sqfs_compressor_t *comp;

    comp = calloc(1, sizeof(*comp));
    if (comp == NULL) {
        return NULL;
    }

    comp->name = "zstd";
    comp->id = SQFS_COMP_ZSTD;
    comp->decompress = zstd_decompress;
    comp->init = zstd_init;
    comp->destroy = zstd_destroy;
    comp->priv = NULL;

    return comp;
}

/*
 * Generic compressor interface
 */

int sqfs_compressor_decompress(sqfs_compressor_t *comp,
                               const void *src, size_t src_size,
                               void *dst, size_t dst_size,
                               size_t *out_size)
{
    if (comp == NULL || comp->decompress == NULL) {
        return SQFS_COMP_UNSUPPORTED;
    }

    return comp->decompress(src, src_size, dst, dst_size, out_size);
}

sqfs_compressor_t *sqfs_compressor_create(sqfs_compressor_id_t id)
{
    switch (id) {
    case SQFS_COMP_GZIP:
        return sqfs_compressor_zlib_new();
    case SQFS_COMP_ZSTD:
        return sqfs_compressor_zstd_new();
    case SQFS_COMP_LZMA:
    case SQFS_COMP_LZO:
    case SQFS_COMP_XZ:
    case SQFS_COMP_LZ4:
        /* Not implemented */
        return NULL;
    default:
        return NULL;
    }
}

void sqfs_compressor_destroy(sqfs_compressor_t *comp)
{
    if (comp == NULL) {
        return;
    }

    if (comp->destroy != NULL && comp->priv != NULL) {
        comp->destroy(comp->priv);
    }

    free(comp);
}

int sqfs_compressor_is_supported(sqfs_compressor_id_t id)
{
    switch (id) {
    case SQFS_COMP_GZIP:
    case SQFS_COMP_ZSTD:
        return 1;
    case SQFS_COMP_LZMA:
    case SQFS_COMP_LZO:
    case SQFS_COMP_XZ:
    case SQFS_COMP_LZ4:
        return 0;
    default:
        return 0;
    }
}

const char *sqfs_compressor_strerror(int err)
{
    switch (err) {
    case SQFS_COMP_OK:
        return "Success";
    case SQFS_COMP_ERROR:
        return "General compression error";
    case SQFS_COMP_OVERFLOW:
        return "Output buffer too small";
    case SQFS_COMP_CORRUPT:
        return "Compressed data is corrupt";
    case SQFS_COMP_UNSUPPORTED:
        return "Unsupported compression algorithm";
    case SQFS_COMP_NO_MEMORY:
        return "Memory allocation failed";
    default:
        return "Unknown error";
    }
}