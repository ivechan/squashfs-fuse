/*
 * SquashFS-FUSE - Extended Attributes (Xattr) Implementation
 *
 * Implements xattr table loading and attribute retrieval.
 */

#include "xattr.h"
#include "utils.h"
#include "superblock.h"
#include "cache.h"
#include "compressor.h"
#include "context.h"

#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

/* ============================================================================
 * Internal Constants
 * ============================================================================ */

#define XATTR_ID_TABLE_HEADER_SIZE  24
#define XATTR_ID_ENTRY_SIZE         16

/* ============================================================================
 * Internal Helper Functions
 * ============================================================================ */

/*
 * Read xattr ID table from disk.
 * The xattr ID table is stored with a header followed by an array of entries.
 */
static int read_xattr_id_table(sqfs_fuse_ctx_t *ctx, sqfs_xattr_table_t *table) {
    sqfs_superblock_t *sb = ctx->sb;
    uint64_t xattr_table_offset = sb->disk.xattr_table;
    int fd = sb->fd;

    /* Read the table header */
    sqfs_xattr_table_header_t header;
    int ret = sqfs_pread_all(fd, &header, sizeof(header), xattr_table_offset);
    if (ret != SQFS_OK) {
        return ret;
    }

    /* Convert from little-endian */
    table->table_start = sqfs_le64_to_cpu(&header.xattr_table_start);
    uint64_t xattr_ids = sqfs_le64_to_cpu(&header.xattr_ids);

    /* Validate */
    SQFS_CHECK(xattr_ids < UINT32_MAX, SQFS_ERR_CORRUPT);

    table->count = (size_t)xattr_ids;

    if (table->count == 0) {
        table->entries = NULL;
        table->loaded = true;
        return SQFS_OK;
    }

    /* Allocate entries array */
    table->entries = sqfs_calloc(table->count, sizeof(sqfs_xattr_id_entry_t));
    if (table->entries == NULL) {
        return SQFS_ERR_NO_MEMORY;
    }

    /* Read entries - they follow the header */
    uint64_t entries_offset = xattr_table_offset + sizeof(sqfs_xattr_table_header_t);

    for (size_t i = 0; i < table->count; i++) {
        sqfs_xattr_id_entry_t entry;
        ret = sqfs_pread_all(fd, &entry, sizeof(entry), entries_offset + i * XATTR_ID_ENTRY_SIZE);
        if (ret != SQFS_OK) {
            sqfs_free(table->entries);
            table->entries = NULL;
            return ret;
        }

        /* Convert from little-endian */
        table->entries[i].xattr_ref = sqfs_le64_to_cpu(&entry.xattr_ref);
        table->entries[i].count = sqfs_le32_to_cpu(&entry.count);
        table->entries[i].size = sqfs_le32_to_cpu(&entry.size);
    }

    table->loaded = true;
    return SQFS_OK;
}

/*
 * Read a metadata block and decompress if needed.
 * Returns the decompressed data in buffer.
 */
static int read_metadata_block_internal(sqfs_fuse_ctx_t *ctx, uint64_t pos,
                                        void *buffer, size_t *out_size) {
    sqfs_superblock_t *sb = ctx->sb;
    int fd = sb->fd;

    /* Read block header (16-bit) */
    uint16_t block_header;
    int ret = sqfs_pread_all(fd, &block_header, sizeof(block_header), pos);
    if (ret != SQFS_OK) {
        return ret;
    }

    block_header = sqfs_le16_to_cpu(&block_header);
    bool uncompressed = (block_header & SQFS_META_UNCOMPRESSED_FLAG) != 0;
    uint16_t compressed_size = block_header & 0x7FFF;

    if (uncompressed) {
        /* Read uncompressed data directly */
        ret = sqfs_pread_all(fd, buffer, compressed_size, pos + sizeof(block_header));
        if (ret != SQFS_OK) {
            return ret;
        }
        *out_size = compressed_size;
    } else {
        /* Read and decompress */
        void *compressed_buf = sqfs_malloc(compressed_size);
        if (compressed_buf == NULL) {
            return SQFS_ERR_NO_MEMORY;
        }

        ret = sqfs_pread_all(fd, compressed_buf, compressed_size, pos + sizeof(block_header));
        if (ret != SQFS_OK) {
            sqfs_free(compressed_buf);
            return ret;
        }

        /* Decompress using context's compressor */
        size_t decompressed_size = 0;
        ret = ctx->comp->decompress(compressed_buf, compressed_size,
                                    buffer, SQFS_META_MAX_SIZE, &decompressed_size);
        sqfs_free(compressed_buf);

        if (ret != SQFS_COMP_OK) {
            return SQFS_ERR_CORRUPT;
        }

        *out_size = decompressed_size;
    }

    return SQFS_OK;
}

/*
 * Read xattr key-value pairs from metadata.
 * Returns an array of sqfs_xattr_t structures.
 */
static int read_xattr_pairs(sqfs_fuse_ctx_t *ctx, uint64_t ref,
                            sqfs_xattr_t **pairs, uint32_t *count) {
    uint64_t block_pos = sqfs_meta_block_pos(ref);
    uint16_t block_offset = sqfs_meta_block_offset(ref);
    uint64_t table_start = ctx->xattr_table->table_start;

    /* Allocate buffer for metadata block */
    void *block_buf = sqfs_malloc(SQFS_META_MAX_SIZE);
    if (block_buf == NULL) {
        return SQFS_ERR_NO_MEMORY;
    }

    /* Read the metadata block */
    size_t block_size;
    int ret = read_metadata_block_internal(ctx, table_start + block_pos,
                                           block_buf, &block_size);
    if (ret != SQFS_OK) {
        sqfs_free(block_buf);
        return ret;
    }

    /* Parse key-value pairs */
    const uint8_t *data = (const uint8_t *)block_buf + block_offset;
    size_t remaining = block_size - block_offset;

    /* First, count the pairs */
    uint32_t pair_count = 0;
    size_t pos = 0;

    while (pos + sizeof(sqfs_xattr_key_t) <= remaining) {
        const sqfs_xattr_key_t *key = (const sqfs_xattr_key_t *)(data + pos);
        uint16_t type = sqfs_le16_to_cpu(&key->type);
        uint16_t name_size = sqfs_le16_to_cpu(&key->name_size);

        /* Check for valid type */
        sqfs_xattr_prefix_t prefix;
        bool is_ool;
        sqfs_xattr_parse_type(type, &prefix, &is_ool);

        if (prefix > SQFS_XATTR_PREFIX_SECURITY) {
            sqfs_free(block_buf);
            return SQFS_ERR_BAD_XATTR;
        }

        pos += sizeof(sqfs_xattr_key_t) + name_size;

        /* Read value size */
        if (pos + sizeof(sqfs_xattr_value_t) > remaining) {
            sqfs_free(block_buf);
            return SQFS_ERR_BAD_XATTR;
        }

        const sqfs_xattr_value_t *val = (const sqfs_xattr_value_t *)(data + pos);
        uint32_t value_size = sqfs_le32_to_cpu(&val->value_size);

        pos += sizeof(sqfs_xattr_value_t) + value_size;
        pair_count++;

        if (pos > remaining) {
            sqfs_free(block_buf);
            return SQFS_ERR_BAD_XATTR;
        }
    }

    if (pair_count == 0) {
        *pairs = NULL;
        *count = 0;
        sqfs_free(block_buf);
        return SQFS_OK;
    }

    /* Allocate pairs array */
    sqfs_xattr_t *result = sqfs_calloc(pair_count, sizeof(sqfs_xattr_t));
    if (result == NULL) {
        sqfs_free(block_buf);
        return SQFS_ERR_NO_MEMORY;
    }

    /* Parse again and fill in the pairs */
    pos = 0;
    uint32_t idx = 0;

    while (idx < pair_count && pos + sizeof(sqfs_xattr_key_t) <= remaining) {
        const sqfs_xattr_key_t *key = (const sqfs_xattr_key_t *)(data + pos);
        uint16_t type = sqfs_le16_to_cpu(&key->type);
        uint16_t name_size = sqfs_le16_to_cpu(&key->name_size);

        sqfs_xattr_prefix_t prefix;
        bool is_ool;
        sqfs_xattr_parse_type(type, &prefix, &is_ool);

        const char *prefix_str = sqfs_xattr_prefix_str(prefix);
        size_t prefix_len = sqfs_xattr_prefix_len(prefix);

        /* Allocate and build full key name */
        size_t full_key_size = prefix_len + name_size + 1;
        result[idx].key = sqfs_malloc(full_key_size);
        if (result[idx].key == NULL) {
            /* Cleanup on error */
            for (uint32_t j = 0; j < idx; j++) {
                sqfs_free(result[j].key);
                sqfs_free(result[j].value);
            }
            sqfs_free(result);
            sqfs_free(block_buf);
            return SQFS_ERR_NO_MEMORY;
        }

        /* Copy prefix and name */
        memcpy(result[idx].key, prefix_str, prefix_len);
        memcpy(result[idx].key + prefix_len, data + pos + sizeof(sqfs_xattr_key_t), name_size);
        result[idx].key[full_key_size - 1] = '\0';

        pos += sizeof(sqfs_xattr_key_t) + name_size;

        /* Read value */
        const sqfs_xattr_value_t *val = (const sqfs_xattr_value_t *)(data + pos);
        uint32_t value_size = sqfs_le32_to_cpu(&val->value_size);

        result[idx].value_size = value_size;
        result[idx].value = sqfs_malloc(value_size);
        if (result[idx].value == NULL) {
            sqfs_free(result[idx].key);
            for (uint32_t j = 0; j < idx; j++) {
                sqfs_free(result[j].key);
                sqfs_free(result[j].value);
            }
            sqfs_free(result);
            sqfs_free(block_buf);
            return SQFS_ERR_NO_MEMORY;
        }

        memcpy(result[idx].value, data + pos + sizeof(sqfs_xattr_value_t), value_size);

        pos += sizeof(sqfs_xattr_value_t) + value_size;
        idx++;
    }

    sqfs_free(block_buf);
    *pairs = result;
    *count = pair_count;
    return SQFS_OK;
}

/*
 * Free an array of xattr pairs.
 */
static void free_xattr_pairs(sqfs_xattr_t *pairs, uint32_t count) {
    if (pairs == NULL) {
        return;
    }

    for (uint32_t i = 0; i < count; i++) {
        sqfs_free(pairs[i].key);
        sqfs_free(pairs[i].value);
    }
    sqfs_free(pairs);
}

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

int sqfs_xattr_table_load(sqfs_fuse_ctx_t *ctx) {
    /* Check if xattrs are disabled */
    if (ctx->sb->disk.flags & SQUASHFS_FLAG_NO_XATTRS) {
        SQFS_LOG("Xattrs disabled in superblock flags");
        return SQFS_ERR_NOT_FOUND;
    }

    /* Check if xattr table exists */
    if (!sqfs_table_valid(ctx->sb->disk.xattr_table)) {
        SQFS_LOG("No xattr table in image");
        return SQFS_ERR_NOT_FOUND;
    }

    /* Allocate xattr table structure */
    ctx->xattr_table = sqfs_calloc(1, sizeof(sqfs_xattr_table_t));
    if (ctx->xattr_table == NULL) {
        return SQFS_ERR_NO_MEMORY;
    }

    /* Read the xattr ID table */
    int ret = read_xattr_id_table(ctx, ctx->xattr_table);
    if (ret != SQFS_OK) {
        sqfs_free(ctx->xattr_table);
        ctx->xattr_table = NULL;
        return ret;
    }

    SQFS_LOG("Loaded xattr table with %zu entries", ctx->xattr_table->count);
    return SQFS_OK;
}

void sqfs_xattr_table_destroy(sqfs_fuse_ctx_t *ctx) {
    if (ctx->xattr_table != NULL) {
        sqfs_free(ctx->xattr_table->entries);
        sqfs_free(ctx->xattr_table);
        ctx->xattr_table = NULL;
    }
}

int sqfs_xattr_get(sqfs_fuse_ctx_t *ctx, uint32_t xattr_idx,
                   const char *name, void *value, size_t size,
                   size_t *out_size) {
    /* Validate parameters */
    SQFS_CHECK(ctx != NULL, SQFS_ERR_CORRUPT);
    SQFS_CHECK(name != NULL, SQFS_ERR_CORRUPT);
    SQFS_CHECK(out_size != NULL, SQFS_ERR_CORRUPT);

    *out_size = 0;

    /* Check if xattr table is loaded */
    if (ctx->xattr_table == NULL || !ctx->xattr_table->loaded) {
        return SQFS_ERR_NOT_FOUND;
    }

    /* Check for invalid index */
    if (xattr_idx == SQFS_XATTR_INVALID_IDX) {
        return SQFS_ERR_NOT_FOUND;
    }

    /* Bounds check */
    if (xattr_idx >= ctx->xattr_table->count) {
        return SQFS_ERR_BAD_XATTR;
    }

    /* Get the xattr ID entry */
    sqfs_xattr_id_entry_t *entry = &ctx->xattr_table->entries[xattr_idx];

    /* Read the xattr pairs */
    sqfs_xattr_t *pairs = NULL;
    uint32_t pair_count = 0;
    int ret = read_xattr_pairs(ctx, entry->xattr_ref, &pairs, &pair_count);
    if (ret != SQFS_OK) {
        return ret;
    }

    /* Search for the requested attribute */
    int found = SQFS_ERR_NOT_FOUND;
    for (uint32_t i = 0; i < pair_count; i++) {
        if (strcmp(pairs[i].key, name) == 0) {
            *out_size = pairs[i].value_size;

            if (value == NULL) {
                /* Caller just wants the size */
                found = SQFS_OK;
            } else if (size < pairs[i].value_size) {
                /* Buffer too small */
                found = SQFS_ERR_OVERFLOW;
            } else {
                /* Copy the value */
                memcpy(value, pairs[i].value, pairs[i].value_size);
                found = SQFS_OK;
            }
            break;
        }
    }

    free_xattr_pairs(pairs, pair_count);
    return found;
}

int sqfs_xattr_list(sqfs_fuse_ctx_t *ctx, uint32_t xattr_idx,
                    char *list, size_t size, size_t *out_size) {
    /* Validate parameters */
    SQFS_CHECK(ctx != NULL, SQFS_ERR_CORRUPT);
    SQFS_CHECK(out_size != NULL, SQFS_ERR_CORRUPT);

    *out_size = 0;

    /* Check if xattr table is loaded */
    if (ctx->xattr_table == NULL || !ctx->xattr_table->loaded) {
        return SQFS_ERR_NOT_FOUND;
    }

    /* Check for invalid index */
    if (xattr_idx == SQFS_XATTR_INVALID_IDX) {
        return SQFS_OK;  /* No xattrs, empty list */
    }

    /* Bounds check */
    if (xattr_idx >= ctx->xattr_table->count) {
        return SQFS_ERR_BAD_XATTR;
    }

    /* Get the xattr ID entry */
    sqfs_xattr_id_entry_t *entry = &ctx->xattr_table->entries[xattr_idx];

    /* Read the xattr pairs */
    sqfs_xattr_t *pairs = NULL;
    uint32_t pair_count = 0;
    int ret = read_xattr_pairs(ctx, entry->xattr_ref, &pairs, &pair_count);
    if (ret != SQFS_OK) {
        return ret;
    }

    /* Calculate total size needed */
    size_t total_size = 0;
    for (uint32_t i = 0; i < pair_count; i++) {
        /* Each name is null-terminated */
        total_size += strlen(pairs[i].key) + 1;
    }

    *out_size = total_size;

    if (list == NULL) {
        /* Caller just wants the size */
        free_xattr_pairs(pairs, pair_count);
        return SQFS_OK;
    }

    if (size < total_size) {
        /* Buffer too small */
        free_xattr_pairs(pairs, pair_count);
        return SQFS_ERR_OVERFLOW;
    }

    /* Copy all names to the list */
    size_t pos = 0;
    for (uint32_t i = 0; i < pair_count; i++) {
        size_t key_len = strlen(pairs[i].key) + 1;
        memcpy(list + pos, pairs[i].key, key_len);
        pos += key_len;
    }

    free_xattr_pairs(pairs, pair_count);
    return SQFS_OK;
}