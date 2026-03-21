/*
 * SquashFS-FUSE - Data Block Reading Implementation
 *
 * Copyright (c) 2024 SquashFS-FUSE Authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file implements data block reading for SquashFS.
 * Handles reading, decompression, and streaming of file data.
 */

#include "data.h"
#include "superblock.h"
#include "compressor.h"
#include "cache.h"
#include "utils.h"
#include "inode.h"
#include "fragment.h"
#include "log.h"
#include "context.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdbool.h>

/* ============================================================================
 * Internal Helper Functions
 * ============================================================================ */

/*
 * Read exactly 'count' bytes from file descriptor at given offset.
 * Returns 0 on success, negative error code on failure.
 */
static int read_block_from_disk(int fd, uint64_t pos, void *buf, size_t count)
{
    if (count == 0) {
        return 0;
    }

    if (sqfs_pread_all(fd, buf, count, pos) != 0) {
        SQFS_LOG_DATA_ERROR("Failed to read %zu bytes at offset %lu", count, (unsigned long)pos);
        return SQFS_ERR_IO;
    }

    return 0;
}

/*
 * Calculate block boundaries for a file
 */
static void calculate_block_layout(sqfs_file_ctx_t *ctx, uint64_t file_size,
                                   uint32_t block_size)
{
    if (file_size == 0) {
        ctx->block_count = 0;
        ctx->tail_size = 0;
        return;
    }

    /* Calculate number of full blocks */
    ctx->block_count = file_size / block_size;
    ctx->tail_size = file_size % block_size;

    /*
     * If there's a tail and no fragments are used, the tail is stored
     * as a separate partial block, so we need to count it.
     * With fragments, the tail goes in a fragment block instead.
     */
    if (ctx->tail_size > 0 && sqfs_fragment_is_none(ctx->frag_idx)) {
        ctx->block_count++;
    }
}

/* ============================================================================
 * File Context Management
 * ============================================================================ */

int sqfs_file_ctx_init(sqfs_file_ctx_t *ctx, sqfs_superblock_t *sb,
                       sqfs_inode_t *inode)
{
    if (!ctx || !sb || !inode) {
        return SQFS_ERR_CORRUPT;
    }

    memset(ctx, 0, sizeof(*ctx));

    /* Get block size from superblock */
    ctx->block_size = sb->disk.block_size;

    /* Initialize based on inode type */
    if (!sqfs_inode_is_file(inode->type)) {
        SQFS_LOG_DATA_ERROR("Invalid inode type for file context: %d", inode->type);
        return SQFS_ERR_BAD_INODE;
    }

    /* Copy file information from inode */
    ctx->file_size = inode->file.file_size;
    ctx->blocks_start = inode->file.blocks_start;
    ctx->frag_idx = inode->file.frag_idx;
    ctx->frag_offset = inode->file.block_offset;
    ctx->sparse_bytes = inode->file.sparse;
    ctx->block_sizes = inode->block_sizes;

    /* Calculate block layout */
    calculate_block_layout(ctx, ctx->file_size, ctx->block_size);

    ctx->current_offset = 0;

    return 0;
}

void sqfs_file_ctx_cleanup(sqfs_file_ctx_t *ctx)
{
    if (ctx) {
        /* block_sizes is owned by inode, not by context */
        ctx->block_sizes = NULL;
    }
}

int sqfs_file_block_index(sqfs_file_ctx_t *ctx, uint64_t offset)
{
    if (!ctx) {
        return -1;
    }

    if (offset >= ctx->file_size) {
        return -1;
    }

    return (int)(offset / ctx->block_size);
}

int sqfs_file_block_location(sqfs_file_ctx_t *ctx, uint32_t block_idx,
                             uint64_t *out_pos, uint32_t *out_size,
                             int *out_uncomp)
{
    uint64_t pos;
    uint32_t size;

    if (!ctx || !out_pos || !out_size || !out_uncomp) {
        return SQFS_ERR_CORRUPT;
    }

    /* Check if block index is valid */
    if (block_idx >= ctx->block_count) {
        /* This might be the fragment block */
        if (block_idx == ctx->block_count && !sqfs_fragment_is_none(ctx->frag_idx)) {
            /* Fragment - handled separately */
            return SQFS_ERR_NOT_FOUND;
        }
        return SQFS_ERR_BAD_BLOCK;
    }

    /* Check for sparse block */
    if (ctx->block_sizes[block_idx] == 0) {
        *out_pos = 0;
        *out_size = 0;
        *out_uncomp = 0;
        return 0;
    }

    /* Calculate position by summing sizes of all previous blocks */
    pos = ctx->blocks_start;
    for (uint32_t i = 0; i < block_idx; i++) {
        pos += sqfs_block_size_get(ctx->block_sizes[i]);
    }

    /* Get this block's size */
    size = ctx->block_sizes[block_idx];
    *out_pos = pos;
    *out_size = size;
    *out_uncomp = sqfs_block_is_uncompressed(size);

    return 0;
}

/* ============================================================================
 * Data Block Reading
 * ============================================================================ */

int sqfs_data_read_block(int fd, uint64_t block_pos, uint32_t block_size,
                         void *block_buf, size_t uncomp_size,
                         sqfs_compressor_t *comp)
{
    int ret;
    int is_uncompressed;
    uint32_t actual_size;
    void *read_buf;
    int need_free = 0;

    if (!block_buf || !comp) {
        return SQFS_ERR_CORRUPT;
    }

    /* Handle sparse blocks (zero-filled) */
    if (block_size == 0) {
        memset(block_buf, 0, uncomp_size);
        return (int)uncomp_size;
    }

    /* Extract compression flag and actual size */
    is_uncompressed = sqfs_block_is_uncompressed(block_size);
    actual_size = sqfs_block_size_get(block_size);

    /* Validate size */
    if (actual_size > uncomp_size && !is_uncompressed) {
        /* Compressed size should be less than uncompressed, unless corrupted */
        SQFS_LOG_DATA_WARN("Block size mismatch: actual=%u, expected=%zu",
                 actual_size, uncomp_size);
    }

    /* Read compressed data directly into output buffer if uncompressed */
    if (is_uncompressed) {
        /* Data is stored uncompressed, read directly */
        ret = read_block_from_disk(fd, block_pos, block_buf, actual_size);
        if (ret != 0) {
            return ret;
        }
        return (int)actual_size;
    }

    /* Need to decompress - allocate temporary buffer for compressed data */
    if (actual_size <= uncomp_size) {
        /* Small block - use end of output buffer for compressed data */
        read_buf = (char *)block_buf + uncomp_size - actual_size;
    } else {
        /* Large compressed block - allocate separate buffer */
        read_buf = sqfs_malloc(actual_size);
        if (!read_buf) {
            return SQFS_ERR_NO_MEMORY;
        }
        need_free = 1;
    }

    /* Read compressed data */
    ret = read_block_from_disk(fd, block_pos, read_buf, actual_size);
    if (ret != 0) {
        if (need_free) {
            sqfs_free(read_buf);
        }
        return ret;
    }

    /* Decompress into output buffer */
    size_t out_size;
    ret = sqfs_compressor_decompress(comp, read_buf, actual_size,
                                     block_buf, uncomp_size, &out_size);

    if (need_free) {
        sqfs_free(read_buf);
    }

    if (ret != SQFS_COMP_OK) {
        SQFS_LOG_DATA_ERROR("Decompression failed: %d", ret);
        return SQFS_ERR_CORRUPT;
    }

    return (int)out_size;
}

/* ============================================================================
 * Metadata Block Reading
 * ============================================================================ */

int sqfs_meta_read_block(int fd, uint64_t pos, void *buffer,
                         size_t *out_size, sqfs_compressor_t *comp)
{
    uint16_t header;
    int ret;
    uint16_t compressed_size;
    int is_uncompressed;
    void *read_buf;
    int need_free = 0;

    if (!buffer || !out_size || !comp) {
        SQFS_LOG_DATA_ERROR("sqfs_meta_read_block: invalid params buffer=%p out_size=%p comp=%p",
                 buffer, out_size, comp);
        return SQFS_ERR_CORRUPT;
    }

    SQFS_LOG_DATA_DEBUG("sqfs_meta_read_block: reading at pos 0x%lx", (unsigned long)pos);

    /* Read the 16-bit header */
    ret = read_block_from_disk(fd, pos, &header, sizeof(header));
    if (ret != 0) {
        SQFS_LOG_DATA_ERROR("sqfs_meta_read_block: failed to read header at pos 0x%lx", (unsigned long)pos);
        return ret;
    }

    /* Convert from little-endian */
    header = (uint16_t)(header & 0xFFFF);
    is_uncompressed = (header & SQFS_META_UNCOMPRESSED_FLAG) != 0;
    compressed_size = header & 0x7FFF;

    SQFS_LOG_DATA_DEBUG("sqfs_meta_read_block: header=0x%04x, uncompressed=%d, size=%u",
             header, is_uncompressed, compressed_size);

    /* Validate size */
    if (compressed_size == 0 || compressed_size > SQFS_META_BLOCK_SIZE) {
        SQFS_LOG_DATA_ERROR("Invalid metadata block size: %u", compressed_size);
        return SQFS_ERR_CORRUPT;
    }

    /* Position after header */
    uint64_t data_pos = pos + sizeof(header);

    if (is_uncompressed) {
        /* Read directly into output buffer */
        ret = read_block_from_disk(fd, data_pos, buffer, compressed_size);
        if (ret != 0) {
            return ret;
        }
        *out_size = compressed_size;
        return 0;
    }

    /* Need to decompress */
    if (compressed_size < SQFS_META_BLOCK_SIZE) {
        /* Use end of output buffer for compressed data */
        read_buf = (char *)buffer + SQFS_META_BLOCK_SIZE - compressed_size;
    } else {
        read_buf = sqfs_malloc(compressed_size);
        if (!read_buf) {
            return SQFS_ERR_NO_MEMORY;
        }
        need_free = 1;
    }

    /* Read compressed data */
    ret = read_block_from_disk(fd, data_pos, read_buf, compressed_size);
    if (ret != 0) {
        if (need_free) {
            sqfs_free(read_buf);
        }
        return ret;
    }

    /* Decompress */
    size_t decomp_size;
    ret = sqfs_compressor_decompress(comp, read_buf, compressed_size,
                                     buffer, SQFS_META_BLOCK_SIZE, &decomp_size);

    if (need_free) {
        sqfs_free(read_buf);
    }

    if (ret != SQFS_COMP_OK) {
        SQFS_LOG_DATA_ERROR("Metadata decompression failed: %d", ret);
        return SQFS_ERR_CORRUPT;
    }

    *out_size = decomp_size;
    return 0;
}

int sqfs_meta_read(int fd, uint64_t ref, void *buffer, size_t size,
                   sqfs_compressor_t *comp, uint64_t table_start)
{
    uint64_t block_pos;
    uint16_t block_offset;
    size_t block_size;
    size_t bytes_read;
    size_t bytes_remaining;
    int ret;
    void *block_buf;

    if (!buffer || !comp) {
        return SQFS_ERR_CORRUPT;
    }

    if (size == 0) {
        return 0;
    }

    /* Parse metadata reference */
    block_pos = sqfs_meta_ref_pos(ref) + table_start;
    block_offset = sqfs_meta_ref_offset(ref);

    /* Allocate buffer for a metadata block */
    block_buf = sqfs_malloc(SQFS_META_BLOCK_SIZE);
    if (!block_buf) {
        return SQFS_ERR_NO_MEMORY;
    }

    bytes_remaining = size;
    bytes_read = 0;

    while (bytes_remaining > 0) {
        /* Read current metadata block */
        ret = sqfs_meta_read_block(fd, block_pos, block_buf, &block_size, comp);
        if (ret != 0) {
            sqfs_free(block_buf);
            return ret;
        }

        /* Calculate how much we can copy from this block */
        size_t available = block_size - block_offset;
        size_t to_copy = (bytes_remaining < available) ? bytes_remaining : available;

        /* Copy data to output buffer */
        memcpy((char *)buffer + bytes_read,
               (char *)block_buf + block_offset,
               to_copy);

        bytes_read += to_copy;
        bytes_remaining -= to_copy;

        /* Move to next block */
        if (bytes_remaining > 0) {
            /* Calculate position of next metadata block */
            /* Need to read header to get compressed size */
            uint16_t header;
            ret = read_block_from_disk(fd, block_pos, &header, sizeof(header));
            if (ret != 0) {
                sqfs_free(block_buf);
                return ret;
            }

            uint16_t compressed_size = header & 0x7FFF;

            /* Skip to next block: header + compressed size */
            block_pos += sizeof(header) + compressed_size;
            block_offset = 0;
        }
    }

    sqfs_free(block_buf);
    return 0;
}

/* ============================================================================
 * Main File Reading Function
 * ============================================================================ */

/*
 * Internal context structure for file reading operations.
 * This represents the minimal state needed for streaming reads.
 */
typedef struct {
    int fd;
    sqfs_superblock_t *sb;
    sqfs_compressor_t *comp;
    sqfs_cache_t *data_cache;
    sqfs_fuse_ctx_t *fuse_ctx;  /* Full context for fragment access */
} sqfs_data_ops_t;

/*
 * Read data from a fragment block.
 *
 * Fragments are small tail ends of files stored together in a single block.
 * This function reads the fragment block and extracts the requested data.
 *
 * @param ops       Data operations context
 * @param frag_idx  Fragment index in the fragment table
 * @param frag_offset  Offset within the fragment block
 * @param buffer    Output buffer
 * @param size      Number of bytes to read
 * @param file_ctx  File context for block size information
 *
 * @return Number of bytes read, or negative error code
 */
static int read_from_fragment(sqfs_data_ops_t *ops, uint32_t frag_idx,
                              uint32_t frag_offset, void *buffer, size_t size,
                              sqfs_file_ctx_t *file_ctx)
{
    size_t bytes_read;
    int ret;

    (void)file_ctx;

    if (ops->fuse_ctx == NULL) {
        /* No context available, return zeros */
        memset(buffer, 0, size);
        return (int)size;
    }

    /* Use the fragment module to read data */
    ret = sqfs_fragment_read(ops->fuse_ctx, frag_idx, frag_offset,
                             buffer, size, &bytes_read);
    if (ret != SQFS_OK) {
        /* Error reading fragment - return zeros as fallback */
        memset(buffer, 0, size);
        return (int)size;
    }

    return (int)bytes_read;
}

/*
 * Core data reading implementation.
 * This is the internal version that takes an explicit operations context.
 */
static int sqfs_data_read_internal(sqfs_data_ops_t *ops, sqfs_inode_t *inode,
                                   uint64_t offset, void *buffer, size_t size)
{
    sqfs_file_ctx_t file_ctx;
    int ret;
    size_t bytes_read;
    size_t bytes_remaining;
    char *out_ptr;
    void *block_buf;

    if (!ops || !inode || !buffer) {
        return SQFS_ERR_CORRUPT;
    }

    /* Check inode type */
    if (!sqfs_inode_is_file(inode->type)) {
        SQFS_LOG_DATA_ERROR("Attempted to read from non-file inode");
        return SQFS_ERR_BAD_INODE;
    }

    /* Initialize file context */
    ret = sqfs_file_ctx_init(&file_ctx, ops->sb, inode);
    if (ret != 0) {
        return ret;
    }

    /* Handle reads starting past end of file */
    if (offset >= file_ctx.file_size) {
        sqfs_file_ctx_cleanup(&file_ctx);
        return 0;
    }

    /* Clamp read size to file size */
    if (offset + size > file_ctx.file_size) {
        size = file_ctx.file_size - offset;
    }

    /* Handle zero-length read */
    if (size == 0) {
        sqfs_file_ctx_cleanup(&file_ctx);
        return 0;
    }

    /* Allocate block buffer once for efficiency */
    block_buf = sqfs_malloc(file_ctx.block_size);
    if (!block_buf) {
        sqfs_file_ctx_cleanup(&file_ctx);
        return SQFS_ERR_NO_MEMORY;
    }

    out_ptr = (char *)buffer;
    bytes_remaining = size;
    bytes_read = 0;

    /* Main read loop - process each block touched by the read */
    while (bytes_remaining > 0) {
        uint32_t block_idx;
        uint64_t block_pos;
        uint32_t block_disk_size;
        int is_uncompressed;
        size_t block_offset;
        size_t to_copy;

        /* Calculate which block contains the current offset */
        block_idx = (offset + bytes_read) / file_ctx.block_size;
        block_offset = (offset + bytes_read) % file_ctx.block_size;

        /* Check if this is the tail fragment */
        if (block_idx == file_ctx.block_count && !sqfs_fragment_is_none(file_ctx.frag_idx)) {
            /* Read from fragment block */
            ret = read_from_fragment(ops, file_ctx.frag_idx,
                                    file_ctx.frag_offset + block_offset,
                                    out_ptr + bytes_read, bytes_remaining,
                                    &file_ctx);
            if (ret < 0) {
                sqfs_free(block_buf);
                sqfs_file_ctx_cleanup(&file_ctx);
                return ret;
            }
            bytes_read += ret;
            break;
        }

        /* Get block location */
        ret = sqfs_file_block_location(&file_ctx, block_idx,
                                       &block_pos, &block_disk_size, &is_uncompressed);

        if (ret == SQFS_ERR_NOT_FOUND && block_idx == file_ctx.block_count) {
            /* No fragment, but at end of blocks - should not happen */
            memset(out_ptr + bytes_read, 0, bytes_remaining);
            bytes_read += bytes_remaining;
            break;
        }

        if (ret != 0) {
            sqfs_free(block_buf);
            sqfs_file_ctx_cleanup(&file_ctx);
            return ret;
        }

        /* Check for sparse block */
        if (block_disk_size == 0) {
            /* Sparse block - fill with zeros */
            to_copy = file_ctx.block_size - block_offset;
            if (to_copy > bytes_remaining) {
                to_copy = bytes_remaining;
            }
            memset(out_ptr + bytes_read, 0, to_copy);
            bytes_read += to_copy;
            bytes_remaining -= to_copy;
            continue;
        }

        /*
         * Try to read from cache first.
         * Use block_pos as the cache key (unique per on-disk block).
         */
        sqfs_data_block_entry_t *cached_block = NULL;
        cache_key_t cache_key = sqfs_data_block_cache_key(block_pos);

        if (ops->data_cache != NULL) {
            cached_block = (sqfs_data_block_entry_t *)sqfs_cache_get(
                ops->data_cache, cache_key);
        }

        if (cached_block != NULL && cached_block->is_cached) {
            /* Cache hit - use cached data */
            to_copy = cached_block->data_size - block_offset;
            if (to_copy > bytes_remaining) {
                to_copy = bytes_remaining;
            }
            if (to_copy > 0 && block_offset < cached_block->data_size) {
                memcpy(out_ptr + bytes_read,
                       (char *)cached_block->data + block_offset, to_copy);
                bytes_read += to_copy;
                bytes_remaining -= to_copy;
                continue;
            }
        }

        /* Cache miss - read and decompress block */
        ret = sqfs_data_read_block(ops->fd, block_pos, block_disk_size,
                                   block_buf, file_ctx.block_size, ops->comp);

        if (ret < 0) {
            sqfs_free(block_buf);
            sqfs_file_ctx_cleanup(&file_ctx);
            return ret;
        }

        /* Determine actual data size */
        size_t actual_size = (size_t)ret;

        /* Store in cache for future reads */
        if (ops->data_cache != NULL && actual_size > 0) {
            sqfs_data_block_entry_t *cache_entry = sqfs_data_block_cache_new();
            if (cache_entry != NULL) {
                cache_entry->data = sqfs_malloc(actual_size);
                if (cache_entry->data != NULL) {
                    memcpy(cache_entry->data, block_buf, actual_size);
                    cache_entry->data_size = actual_size;
                    cache_entry->block_pos = block_pos;
                    cache_entry->is_cached = true;

                    /* Put in cache - ignore errors, cache is optional */
                    sqfs_cache_put(ops->data_cache, cache_key, cache_entry,
                                   sizeof(*cache_entry) + actual_size);
                } else {
                    sqfs_data_block_cache_free(cache_entry);
                }
            }
        }

        /* Copy data from block to output buffer */
        to_copy = file_ctx.block_size - block_offset;
        if (to_copy > bytes_remaining) {
            to_copy = bytes_remaining;
        }
        if (to_copy > actual_size - block_offset) {
            to_copy = actual_size - block_offset;
        }
        if (to_copy > 0) {
            memcpy(out_ptr + bytes_read, (char *)block_buf + block_offset, to_copy);
            bytes_read += to_copy;
            bytes_remaining -= to_copy;
        }
    }

    sqfs_free(block_buf);
    sqfs_file_ctx_cleanup(&file_ctx);
    return (int)bytes_read;
}

int sqfs_data_read(sqfs_fuse_ctx_t *fuse_ctx, sqfs_inode_t *inode,
                   uint64_t offset, void *buffer, size_t size)
{
    sqfs_data_ops_t ops;

    if (!fuse_ctx) {
        return SQFS_ERR_CORRUPT;
    }

    /* Initialize ops from fuse_ctx */
    ops.fd = fuse_ctx->sb->fd;
    ops.sb = fuse_ctx->sb;
    ops.comp = fuse_ctx->comp;
    ops.data_cache = &fuse_ctx->data_cache;
    ops.fuse_ctx = fuse_ctx;

    return sqfs_data_read_internal(&ops, inode, offset, buffer, size);
}

/* ============================================================================
 * Sparse File Handling Utilities
 * ============================================================================ */

/*
 * Check if a range of bytes in a file is sparse (all zeros).
 * This is used for sparse file optimization.
 */
int sqfs_data_is_sparse(sqfs_file_ctx_t *ctx, uint64_t offset, size_t size)
{
    uint64_t end_offset;
    uint32_t start_block, end_block;
    uint32_t i;

    if (!ctx || ctx->sparse_bytes == 0) {
        return 0;
    }

    end_offset = offset + size;
    if (end_offset > ctx->file_size) {
        end_offset = ctx->file_size;
    }

    start_block = offset / ctx->block_size;
    end_block = (end_offset - 1) / ctx->block_size;

    for (i = start_block; i <= end_block; i++) {
        if (i >= ctx->block_count) {
            break;
        }

        /* If block has actual data, not sparse */
        if (ctx->block_sizes[i] != 0) {
            return 0;
        }
    }

    return 1;
}

/*
 * Fill a buffer with zeros for sparse regions.
 * Used when reading from sparse file holes.
 */
void sqfs_data_fill_sparse(void *buffer, size_t size)
{
    if (buffer && size > 0) {
        memset(buffer, 0, size);
    }
}

/* ============================================================================
 * Data Block Cache Operations
 * ============================================================================ */

sqfs_data_block_entry_t *sqfs_data_block_cache_new(void)
{
    sqfs_data_block_entry_t *entry;

    entry = (sqfs_data_block_entry_t *)sqfs_calloc(1, sizeof(*entry));
    if (entry == NULL) {
        return NULL;
    }

    entry->block_pos = 0;
    entry->data = NULL;
    entry->data_size = 0;
    entry->is_cached = false;

    return entry;
}

void sqfs_data_block_cache_free(void *ptr)
{
    sqfs_data_block_entry_t *entry = (sqfs_data_block_entry_t *)ptr;

    if (entry != NULL) {
        if (entry->data != NULL) {
            sqfs_free(entry->data);
            entry->data = NULL;
        }
        sqfs_free(entry);
    }
}