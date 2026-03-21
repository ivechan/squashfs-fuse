/*
 * SquashFS-FUSE - Compressor Interface
 *
 * Copyright (c) 2024 SquashFS-FUSE Authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file defines the compressor abstraction layer for SquashFS.
 * Supported compressors: zlib (gzip), zstd
 */

#ifndef SQFS_COMPRESSOR_H
#define SQFS_COMPRESSOR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * SquashFS compressor IDs (from format specification)
 */
typedef enum {
    SQFS_COMP_GZIP = 1,    /* zlib deflate (raw zlib streams, no gzip headers) */
    SQFS_COMP_LZMA = 2,    /* LZMA version 1 (deprecated) */
    SQFS_COMP_LZO  = 3,    /* LZO */
    SQFS_COMP_XZ   = 4,    /* LZMA version 2 (xz-utils) */
    SQFS_COMP_LZ4  = 5,    /* LZ4 */
    SQFS_COMP_ZSTD = 6,    /* Zstandard */
} sqfs_compressor_id_t;

/*
 * Compressor return codes
 */
#define SQFS_COMP_OK            0    /* Success */
#define SQFS_COMP_ERROR         -1   /* General error */
#define SQFS_COMP_OVERFLOW      -2   /* Output buffer too small */
#define SQFS_COMP_CORRUPT       -3   /* Data corruption */
#define SQFS_COMP_UNSUPPORTED   -4   /* Unsupported compression algorithm */
#define SQFS_COMP_NO_MEMORY     -5   /* Memory allocation failed */

/*
 * Block size marker for uncompressed data
 * Bit 24 set in block size indicates uncompressed block
 */
#define SQFS_BLOCK_UNCOMPRESSED_FLAG    (1U << 24)
#define SQFS_BLOCK_SIZE_MASK            0x00FFFFFF

/*
 * Metadata block header flags
 * Bit 15 set indicates uncompressed block
 * Bits 0-14 contain compressed size (max 8 KiB)
 */
#define SQFS_META_UNCOMPRESSED_FLAG     0x8000
#define SQFS_META_MAX_SIZE              8192

/*
 * Forward declaration
 */
typedef struct sqfs_compressor sqfs_compressor_t;

/*
 * Compressor operations
 *
 * Each compressor implementation must provide these operations.
 * The decompress function is mandatory; init/destroy are optional.
 */
struct sqfs_compressor {
    const char *name;           /* Human-readable name */
    sqfs_compressor_id_t id;    /* Compressor ID */

    /*
     * Decompress data
     *
     * @param src       Source buffer (compressed data)
     * @param src_size  Size of compressed data
     * @param dst       Destination buffer (decompressed data)
     * @param dst_size  Size of destination buffer
     * @param out_size  Output: actual decompressed size (may be NULL)
     *
     * @return SQFS_COMP_OK on success, negative error code on failure
     */
    int (*decompress)(const void *src, size_t src_size,
                      void *dst, size_t dst_size, size_t *out_size);

    /*
     * Initialize compressor context (optional)
     * Some compressors need persistent context for streaming operations.
     *
     * @param ctx   Output: compressor context pointer
     *
     * @return SQFS_COMP_OK on success, negative error code on failure
     */
    int (*init)(void **ctx);

    /*
     * Destroy compressor context (optional)
     *
     * @param ctx   Compressor context to destroy
     */
    void (*destroy)(void *ctx);

    /* Internal use: context pointer for compressors that need it */
    void *priv;
};

/*
 * Decompress data using a compressor
 *
 * @param comp      Compressor instance
 * @param src       Source buffer (compressed data)
 * @param src_size  Size of compressed data
 * @param dst       Destination buffer (decompressed data)
 * @param dst_size  Size of destination buffer
 * @param out_size  Output: actual decompressed size (may be NULL)
 *
 * @return SQFS_COMP_OK on success, negative error code on failure
 */
int sqfs_compressor_decompress(sqfs_compressor_t *comp,
                               const void *src, size_t src_size,
                               void *dst, size_t dst_size,
                               size_t *out_size);

/*
 * Create a compressor instance by ID
 *
 * @param id    Compressor ID (SQFS_COMP_GZIP, SQFS_COMP_ZSTD, etc.)
 *
 * @return Pointer to compressor instance, or NULL if unsupported
 */
sqfs_compressor_t *sqfs_compressor_create(sqfs_compressor_id_t id);

/*
 * Destroy a compressor instance
 *
 * @param comp  Compressor instance to destroy
 */
void sqfs_compressor_destroy(sqfs_compressor_t *comp);

/*
 * Check if a compressor ID is supported
 *
 * @param id    Compressor ID to check
 *
 * @return true if supported, false otherwise
 */
int sqfs_compressor_is_supported(sqfs_compressor_id_t id);

/*
 * Get human-readable error message
 *
 * @param err   Error code from compressor operation
 *
 * @return Static string describing the error
 */
const char *sqfs_compressor_strerror(int err);

/*
 * Built-in compressor implementations
 */
extern sqfs_compressor_t *sqfs_compressor_zlib_new(void);
extern sqfs_compressor_t *sqfs_compressor_zstd_new(void);

#ifdef __cplusplus
}
#endif

#endif /* SQFS_COMPRESSOR_H */