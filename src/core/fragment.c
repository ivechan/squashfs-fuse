/*
 * SquashFS-FUSE - Fragment Implementation
 *
 * Copyright (c) 2024 SquashFS-FUSE Authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file implements fragment table loading and fragment block reading
 * for SquashFS filesystem.
 *
 * Fragments are used to store the tail ends of small files efficiently.
 * Multiple file tails are packed together into fragment blocks.
 */

#include "fragment.h"
#include "superblock.h"
#include "compressor.h"
#include "cache.h"
#include "data.h"
#include "utils.h"
#include "context.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

/* Number of fragment entries per metadata block (8 KiB / 16 bytes) */
#define FRAGMENT_ENTRIES_PER_BLOCK  512

/* Default fragment cache size */
#define FRAGMENT_CACHE_MAX_ENTRIES   64
#define FRAGMENT_CACHE_MAX_MEMORY    (8 * 1024 * 1024)  /* 8 MiB */

/* Forward declarations for context access */
static int get_context_resources(sqfs_fuse_ctx_t *ctx,
                                  sqfs_superblock_t **sb,
                                  sqfs_compressor_t **comp,
                                  sqfs_cache_t **cache,
                                  sqfs_fragment_table_t **frag_table);

/* ============================================================================
 * Fragment Table Management
 * ============================================================================ */

void sqfs_fragment_table_init(sqfs_fragment_table_t *table)
{
    if (table) {
        memset(table, 0, sizeof(*table));
    }
}

void sqfs_fragment_table_destroy(sqfs_fragment_table_t *table)
{
    if (table) {
        if (table->entries) {
            sqfs_free(table->entries);
            table->entries = NULL;
        }
        table->count = 0;
        table->loaded = false;
    }
}

/*
 * Read the fragment table index.
 * The fragment table is stored as metadata blocks containing sqfs_frag_entry_t
 * entries. The index is an array of 64-bit pointers to these blocks.
 */
static int read_fragment_table_index(int fd, uint64_t table_pos,
                                     uint32_t frag_count,
                                     uint64_t **index_out,
                                     uint32_t *index_count_out,
                                     sqfs_compressor_t *comp)
{
    uint32_t num_blocks;
    uint64_t *index;
    size_t index_size;
    int ret;

    (void)comp;  /* Will be used for decompression */

    if (frag_count == 0) {
        *index_out = NULL;
        *index_count_out = 0;
        return SQFS_OK;
    }

    /* Calculate number of metadata blocks needed */
    num_blocks = (frag_count + FRAGMENT_ENTRIES_PER_BLOCK - 1) /
                 FRAGMENT_ENTRIES_PER_BLOCK;

    /* Index is an array of 64-bit pointers (one per metadata block) */
    index_size = num_blocks * sizeof(uint64_t);

    /* Read the index - it's stored after the last metadata block */
    /* First, we need to read through the metadata blocks to find the index */
    /* Actually, the index is stored at the end of the table */

    /* Allocate buffer for index */
    index = sqfs_malloc(index_size);
    if (!index) {
        return SQFS_ERR_NO_MEMORY;
    }

    /* Read index from disk - it's at table_pos */
    /* The format is: metadata blocks followed by index of block positions */
    /* But actually, we read the index first, then use it to find blocks */

    /* In SquashFS, the table position points to the index (list of 64-bit
     * pointers to metadata blocks). We read the index, then each index entry
     * tells us where to find the metadata block containing fragment entries. */

    /* Read the index entries */
    ret = sqfs_pread_all(fd, index, index_size, table_pos);
    if (ret != SQFS_OK) {
        sqfs_free(index);
        SQFS_LOG("Failed to read fragment table index");
        return ret;
    }

    *index_out = index;
    *index_count_out = num_blocks;
    return SQFS_OK;
}

/*
 * Load fragment entries from metadata blocks.
 */
static int load_fragment_entries(int fd, uint64_t *block_index,
                                 uint32_t num_blocks, uint32_t frag_count,
                                 sqfs_frag_entry_t *entries,
                                 sqfs_compressor_t *comp)
{
    uint32_t entries_read = 0;
    uint32_t i;
    int ret;
    void *block_buf;
    size_t block_size;

    block_buf = sqfs_malloc(SQFS_META_BLOCK_SIZE);
    if (!block_buf) {
        return SQFS_ERR_NO_MEMORY;
    }

    for (i = 0; i < num_blocks; i++) {
        uint64_t block_pos = block_index[i];
        uint32_t entries_in_block;
        size_t copy_size;

        /* Read and decompress metadata block */
        ret = sqfs_meta_read_block(fd, block_pos, block_buf, &block_size, comp);
        if (ret != SQFS_OK) {
            sqfs_free(block_buf);
            SQFS_LOG("Failed to read fragment metadata block %u", i);
            return ret;
        }

        /* Calculate how many entries are in this block */
        entries_in_block = frag_count - entries_read;
        if (entries_in_block > FRAGMENT_ENTRIES_PER_BLOCK) {
            entries_in_block = FRAGMENT_ENTRIES_PER_BLOCK;
        }

        /* Copy entries */
        copy_size = entries_in_block * sizeof(sqfs_frag_entry_t);
        if (copy_size > block_size) {
            sqfs_free(block_buf);
            SQFS_LOG("Fragment metadata block too small");
            return SQFS_ERR_CORRUPT;
        }

        memcpy(&entries[entries_read], block_buf, copy_size);
        entries_read += entries_in_block;
    }

    sqfs_free(block_buf);
    return SQFS_OK;
}

int sqfs_fragment_table_load(sqfs_fuse_ctx_t *ctx)
{
    sqfs_superblock_t *sb;
    sqfs_compressor_t *comp;
    sqfs_fragment_table_t *frag_table;
    uint64_t *block_index = NULL;
    uint32_t num_blocks;
    uint32_t frag_count;
    int ret;

    /* Get context resources */
    ret = get_context_resources(ctx, &sb, &comp, NULL, &frag_table);
    if (ret != SQFS_OK) {
        return ret;
    }

    /* Check if already loaded */
    if (frag_table->loaded) {
        return SQFS_OK;
    }

    /* Check if filesystem has fragments */
    if (!sb->has_fragments) {
        frag_table->entries = NULL;
        frag_table->count = 0;
        frag_table->loaded = true;
        return SQFS_OK;
    }

    frag_count = sb->disk.frag_count;
    if (frag_count == 0) {
        frag_table->entries = NULL;
        frag_table->count = 0;
        frag_table->loaded = true;
        return SQFS_OK;
    }

    /* Allocate entries array */
    frag_table->entries = sqfs_calloc(frag_count, sizeof(sqfs_frag_entry_t));
    if (!frag_table->entries) {
        return SQFS_ERR_NO_MEMORY;
    }

    /* Read the fragment table index */
    ret = read_fragment_table_index(sb->fd, sb->disk.frag_table,
                                     frag_count, &block_index, &num_blocks,
                                     comp);
    if (ret != SQFS_OK) {
        sqfs_free(frag_table->entries);
        frag_table->entries = NULL;
        return ret;
    }

    /* Load entries from metadata blocks */
    if (num_blocks > 0 && block_index) {
        ret = load_fragment_entries(sb->fd, block_index, num_blocks,
                                     frag_count, frag_table->entries, comp);
        sqfs_free(block_index);

        if (ret != SQFS_OK) {
            sqfs_free(frag_table->entries);
            frag_table->entries = NULL;
            return ret;
        }
    }

    frag_table->count = frag_count;
    frag_table->loaded = true;

    SQFS_LOG("Loaded fragment table: %u entries", frag_count);
    return SQFS_OK;
}

int sqfs_fragment_get_entry(sqfs_fuse_ctx_t *ctx, uint32_t idx,
                            sqfs_frag_entry_t *entry)
{
    sqfs_fragment_table_t *frag_table;
    int ret;

    ret = get_context_resources(ctx, NULL, NULL, NULL, &frag_table);
    if (ret != SQFS_OK) {
        return ret;
    }

    if (!frag_table->loaded) {
        SQFS_LOG("Fragment table not loaded");
        return SQFS_ERR_CORRUPT;
    }

    if (idx >= frag_table->count) {
        return SQFS_ERR_NOT_FOUND;
    }

    if (entry) {
        *entry = frag_table->entries[idx];
    }

    return SQFS_OK;
}

bool sqfs_fragment_index_valid(sqfs_fuse_ctx_t *ctx, uint32_t idx)
{
    sqfs_fragment_table_t *frag_table;

    if (get_context_resources(ctx, NULL, NULL, NULL, &frag_table) != SQFS_OK) {
        return false;
    }

    if (!frag_table->loaded) {
        return false;
    }

    return idx < frag_table->count;
}

/* ============================================================================
 * Fragment Block Reading
 * ============================================================================ */

/*
 * Get the on-disk location and size of a fragment block.
 */
int sqfs_fragment_location(sqfs_fuse_ctx_t *ctx, uint32_t frag_idx,
                           uint64_t *out_pos, uint32_t *out_size)
{
    sqfs_frag_entry_t entry;
    int ret;

    if (sqfs_fragment_is_none(frag_idx)) {
        return SQFS_ERR_NOT_FOUND;
    }

    ret = sqfs_fragment_get_entry(ctx, frag_idx, &entry);
    if (ret != SQFS_OK) {
        return ret;
    }

    /* Validate entry */
    if (entry.start == 0 && entry.size == 0) {
        SQFS_LOG("Invalid fragment entry %u: zero start and size", frag_idx);
        return SQFS_ERR_CORRUPT;
    }

    if (out_pos) {
        *out_pos = entry.start;
    }

    if (out_size) {
        *out_size = entry.size;
    }

    return SQFS_OK;
}

/*
 * Read and decompress a fragment block.
 */
int sqfs_fragment_read_block(sqfs_fuse_ctx_t *ctx, uint32_t frag_idx,
                             void *buffer, size_t *out_size)
{
    sqfs_superblock_t *sb;
    sqfs_compressor_t *comp;
    uint64_t block_pos;
    uint32_t block_size;
    uint32_t actual_size;
    int is_uncompressed;
    int ret;
    void *read_buf;
    int need_free = 0;
    size_t block_buf_size;

    if (!buffer || !out_size) {
        return SQFS_ERR_CORRUPT;
    }

    /* Get location */
    ret = sqfs_fragment_location(ctx, frag_idx, &block_pos, &block_size);
    if (ret != SQFS_OK) {
        return ret;
    }

    /* Get context resources */
    ret = get_context_resources(ctx, &sb, &comp, NULL, NULL);
    if (ret != SQFS_OK) {
        return ret;
    }

    /* Extract compression flag and actual size */
    is_uncompressed = sqfs_fragment_is_uncompressed(block_size);
    actual_size = sqfs_fragment_size(block_size);

    /* Block buffer size is the filesystem block size */
    block_buf_size = sb->disk.block_size;

    /* Validate actual size */
    if (actual_size > block_buf_size && !is_uncompressed) {
        SQFS_LOG("Fragment block size %u exceeds block size %zu",
                 actual_size, block_buf_size);
        /* Continue anyway - might be valid */
    }

    if (is_uncompressed) {
        /* Read directly into output buffer */
        ret = sqfs_pread_all(sb->fd, buffer, actual_size, block_pos);
        if (ret != SQFS_OK) {
            SQFS_LOG("Failed to read uncompressed fragment block");
            return ret;
        }
        *out_size = actual_size;
        return SQFS_OK;
    }

    /* Need to decompress */
    if (actual_size < block_buf_size) {
        /* Use end of output buffer for compressed data */
        read_buf = (char *)buffer + block_buf_size - actual_size;
    } else {
        /* Allocate separate buffer */
        read_buf = sqfs_malloc(actual_size);
        if (!read_buf) {
            return SQFS_ERR_NO_MEMORY;
        }
        need_free = 1;
    }

    /* Read compressed data */
    ret = sqfs_pread_all(sb->fd, read_buf, actual_size, block_pos);
    if (ret != SQFS_OK) {
        if (need_free) {
            sqfs_free(read_buf);
        }
        SQFS_LOG("Failed to read compressed fragment block");
        return ret;
    }

    /* Decompress */
    ret = sqfs_compressor_decompress(comp, read_buf, actual_size,
                                     buffer, block_buf_size, out_size);

    if (need_free) {
        sqfs_free(read_buf);
    }

    if (ret != SQFS_COMP_OK) {
        SQFS_LOG("Fragment block decompression failed: %d", ret);
        return SQFS_ERR_CORRUPT;
    }

    return SQFS_OK;
}

/*
 * Read data from a fragment block with caching.
 */
int sqfs_fragment_read(sqfs_fuse_ctx_t *ctx, uint32_t frag_idx,
                       uint32_t offset, void *buffer, size_t size,
                       size_t *read_size)
{
    sqfs_superblock_t *sb;
    sqfs_cache_t *data_cache;
    sqfs_fragment_t *frag = NULL;
    size_t block_size;
    size_t available;
    size_t to_copy;
    int ret;
    cache_key_t cache_key;
    bool need_free_frag = false;

    if (!buffer || !read_size) {
        return SQFS_ERR_CORRUPT;
    }

    *read_size = 0;

    /* Handle no-fragment case */
    if (sqfs_fragment_is_none(frag_idx)) {
        return SQFS_ERR_NOT_FOUND;
    }

    /* Get context resources */
    ret = get_context_resources(ctx, &sb, NULL, &data_cache, NULL);
    if (ret != SQFS_OK) {
        return ret;
    }

    block_size = sb->disk.block_size;

    /* Validate offset */
    if (offset >= block_size) {
        SQFS_LOG("Fragment offset %u exceeds block size %zu", offset, block_size);
        return SQFS_ERR_CORRUPT;
    }

    /* Try to get from cache */
    cache_key = sqfs_fragment_cache_key(frag_idx);

    if (data_cache) {
        frag = (sqfs_fragment_t *)sqfs_cache_get(data_cache, cache_key);
    }

    if (frag && frag->is_cached) {
        /* Cache hit - use cached data */
        available = frag->data_size;
        if (offset > available) {
            /* Offset beyond data - return zeros */
            to_copy = (size < block_size - offset) ? size : (block_size - offset);
            memset(buffer, 0, to_copy);
            *read_size = to_copy;
            return SQFS_OK;
        }

        to_copy = available - offset;
        if (to_copy > size) {
            to_copy = size;
        }

        memcpy(buffer, (char *)frag->data + offset, to_copy);
        *read_size = to_copy;
        return SQFS_OK;
    }

    /* Cache miss - need to read and decompress */
    frag = sqfs_fragment_cache_entry_new();
    if (!frag) {
        return SQFS_ERR_NO_MEMORY;
    }
    need_free_frag = true;

    frag->data = sqfs_malloc(block_size);
    if (!frag->data) {
        sqfs_fragment_cache_entry_free(frag);
        return SQFS_ERR_NO_MEMORY;
    }

    /* Read the fragment block */
    ret = sqfs_fragment_read_block(ctx, frag_idx, frag->data, &frag->data_size);
    if (ret != SQFS_OK) {
        sqfs_fragment_cache_entry_free(frag);
        return ret;
    }

    frag->is_cached = true;

    /* Store in cache */
    if (data_cache) {
        ret = sqfs_cache_put(data_cache, cache_key, frag,
                             sizeof(sqfs_fragment_t) + block_size);
        if (ret == 0) {
            /* Cache took ownership */
            need_free_frag = false;
        }
    }

    /* Copy data to output buffer */
    available = frag->data_size;
    if (offset > available) {
        to_copy = (size < block_size - offset) ? size : (block_size - offset);
        memset(buffer, 0, to_copy);
        *read_size = to_copy;
    } else {
        to_copy = available - offset;
        if (to_copy > size) {
            to_copy = size;
        }

        memcpy(buffer, (char *)frag->data + offset, to_copy);
        *read_size = to_copy;
    }

    if (need_free_frag) {
        sqfs_fragment_cache_entry_free(frag);
    }

    return SQFS_OK;
}

/* ============================================================================
 * Fragment Cache Entry Management
 * ============================================================================ */

sqfs_fragment_t *sqfs_fragment_cache_entry_new(void)
{
    sqfs_fragment_t *entry;

    entry = (sqfs_fragment_t *)sqfs_calloc(1, sizeof(sqfs_fragment_t));
    if (!entry) {
        return NULL;
    }

    entry->block_start = 0;
    entry->data = NULL;
    entry->data_size = 0;
    entry->is_cached = false;

    return entry;
}

void sqfs_fragment_cache_entry_free(void *ptr)
{
    sqfs_fragment_t *entry = (sqfs_fragment_t *)ptr;

    if (entry) {
        if (entry->data) {
            sqfs_free(entry->data);
            entry->data = NULL;
        }
        sqfs_free(entry);
    }
}

/* ============================================================================
 * Internal Helper: Context Resource Access
 * ============================================================================ */

/*
 * Get resources from the FUSE context.
 */
static int get_context_resources(sqfs_fuse_ctx_t *ctx,
                                  sqfs_superblock_t **sb,
                                  sqfs_compressor_t **comp,
                                  sqfs_cache_t **cache,
                                  sqfs_fragment_table_t **frag_table)
{
    if (!ctx) {
        return SQFS_ERR_CORRUPT;
    }

    if (sb) {
        *sb = ctx->sb;
    }

    if (comp) {
        *comp = ctx->comp;
    }

    if (cache) {
        *cache = &ctx->data_cache;
    }

    if (frag_table) {
        *frag_table = ctx->fragment_table;
    }

    return SQFS_OK;
}

/* ============================================================================
 * Debugging Utilities
 * ============================================================================ */

/* ============================================================================
 * Debugging Utilities
 * ============================================================================ */

#ifdef SQFS_DEBUG
/*
 * Print fragment table information (for debugging).
 */
void sqfs_fragment_table_print(const sqfs_fragment_table_t *table)
{
    uint32_t i;

    if (!table) {
        printf("Fragment table: NULL\n");
        return;
    }

    printf("=== Fragment Table ===\n");
    printf("Loaded: %s\n", table->loaded ? "yes" : "no");
    printf("Count:  %u\n", table->count);

    if (table->entries && table->count > 0) {
        printf("\nEntries:\n");
        for (i = 0; i < table->count && i < 10; i++) {
            printf("  [%u] pos=0x%016lx size=%u (%s)\n",
                   i,
                   (unsigned long)table->entries[i].start,
                   sqfs_fragment_size(table->entries[i].size),
                   sqfs_fragment_is_uncompressed(table->entries[i].size) ?
                       "uncompressed" : "compressed");
        }
        if (table->count > 10) {
            printf("  ... (%u more entries)\n", table->count - 10);
        }
    }
}
#endif /* SQFS_DEBUG */