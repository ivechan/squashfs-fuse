/*
 * SquashFS-FUSE - Data Block Reading Interface
 *
 * Copyright (c) 2024 SquashFS-FUSE Authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file defines the data block reading interface for SquashFS.
 * Handles reading and decompression of file data blocks.
 */

#ifndef SQFS_DATA_H
#define SQFS_DATA_H

#include <stdint.h>
#include <stddef.h>

#include "compressor.h"
#include "superblock.h"
#include "inode.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Data block size constraints (from SquashFS specification)
 */
#define SQFS_BLOCK_SIZE_MIN         4096        /* 4 KiB */
#define SQFS_BLOCK_SIZE_MAX         1048576     /* 1 MiB */
#define SQFS_BLOCK_SIZE_DEFAULT     131072      /* 128 KiB */

/*
 * Metadata block size (fixed at 8 KiB)
 */
#define SQFS_META_BLOCK_SIZE        8192

/*
 * Maximum number of blocks that can be cached
 */
#define SQFS_DATA_CACHE_MAX_BLOCKS  128

/*
 * Fragment none marker
 */
#define SQFS_FRAGMENT_NONE          0xFFFFFFFF

/*
 * Data block descriptor
 * Used to track information about a single data block
 */
typedef struct {
    uint64_t disk_pos;      /* Position on disk (0 if sparse) */
    uint32_t disk_size;     /* On-disk size (compressed or uncompressed) */
    uint32_t uncomp_size;   /* Uncompressed size (typically block_size) */
    int is_uncompressed;    /* True if block is stored uncompressed */
    int is_sparse;          /* True if block is all zeros (sparse file) */
} sqfs_data_block_t;

/*
 * Data block cache entry
 * Represents a cached, decompressed data block.
 */
typedef struct {
    uint64_t block_pos;     /* On-disk position (cache key) */
    void *data;             /* Decompressed data (block_size bytes) */
    size_t data_size;       /* Actual data size */
    bool is_cached;         /* True if data is valid */
} sqfs_data_block_entry_t;

/*
 * Create a new data block cache entry.
 *
 * @return Pointer to allocated entry, or NULL on failure
 */
sqfs_data_block_entry_t *sqfs_data_block_cache_new(void);

/*
 * Free a data block cache entry.
 *
 * @param entry  Entry to free (called by cache as free_fn)
 */
void sqfs_data_block_cache_free(void *entry);

/*
 * Generate cache key for a data block.
 * Uses the on-disk position as a unique identifier.
 *
 * @param block_pos  On-disk position of the block
 * @return Cache key
 */
static inline uint64_t sqfs_data_block_cache_key(uint64_t block_pos) {
    return block_pos;
}

/*
 * File read context
 * Maintains state for streaming file reads
 */
typedef struct {
    uint64_t file_size;         /* Total file size */
    uint64_t current_offset;    /* Current read position */
    uint64_t blocks_start;      /* Position of first data block */
    uint32_t block_size;        /* Block size from superblock */
    uint32_t block_count;       /* Number of full blocks */
    uint32_t tail_size;         /* Size of tail end (fragment or last block) */
    uint32_t frag_idx;          /* Fragment index (0xFFFFFFFF if none) */
    uint32_t frag_offset;       /* Offset within fragment block */
    uint64_t sparse_bytes;      /* Sparse (zero) bytes saved */
    uint32_t *block_sizes;      /* Array of block sizes */
} sqfs_file_ctx_t;

/*
 * Initialize a file read context from an inode
 */
int sqfs_file_ctx_init(sqfs_file_ctx_t *ctx, sqfs_superblock_t *sb,
                       sqfs_inode_t *inode);

/*
 * Clean up a file read context
 */
void sqfs_file_ctx_cleanup(sqfs_file_ctx_t *ctx);

/*
 * Calculate the block index for a given file offset
 */
int sqfs_file_block_index(sqfs_file_ctx_t *ctx, uint64_t offset);

/*
 * Calculate the on-disk position of a data block
 */
int sqfs_file_block_location(sqfs_file_ctx_t *ctx, uint32_t block_idx,
                             uint64_t *out_pos, uint32_t *out_size,
                             int *out_uncomp);

/*
 * Read a data block from disk
 */
int sqfs_data_read_block(int fd, uint64_t block_pos, uint32_t block_size,
                         void *block_buf, size_t uncomp_size,
                         sqfs_compressor_t *comp);

/*
 * Read file data with streaming support
 */
struct sqfs_ctx;
typedef struct sqfs_ctx sqfs_ctx_t;
typedef sqfs_ctx_t sqfs_fuse_ctx_t;  /* Backward compatibility */

int sqfs_data_read(sqfs_ctx_t *ctx, sqfs_inode_t *inode,
                   uint64_t offset, void *buffer, size_t size);

/*
 * Read a metadata block from disk
 */
int sqfs_meta_read_block(int fd, uint64_t pos, void *buffer,
                         size_t *out_size, sqfs_compressor_t *comp);

/*
 * Read metadata that may span block boundaries
 */
int sqfs_meta_read(int fd, uint64_t ref, void *buffer, size_t size,
                   sqfs_compressor_t *comp, uint64_t table_start);

/*
 * Utility functions for metadata references
 */
static inline uint64_t sqfs_meta_ref_pos(uint64_t ref)
{
    return ref >> 16;
}

static inline uint16_t sqfs_meta_ref_offset(uint64_t ref)
{
    return (uint16_t)(ref & 0xFFFF);
}

static inline uint64_t sqfs_meta_ref_make(uint64_t pos, uint16_t offset)
{
    return (pos << 16) | offset;
}

/*
 * Utility functions for block size encoding
 */
static inline uint32_t sqfs_block_size_get(uint32_t size)
{
    return size & SQFS_BLOCK_SIZE_MASK;
}

static inline uint32_t sqfs_block_size_encode(uint32_t size, int uncompressed)
{
    if (uncompressed) {
        return size | SQFS_BLOCK_UNCOMPRESSED_FLAG;
    }
    return size;
}

/*
 * Utility functions for metadata block header
 */
static inline int sqfs_meta_header_is_uncompressed(uint16_t header)
{
    return (header & SQFS_META_UNCOMPRESSED_FLAG) != 0;
}

static inline uint16_t sqfs_meta_header_size(uint16_t header)
{
    return header & 0x7FFF;
}

/*
 * Check if a block is sparse (disk_size == 0)
 */
static inline int sqfs_block_is_sparse(uint32_t disk_size)
{
    return disk_size == 0;
}

#ifdef __cplusplus
}
#endif

#endif /* SQFS_DATA_H */