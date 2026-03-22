/*
 * SquashFS-FUSE - Fragment Interface
 *
 * Copyright (c) 2024 SquashFS-FUSE Authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file defines the fragment table interface for SquashFS.
 * Fragments are used to store the tail end of small files efficiently.
 *
 * A fragment block contains multiple file tails packed together.
 * The fragment table maps fragment indices to their on-disk locations.
 */

#ifndef SQFS_FRAGMENT_H
#define SQFS_FRAGMENT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "compressor.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Special value indicating no fragment
 */
#define SQFS_FRAGMENT_NONE      0xFFFFFFFF

/*
 * Fragment table entry (16 bytes on disk)
 * Stored in the fragment table, which may be compressed.
 */
typedef struct __attribute__((packed)) {
    uint64_t start;            /* On-disk position of fragment block */
    uint32_t size;             /* Compressed size, bit 24 = uncompressed flag */
    uint32_t unused;           /* Reserved, must be 0 */
} sqfs_frag_entry_t;

/*
 * Fragment cache entry
 * Represents a cached, decompressed fragment block.
 */
typedef struct {
    uint64_t block_start;      /* On-disk position (cache key) */
    void *data;                /* Decompressed data (block_size bytes) */
    size_t data_size;          /* Actual data size */
    bool is_cached;            /* True if data is valid */
} sqfs_fragment_t;

/*
 * Fragment table structure
 * Holds the loaded fragment table entries.
 */
typedef struct {
    sqfs_frag_entry_t *entries;  /* Array of fragment entries */
    uint32_t count;              /* Number of entries */
    bool loaded;                 /* True if table has been loaded */
} sqfs_fragment_table_t;

/*
 * Forward declarations
 */
struct sqfs_ctx;
typedef struct sqfs_ctx sqfs_ctx_t;

/* Backward compatibility */
typedef sqfs_ctx_t sqfs_fuse_ctx_t;

/* ============================================================================
 * Fragment Table Operations
 * ============================================================================ */

/*
 * Initialize a fragment table structure.
 *
 * @param table  Fragment table to initialize
 */
void sqfs_fragment_table_init(sqfs_fragment_table_t *table);

/*
 * Destroy a fragment table and free resources.
 *
 * @param table  Fragment table to destroy
 */
void sqfs_fragment_table_destroy(sqfs_fragment_table_t *table);

/*
 * Load the fragment table from a SquashFS image.
 *
 * The fragment table is stored as a list of 64-bit pointers to
 * metadata blocks containing the fragment entries. Each metadata
 * block contains up to 512 fragment entries (8 KiB / 16 bytes).
 *
 * Parameters:
 *   ctx   - FUSE context with superblock, file descriptor, and compressor
 *
 * Returns:
 *   SQFS_OK on success
 *   SQFS_ERR_CORRUPT if table is corrupted
 *   SQFS_ERR_NO_MEMORY on allocation failure
 *   SQFS_ERR_IO on I/O error
 */
int sqfs_fragment_table_load(sqfs_fuse_ctx_t *ctx);

/*
 * Get a fragment entry by index.
 *
 * Parameters:
 *   ctx    - FUSE context
 *   idx    - Fragment index
 *   entry  - Output: fragment entry
 *
 * Returns:
 *   SQFS_OK on success
 *   SQFS_ERR_NOT_FOUND if index is out of range
 */
int sqfs_fragment_get_entry(sqfs_fuse_ctx_t *ctx, uint32_t idx,
                            sqfs_frag_entry_t *entry);

/*
 * Check if a fragment index is valid.
 *
 * Parameters:
 *   ctx  - FUSE context
 *   idx  - Fragment index to check
 *
 * Returns:
 *   true if index is valid, false otherwise
 */
bool sqfs_fragment_index_valid(sqfs_fuse_ctx_t *ctx, uint32_t idx);

/* ============================================================================
 * Fragment Block Operations
 * ============================================================================ */

/*
 * Read data from a fragment block.
 *
 * This function reads data from a specific fragment. It handles:
 * - Loading and decompressing the fragment block
 * - Caching the decompressed block for reuse
 * - Reading the requested portion of the fragment
 *
 * Parameters:
 *   ctx       - FUSE context with caches and compressor
 *   frag_idx  - Fragment index (0xFFFFFFFF = no fragment)
 *   offset    - Offset within the fragment block
 *   buffer    - Output buffer
 *   size      - Number of bytes to read
 *   read_size - Output: actual bytes read (may be less than size at end)
 *
 * Returns:
 *   SQFS_OK on success
 *   SQFS_ERR_NOT_FOUND if frag_idx is invalid or SQFS_FRAGMENT_NONE
 *   SQFS_ERR_CORRUPT if fragment data is corrupted
 *   SQFS_ERR_NO_MEMORY on allocation failure
 *   SQFS_ERR_IO on I/O error
 */
int sqfs_fragment_read(sqfs_fuse_ctx_t *ctx, uint32_t frag_idx,
                       uint32_t offset, void *buffer, size_t size,
                       size_t *read_size);

/*
 * Read an entire fragment block into a buffer.
 *
 * This is a lower-level function that reads and decompresses
 * an entire fragment block. The caller must provide a buffer
 * large enough to hold block_size bytes.
 *
 * Parameters:
 *   ctx        - FUSE context
 *   frag_idx   - Fragment index
 *   buffer     - Output buffer (must be at least block_size bytes)
 *   out_size   - Output: actual data size in the fragment block
 *
 * Returns:
 *   SQFS_OK on success
 *   SQFS_ERR_NOT_FOUND if frag_idx is invalid
 *   SQFS_ERR_CORRUPT if fragment data is corrupted
 *   SQFS_ERR_NO_MEMORY on allocation failure
 *   SQFS_ERR_IO on I/O error
 */
int sqfs_fragment_read_block(sqfs_fuse_ctx_t *ctx, uint32_t frag_idx,
                             void *buffer, size_t *out_size);

/*
 * Get the on-disk location of a fragment block.
 *
 * Parameters:
 *   ctx        - FUSE context
 *   frag_idx   - Fragment index
 *   out_pos    - Output: on-disk position
 *   out_size   - Output: on-disk size (with uncompressed flag)
 *
 * Returns:
 *   SQFS_OK on success
 *   SQFS_ERR_NOT_FOUND if frag_idx is invalid
 */
int sqfs_fragment_location(sqfs_fuse_ctx_t *ctx, uint32_t frag_idx,
                           uint64_t *out_pos, uint32_t *out_size);

/* ============================================================================
 * Fragment Cache Operations
 * ============================================================================ */

/*
 * Create a new fragment cache entry.
 *
 * @return Pointer to allocated entry, or NULL on failure
 */
sqfs_fragment_t *sqfs_fragment_cache_entry_new(void);

/*
 * Free a fragment cache entry.
 *
 * @param entry  Entry to free
 */
void sqfs_fragment_cache_entry_free(void *entry);

/*
 * Get the cache key for a fragment block.
 *
 * @param frag_idx  Fragment index
 * @return Cache key (block_start from fragment entry)
 */
static inline uint64_t sqfs_fragment_cache_key(uint32_t frag_idx) {
    return (uint64_t)frag_idx;
}

/* ============================================================================
 * Utility Functions
 * ============================================================================ */

/*
 * Check if a fragment index indicates no fragment.
 *
 * @param frag_idx  Fragment index to check
 * @return true if no fragment, false otherwise
 */
static inline bool sqfs_fragment_is_none(uint32_t frag_idx) {
    return frag_idx == SQFS_FRAGMENT_NONE;
}

/*
 * Check if a fragment size indicates uncompressed storage.
 *
 * @param size  Encoded fragment size
 * @return true if uncompressed, false otherwise
 */
static inline bool sqfs_fragment_is_uncompressed(uint32_t size) {
    return (size & (1U << 24)) != 0;
}

/*
 * Get the actual fragment size from encoded size.
 *
 * @param size  Encoded fragment size
 * @return Actual size in bytes
 */
static inline uint32_t sqfs_fragment_size(uint32_t size) {
    return size & 0x00FFFFFF;
}

/*
 * Calculate the number of metadata blocks needed for fragment table entries.
 *
 * Each metadata block can hold up to 512 entries (8 KiB / 16 bytes).
 *
 * @param frag_count  Number of fragment entries
 * @return Number of metadata blocks
 */
static inline uint32_t sqfs_fragment_table_blocks(uint32_t frag_count) {
    return (frag_count + 511) / 512;
}

#ifdef __cplusplus
}
#endif

#endif /* SQFS_FRAGMENT_H */