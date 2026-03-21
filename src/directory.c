/*
 * SquashFS-FUSE - Directory Table Implementation
 *
 * Copyright (c) 2024 SquashFS-FUSE Authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file implements directory table parsing for SquashFS.
 */

#define _POSIX_C_SOURCE 200809L
#include "directory.h"
#include "utils.h"
#include "superblock.h"
#include "compressor.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/*
 * FUSE context structure
 * This matches the definition in main.c
 */
struct sqfs_fuse_ctx {
    sqfs_superblock_t    *sb;           /* Superblock */
    sqfs_compressor_t    *comp;         /* Compressor instance */
    /* Other fields not needed by directory operations */
};

/*
 * Metadata block constants
 */
#define SQFS_META_BLOCK_SIZE    8192

/*
 * Read a metadata block header
 *
 * @param fd        File descriptor
 * @param pos       Position of the block (including header)
 * @param header    Output: 16-bit header value
 *
 * @return 0 on success, negative error code on failure
 */
static int read_meta_header(int fd, uint64_t pos, uint16_t *header)
{
    return sqfs_pread_all(fd, header, sizeof(*header), pos);
}

/*
 * Decompress a metadata block
 *
 * @param fd            File descriptor
 * @param pos           Position of block data (after header)
 * @param comp_size     Compressed size (from header)
 * @param is_uncomp     True if block is uncompressed
 * @param buffer        Output buffer (must be SQFS_META_BLOCK_SIZE bytes)
 * @param out_size      Output: actual uncompressed size
 * @param comp          Compressor instance
 *
 * @return 0 on success, negative error code on failure
 */
static int decompress_meta_block(int fd, uint64_t pos, uint16_t comp_size,
                                  int is_uncomp, void *buffer, size_t *out_size,
                                  sqfs_compressor_t *comp)
{
    int ret;

    if (is_uncomp) {
        /* Block is uncompressed, read directly */
        ret = sqfs_pread_all(fd, buffer, comp_size, pos);
        if (ret != 0) {
            return ret;
        }
        *out_size = comp_size;
    } else {
        /* Block is compressed */
        void *comp_buf = sqfs_malloc(comp_size);
        if (comp_buf == NULL) {
            return SQFS_ERR_NO_MEMORY;
        }

        ret = sqfs_pread_all(fd, comp_buf, comp_size, pos);
        if (ret != 0) {
            sqfs_free(comp_buf);
            return ret;
        }

        ret = sqfs_compressor_decompress(comp, comp_buf, comp_size,
                                          buffer, SQFS_META_BLOCK_SIZE, out_size);
        sqfs_free(comp_buf);

        if (ret != SQFS_COMP_OK) {
            SQFS_LOG("Failed to decompress metadata block");
            return SQFS_ERR_CORRUPT;
        }
    }

    return 0;
}

/*
 * Read a single metadata block
 *
 * @param fd            File descriptor
 * @param pos           Position of block (including header)
 * @param buffer        Output buffer (must be SQFS_META_BLOCK_SIZE bytes)
 * @param out_size      Output: uncompressed size
 * @param comp          Compressor instance
 * @param next_pos      Output: position of next block (may be NULL)
 *
 * @return 0 on success, negative error code on failure
 */
static int read_meta_block(int fd, uint64_t pos, void *buffer, size_t *out_size,
                           sqfs_compressor_t *comp, uint64_t *next_pos)
{
    uint16_t header;
    int ret;
    uint16_t comp_size;
    int is_uncomp;

    /* Read block header */
    ret = read_meta_header(fd, pos, &header);
    if (ret != 0) {
        return ret;
    }

    /* Parse header */
    is_uncomp = (header & 0x8000) != 0;
    comp_size = header & 0x7FFF;

    /* Read and decompress block */
    ret = decompress_meta_block(fd, pos + 2, comp_size, is_uncomp,
                                 buffer, out_size, comp);
    if (ret != 0) {
        return ret;
    }

    /* Calculate next block position */
    if (next_pos != NULL) {
        *next_pos = pos + 2 + comp_size;
    }

    return 0;
}

/*
 * Read directory data from the directory table
 *
 * Directory data is stored in metadata blocks (8 KiB uncompressed).
 * This function reads the raw directory data into a buffer.
 *
 * @param fd            File descriptor
 * @param dir_table     Directory table start position
 * @param comp          Compressor
 * @param block_idx     Byte offset within directory table
 * @param block_offset  Offset within the first block
 * @param size          Size of directory data
 * @param buffer        Output buffer (must be at least size bytes)
 *
 * @return 0 on success, negative error code on failure
 */
static int read_directory_data(int fd, uint64_t dir_table,
                                sqfs_compressor_t *comp,
                                uint64_t block_idx, uint16_t block_offset,
                                uint32_t size, void *buffer)
{
    uint64_t block_pos;
    uint8_t block_buf[SQFS_META_BLOCK_SIZE];
    size_t block_data_size;
    int ret;
    size_t copied;

    /* Calculate position of first metadata block */
    block_pos = dir_table + block_idx;

    /* Read the first metadata block */
    ret = read_meta_block(fd, block_pos, block_buf, &block_data_size, comp, &block_pos);
    if (ret != 0) {
        SQFS_LOG("Failed to read directory metadata block");
        return ret;
    }

    /* Check block offset is valid */
    if (block_offset >= block_data_size) {
        SQFS_LOG("Invalid block offset %u (block size=%zu)",
                 block_offset, block_data_size);
        return SQFS_ERR_BAD_DIR;
    }

    /* Check if the directory data fits in a single block */
    if ((size_t)block_offset + size <= block_data_size) {
        memcpy(buffer, block_buf + block_offset, size);
        return 0;
    }

    /* Directory spans multiple blocks */
    copied = block_data_size - block_offset;
    memcpy(buffer, block_buf + block_offset, copied);

    /* Read remaining blocks */
    while (copied < size) {
        ret = read_meta_block(fd, block_pos, block_buf, &block_data_size, comp, &block_pos);
        if (ret != 0) {
            SQFS_LOG("Failed to read directory metadata block");
            return ret;
        }

        /* Copy remaining data */
        size_t to_copy = size - copied;
        if (to_copy > block_data_size) {
            to_copy = block_data_size;
        }
        memcpy((char *)buffer + copied, block_buf, to_copy);
        copied += to_copy;
    }

    return 0;
}

/*
 * Parse directory entries from raw directory data
 *
 * @param data          Raw directory data
 * @param size          Size of directory data
 * @param entries       Output: array of parsed entries
 * @param count         Output: number of entries
 *
 * @return 0 on success, negative error code on failure
 */
static int parse_directory_entries(const void *data, uint32_t size,
                                    sqfs_dirent_t **entries, size_t *count)
{
    const uint8_t *ptr = (const uint8_t *)data;
    const uint8_t *end = ptr + size;
    sqfs_dirent_t *result = NULL;
    size_t capacity = 0;
    size_t num_entries = 0;
    int ret = 0;

    while (ptr < end) {
        /* Check we have enough data for the header */
        if ((size_t)(end - ptr) < sizeof(sqfs_dir_header_t)) {
            SQFS_LOG("Directory data truncated at header");
            ret = SQFS_ERR_BAD_DIR;
            goto error;
        }

        /* Read directory header (little-endian) */
        sqfs_dir_header_t header;
        header.count = sqfs_le32_to_cpu(ptr);
        header.start = sqfs_le32_to_cpu(ptr + 4);
        header.inode_number = sqfs_le32_to_cpu(ptr + 8);

        uint32_t entry_count = header.count + 1;  /* count is entries - 1 */

        ptr += sizeof(sqfs_dir_header_t);

        /* Process each entry */
        for (uint32_t i = 0; i < entry_count; i++) {
            /* Check we have enough data for the entry */
            if ((size_t)(end - ptr) < sizeof(sqfs_dir_entry_t)) {
                SQFS_LOG("Directory data truncated at entry %u", i);
                ret = SQFS_ERR_BAD_DIR;
                goto error;
            }

            /* Read directory entry (little-endian) */
            sqfs_dir_entry_t entry;
            entry.offset = sqfs_le16_to_cpu(ptr);
            entry.inode_offset = (int16_t)sqfs_le16_to_cpu(ptr + 2);
            entry.type = sqfs_le16_to_cpu(ptr + 4);
            entry.name_size = sqfs_le16_to_cpu(ptr + 6);

            uint16_t name_len = entry.name_size + 1;  /* name_size is length - 1 */

            /* Check we have enough data for the name */
            if ((size_t)(end - ptr) < sizeof(sqfs_dir_entry_t) + name_len) {
                SQFS_LOG("Directory data truncated at entry name");
                ret = SQFS_ERR_BAD_DIR;
                goto error;
            }

            /* Validate inode type (must be a basic type: 1-7) */
            if (entry.type < 1 || entry.type > 7) {
                SQFS_LOG("Invalid inode type %u in directory entry", entry.type);
                ret = SQFS_ERR_BAD_DIR;
                goto error;
            }

            /* Grow array if needed */
            if (num_entries >= capacity) {
                size_t new_capacity = capacity == 0 ? 16 : capacity * 2;
                sqfs_dirent_t *new_result = sqfs_realloc(result,
                    new_capacity * sizeof(sqfs_dirent_t));
                if (new_result == NULL) {
                    ret = SQFS_ERR_NO_MEMORY;
                    goto error;
                }
                result = new_result;
                capacity = new_capacity;
            }

            /* Fill in the entry */
            sqfs_dirent_t *dirent = &result[num_entries];

            /* Allocate and copy name */
            dirent->name = sqfs_malloc(name_len + 1);  /* +1 for null terminator */
            if (dirent->name == NULL) {
                ret = SQFS_ERR_NO_MEMORY;
                goto error;
            }
            memcpy(dirent->name, ptr + sizeof(sqfs_dir_entry_t), name_len);
            dirent->name[name_len] = '\0';

            /* Calculate absolute inode number */
            /* inode_offset is signed, so we do signed arithmetic then cast */
            dirent->inode_number = (uint64_t)((int64_t)(int32_t)header.inode_number +
                                               (int64_t)entry.inode_offset);

            /* Calculate inode reference from header.start and entry.offset */
            /* Reference format: (block_pos << 16) | offset */
            dirent->inode_ref = ((uint64_t)header.start << 16) | entry.offset;

            /* Map basic type to inode type enum */
            dirent->type = (sqfs_inode_type_t)entry.type;

            num_entries++;
            ptr += sizeof(sqfs_dir_entry_t) + name_len;
        }
    }

    *entries = result;
    *count = num_entries;
    return 0;

error:
    if (result != NULL) {
        for (size_t i = 0; i < num_entries; i++) {
            sqfs_free(result[i].name);
        }
        sqfs_free(result);
    }
    return ret;
}

int sqfs_dir_read(sqfs_fuse_ctx_t *ctx, sqfs_inode_t *dir_inode,
                  sqfs_dirent_t **entries, size_t *count)
{
    void *dir_data = NULL;
    int ret;
    uint32_t dir_size;
    uint64_t block_idx;
    uint16_t block_offset;

    if (ctx == NULL || dir_inode == NULL || entries == NULL || count == NULL) {
        return SQFS_ERR_CORRUPT;
    }

    /* Validate inode is a directory */
    if (!sqfs_inode_is_dir(dir_inode->type)) {
        SQFS_LOG("Inode is not a directory (type=%d)", dir_inode->type);
        return SQFS_ERR_BAD_INODE;
    }

    /* Get directory metadata from inode */
    dir_size = dir_inode->dir.size;
    block_idx = dir_inode->dir.block_idx;
    block_offset = dir_inode->dir.block_offset;

    /* Sanity check directory size */
    if (dir_size == 0) {
        /* Empty directory */
        *entries = NULL;
        *count = 0;
        return 0;
    }

    /* Allocate buffer for directory data */
    dir_data = sqfs_malloc(dir_size);
    if (dir_data == NULL) {
        return SQFS_ERR_NO_MEMORY;
    }

    /* Read directory data from disk */
    ret = read_directory_data(ctx->sb->fd, ctx->sb->disk.dir_table,
                               ctx->comp, block_idx, block_offset,
                               dir_size, dir_data);
    if (ret != 0) {
        sqfs_free(dir_data);
        return ret;
    }

    /* Parse directory entries */
    ret = parse_directory_entries(dir_data, dir_size, entries, count);

    sqfs_free(dir_data);
    return ret;
}

void sqfs_dirent_free(sqfs_dirent_t *entries, size_t count)
{
    if (entries == NULL) {
        return;
    }

    for (size_t i = 0; i < count; i++) {
        sqfs_free(entries[i].name);
    }
    sqfs_free(entries);
}

sqfs_dirent_t *sqfs_dirent_lookup(sqfs_dirent_t *entries, size_t count,
                                   const char *name)
{
    if (entries == NULL || name == NULL) {
        return NULL;
    }

    for (size_t i = 0; i < count; i++) {
        if (strcmp(entries[i].name, name) == 0) {
            return &entries[i];
        }
    }

    return NULL;
}

void sqfs_directory_print(const sqfs_dirent_t *entries, size_t count)
{
    if (entries == NULL) {
        printf("Directory: (null)\n");
        return;
    }

    printf("Directory entries (%zu):\n", count);
    for (size_t i = 0; i < count; i++) {
        const char *type_str;
        switch (entries[i].type) {
            case SQFS_INODE_DIR:     type_str = "dir"; break;
            case SQFS_INODE_FILE:    type_str = "file"; break;
            case SQFS_INODE_SYMLINK: type_str = "symlink"; break;
            case SQFS_INODE_BLKDEV:  type_str = "blkdev"; break;
            case SQFS_INODE_CHRDEV:  type_str = "chrdev"; break;
            case SQFS_INODE_FIFO:    type_str = "fifo"; break;
            case SQFS_INODE_SOCKET:  type_str = "socket"; break;
            default:                 type_str = "unknown"; break;
        }
        printf("  [%zu] %s (inode=%lu, type=%s)\n",
               i, entries[i].name,
               (unsigned long)entries[i].inode_number, type_str);
    }
}