/*
 * SquashFS-FUSE - Inode Implementation
 *
 * Inode parsing and management functions.
 *
 * Copyright (C) 2024
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define _POSIX_C_SOURCE 200809L

#include "inode.h"
#include "superblock.h"
#include "utils.h"
#include "cache.h"
#include "compressor.h"
#include "data.h"
#include "context.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ============================================================================
 * Internal Constants
 * ============================================================================ */

/* Invalid xattr index (indicates no xattrs) */
#define XATTR_INVALID 0xFFFFFFFF

/* Invalid fragment index (indicates no fragment) */
#define FRAGMENT_INVALID 0xFFFFFFFF

/* Maximum inode size (for buffer allocation) */
/* Large files can have many block sizes, e.g., 100MB file = 800 blocks = 3200+ bytes */
#define MAX_INODE_SIZE 8192

/* ============================================================================
 * Internal Helpers
 * ============================================================================ */

/*
 * Read raw inode data from metadata block.
 */
static int read_inode_metadata(sqfs_fuse_ctx_t *ctx, uint64_t ref,
                               void *buffer, size_t size)
{
    uint64_t block_pos = sqfs_meta_ref_pos(ref);
    uint16_t block_offset = sqfs_meta_ref_offset(ref);

    /* Inode metadata references are relative to inode_table start */
    uint64_t abs_pos = ctx->sb->disk.inode_table + block_pos;

    SQFS_LOG_INODE_DEBUG("read_inode_metadata: ref=0x%lx, block_pos=0x%lx, inode_table=0x%lx, abs_pos=0x%lx",
                         (unsigned long)ref, (unsigned long)block_pos,
                         (unsigned long)ctx->sb->disk.inode_table, (unsigned long)abs_pos);

    /* Read metadata block */
    uint8_t meta_block[SQFS_META_BLOCK_SIZE];
    size_t out_size;
    int ret;

    ret = sqfs_meta_read_block(ctx->sb->fd, abs_pos, meta_block, &out_size, ctx->comp);
    if (ret != SQFS_OK) {
        SQFS_LOG_INODE_ERROR("read_inode_metadata: sqfs_meta_read_block failed at pos 0x%lx, ret=%d",
                             (unsigned long)abs_pos, ret);
        return ret;
    }

    SQFS_LOG_INODE_DEBUG("read_inode_metadata: read %zu bytes, block_offset=%u",
                         out_size, block_offset);

    /* Check bounds */
    if (block_offset + size > out_size) {
        SQFS_LOG_INODE_ERROR("read_inode_metadata: bounds check failed, offset=%u + size=%zu > out_size=%zu",
                             block_offset, size, out_size);
        return SQFS_ERR_BAD_INODE;
    }

    /* Copy inode data */
    memcpy(buffer, meta_block + block_offset, size);

    return SQFS_OK;
}

/*
 * Parse inode header from raw data.
 * All fields are little-endian.
 */
static void parse_inode_header(const void *data, sqfs_inode_header_t *header)
{
    const uint8_t *p = (const uint8_t *)data;

    header->type = sqfs_le16_to_cpu(p);
    header->permissions = sqfs_le16_to_cpu(p + 2);
    header->uid_idx = sqfs_le16_to_cpu(p + 4);
    header->gid_idx = sqfs_le16_to_cpu(p + 6);
    header->mtime = sqfs_le32_to_cpu(p + 8);
    header->inode_number = sqfs_le32_to_cpu(p + 12);
}

/*
 * Parse basic directory inode.
 */
static int parse_inode_dir(const void *data, sqfs_inode_t *inode)
{
    const uint8_t *p = (const uint8_t *)data + sizeof(sqfs_inode_header_t);

    inode->dir.block_idx = sqfs_le32_to_cpu(p);
    inode->dir.block_idx &= 0xFFFFFFFF;  /* Ensure 32-bit value */
    inode->link_count = sqfs_le32_to_cpu(p + 4);
    inode->dir.size = sqfs_le16_to_cpu(p + 8);
    /* file_size in disk format is actual size + 3 */
    if (inode->dir.size >= 3) {
        inode->dir.size -= 3;
    }
    inode->dir.block_offset = sqfs_le16_to_cpu(p + 10);
    inode->dir.parent_inode = sqfs_le32_to_cpu(p + 12);
    inode->dir.index_count = 0;  /* Basic directories don't have index */

    return SQFS_OK;
}

/*
 * Parse extended directory inode.
 */
static int parse_inode_ldir(const void *data, sqfs_inode_t *inode)
{
    const uint8_t *p = (const uint8_t *)data + sizeof(sqfs_inode_header_t);

    inode->link_count = sqfs_le32_to_cpu(p);
    inode->dir.size = sqfs_le32_to_cpu(p + 4);
    inode->dir.block_idx = sqfs_le32_to_cpu(p + 8);
    inode->dir.parent_inode = sqfs_le32_to_cpu(p + 12);
    inode->dir.index_count = sqfs_le16_to_cpu(p + 16);
    inode->dir.block_offset = sqfs_le16_to_cpu(p + 18);
    inode->xattr_idx = sqfs_le32_to_cpu(p + 20);

    return SQFS_OK;
}

/*
 * Parse basic file inode.
 * Returns the number of bytes consumed from the data buffer.
 */
static int parse_inode_file(const void *data, sqfs_inode_t *inode,
                            uint32_t block_size, size_t *bytes_consumed)
{
    const uint8_t *p = (const uint8_t *)data + sizeof(sqfs_inode_header_t);
    uint32_t file_size;
    uint32_t frag_idx;
    uint32_t block_count;
    size_t header_size = 16;  /* Fixed part after common header */

    inode->file.blocks_start = sqfs_le32_to_cpu(p);
    frag_idx = sqfs_le32_to_cpu(p + 4);
    inode->file.frag_idx = frag_idx;
    inode->file.block_offset = sqfs_le32_to_cpu(p + 8);
    file_size = sqfs_le32_to_cpu(p + 12);
    inode->file.file_size = file_size;
    inode->file.sparse = 0;
    inode->file.xattr_idx = XATTR_INVALID;

    /* Basic file inodes don't store link_count, default to 1 */
    inode->link_count = 1;

    /* Calculate number of blocks */
    bool has_fragment = (frag_idx != FRAGMENT_INVALID);
    block_count = sqfs_calc_block_count(file_size, block_size, has_fragment);
    inode->file.block_count = block_count;

    /* Allocate and read block sizes array */
    if (block_count > 0) {
        inode->block_sizes = sqfs_calloc(block_count, sizeof(uint32_t));
        if (inode->block_sizes == NULL) {
            return SQFS_ERR_NO_MEMORY;
        }

        const uint8_t *block_sizes_data = p + header_size;
        for (uint32_t i = 0; i < block_count; i++) {
            inode->block_sizes[i] = sqfs_le32_to_cpu(block_sizes_data + i * 4);
        }

        *bytes_consumed = sizeof(sqfs_inode_header_t) + header_size +
                          block_count * sizeof(uint32_t);
    } else {
        inode->block_sizes = NULL;
        *bytes_consumed = sizeof(sqfs_inode_header_t) + header_size;
    }

    return SQFS_OK;
}

/*
 * Parse extended file inode.
 * Returns the number of bytes consumed from the data buffer.
 */
static int parse_inode_lfile(const void *data, sqfs_inode_t *inode,
                             uint32_t block_size, size_t *bytes_consumed)
{
    const uint8_t *p = (const uint8_t *)data + sizeof(sqfs_inode_header_t);
    uint64_t file_size;
    uint32_t frag_idx;
    uint32_t block_count;
    size_t header_size = 40;  /* Fixed part after common header */

    inode->file.blocks_start = sqfs_le64_to_cpu(p);
    file_size = sqfs_le64_to_cpu(p + 8);
    inode->file.file_size = file_size;
    inode->file.sparse = sqfs_le64_to_cpu(p + 16);
    inode->link_count = sqfs_le32_to_cpu(p + 24);
    frag_idx = sqfs_le32_to_cpu(p + 28);
    inode->file.frag_idx = frag_idx;
    inode->file.block_offset = sqfs_le32_to_cpu(p + 32);
    inode->file.xattr_idx = sqfs_le32_to_cpu(p + 36);

    /* Calculate number of blocks */
    bool has_fragment = (frag_idx != FRAGMENT_INVALID);
    block_count = sqfs_calc_block_count(file_size, block_size, has_fragment);
    inode->file.block_count = block_count;

    /* Allocate and read block sizes array */
    if (block_count > 0) {
        inode->block_sizes = sqfs_calloc(block_count, sizeof(uint32_t));
        if (inode->block_sizes == NULL) {
            return SQFS_ERR_NO_MEMORY;
        }

        const uint8_t *block_sizes_data = p + header_size;
        for (uint32_t i = 0; i < block_count; i++) {
            inode->block_sizes[i] = sqfs_le32_to_cpu(block_sizes_data + i * 4);
        }

        *bytes_consumed = sizeof(sqfs_inode_header_t) + header_size +
                          block_count * sizeof(uint32_t);
    } else {
        inode->block_sizes = NULL;
        *bytes_consumed = sizeof(sqfs_inode_header_t) + header_size;
    }

    return SQFS_OK;
}

/*
 * Parse basic symlink inode.
 * Returns the number of bytes consumed from the data buffer.
 */
static int parse_inode_symlink(const void *data, sqfs_inode_t *inode,
                               size_t *bytes_consumed)
{
    const uint8_t *p = (const uint8_t *)data + sizeof(sqfs_inode_header_t);
    uint32_t target_size;

    inode->link_count = sqfs_le32_to_cpu(p);
    target_size = sqfs_le32_to_cpu(p + 4);
    inode->symlink.target_size = target_size;

    /* Allocate and copy target path */
    inode->symlink.target = sqfs_malloc(target_size + 1);
    if (inode->symlink.target == NULL) {
        return SQFS_ERR_NO_MEMORY;
    }

    memcpy(inode->symlink.target, p + 8, target_size);
    inode->symlink.target[target_size] = '\0';

    *bytes_consumed = sizeof(sqfs_inode_header_t) + 8 + target_size;

    return SQFS_OK;
}

/*
 * Parse extended symlink inode.
 * Returns the number of bytes consumed from the data buffer.
 */
static int parse_inode_lsymlink(const void *data, sqfs_inode_t *inode,
                                size_t *bytes_consumed)
{
    const uint8_t *p = (const uint8_t *)data + sizeof(sqfs_inode_header_t);
    uint32_t target_size;

    inode->link_count = sqfs_le32_to_cpu(p);
    target_size = sqfs_le32_to_cpu(p + 4);
    inode->symlink.target_size = target_size;

    /* Allocate and copy target path */
    inode->symlink.target = sqfs_malloc(target_size + 1);
    if (inode->symlink.target == NULL) {
        return SQFS_ERR_NO_MEMORY;
    }

    memcpy(inode->symlink.target, p + 8, target_size);
    inode->symlink.target[target_size] = '\0';

    /* Extended symlink has xattr_idx after target */
    inode->xattr_idx = sqfs_le32_to_cpu(p + 8 + target_size);

    *bytes_consumed = sizeof(sqfs_inode_header_t) + 8 + target_size + 4;

    return SQFS_OK;
}

/*
 * Parse device inode (basic or extended).
 */
static int parse_inode_dev(const void *data, sqfs_inode_t *inode,
                           bool is_extended, size_t *bytes_consumed)
{
    const uint8_t *p = (const uint8_t *)data + sizeof(sqfs_inode_header_t);
    uint32_t dev_num;

    inode->link_count = sqfs_le32_to_cpu(p);
    dev_num = sqfs_le32_to_cpu(p + 4);

    /* Decode major/minor from encoded device number */
    inode->dev.major = sqfs_dev_major(dev_num);
    inode->dev.minor = sqfs_dev_minor(dev_num);

    if (is_extended) {
        inode->xattr_idx = sqfs_le32_to_cpu(p + 8);
        *bytes_consumed = sizeof(sqfs_inode_header_t) + 12;
    } else {
        inode->xattr_idx = XATTR_INVALID;
        *bytes_consumed = sizeof(sqfs_inode_header_t) + 8;
    }

    return SQFS_OK;
}

/*
 * Parse IPC inode (FIFO/Socket, basic or extended).
 */
static int parse_inode_ipc(const void *data, sqfs_inode_t *inode,
                           bool is_extended, size_t *bytes_consumed)
{
    const uint8_t *p = (const uint8_t *)data + sizeof(sqfs_inode_header_t);

    inode->link_count = sqfs_le32_to_cpu(p);

    if (is_extended) {
        inode->xattr_idx = sqfs_le32_to_cpu(p + 4);
        *bytes_consumed = sizeof(sqfs_inode_header_t) + 8;
    } else {
        inode->xattr_idx = XATTR_INVALID;
        *bytes_consumed = sizeof(sqfs_inode_header_t) + 4;
    }

    return SQFS_OK;
}

/*
 * Parse raw inode data into runtime structure.
 */
static int parse_inode(const void *data, size_t data_size,
                       uint32_t block_size, sqfs_inode_t *inode)
{
    sqfs_inode_header_t header;
    size_t bytes_consumed = 0;
    int ret = SQFS_OK;

    if (data_size < sizeof(sqfs_inode_header_t)) {
        return SQFS_ERR_BAD_INODE;
    }

    parse_inode_header(data, &header);

    /* Validate inode type */
    if (header.type < SQFS_INODE_DIR || header.type > SQFS_INODE_LSOCKET) {
        return SQFS_ERR_BAD_INODE;
    }

    /* Initialize common fields */
    inode->inode_number = header.inode_number;
    inode->type = (sqfs_inode_type_t)header.type;
    inode->permissions = header.permissions;
    /* uid and gid will be resolved from uid_idx/gid_idx later */
    inode->uid = header.uid_idx;
    inode->gid = header.gid_idx;
    inode->mtime = header.mtime;
    inode->block_sizes = NULL;

    /* Parse type-specific data */
    switch (header.type) {
    case SQFS_INODE_DIR:
        if (data_size < sizeof(sqfs_inode_dir_t)) {
            return SQFS_ERR_BAD_INODE;
        }
        ret = parse_inode_dir(data, inode);
        break;

    case SQFS_INODE_LDIR:
        if (data_size < sizeof(sqfs_inode_ldir_t)) {
            return SQFS_ERR_BAD_INODE;
        }
        ret = parse_inode_ldir(data, inode);
        break;

    case SQFS_INODE_FILE:
        ret = parse_inode_file(data, inode, block_size, &bytes_consumed);
        break;

    case SQFS_INODE_LFILE:
        ret = parse_inode_lfile(data, inode, block_size, &bytes_consumed);
        break;

    case SQFS_INODE_SYMLINK:
        ret = parse_inode_symlink(data, inode, &bytes_consumed);
        break;

    case SQFS_INODE_LSYMLINK:
        ret = parse_inode_lsymlink(data, inode, &bytes_consumed);
        break;

    case SQFS_INODE_BLKDEV:
    case SQFS_INODE_CHRDEV:
        ret = parse_inode_dev(data, inode, false, &bytes_consumed);
        break;

    case SQFS_INODE_LBLKDEV:
    case SQFS_INODE_LCHRDEV:
        ret = parse_inode_dev(data, inode, true, &bytes_consumed);
        break;

    case SQFS_INODE_FIFO:
    case SQFS_INODE_SOCKET:
        ret = parse_inode_ipc(data, inode, false, &bytes_consumed);
        break;

    case SQFS_INODE_LFIFO:
    case SQFS_INODE_LSOCKET:
        ret = parse_inode_ipc(data, inode, true, &bytes_consumed);
        break;

    default:
        return SQFS_ERR_BAD_INODE;
    }

    /* Check if we consumed more data than available */
    if (ret == SQFS_OK && bytes_consumed > 0 && bytes_consumed > data_size) {
        return SQFS_ERR_BAD_INODE;
    }

    return ret;
}

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

/*
 * Calculate the number of data blocks for a file.
 *
 * For a file of size file_size with blocks of size block_size:
 * - Full blocks = file_size / block_size
 * - If there's a remainder and no fragment, the last partial block counts
 * - If there's a fragment, only full blocks count
 */
uint32_t sqfs_calc_block_count(uint64_t file_size, uint32_t block_size,
                               bool has_fragment)
{
    uint64_t full_blocks;
    uint64_t remainder;

    if (file_size == 0) {
        return 0;
    }

    full_blocks = file_size / block_size;
    remainder = file_size % block_size;

    /*
     * If there's a remainder and no fragment, the partial block counts as a block.
     * With fragments, the remainder goes in a fragment block instead.
     */
    if (!has_fragment && remainder != 0) {
        full_blocks++;
    }

    return (uint32_t)full_blocks;
}

/*
 * Get inode from metadata reference.
 *
 * The reference encodes both the block position and offset within the block.
 */
int sqfs_inode_get_by_ref(sqfs_fuse_ctx_t *ctx, uint64_t ref,
                          sqfs_inode_t **inode)
{
    int ret;
    uint8_t raw_inode[MAX_INODE_SIZE];
    sqfs_inode_header_t header;
    sqfs_inode_t *new_inode;
    size_t inode_size;

    if (inode == NULL) {
        return SQFS_ERR_BAD_INODE;
    }

    /* Read raw inode data - just the header first */
    ret = read_inode_metadata(ctx, ref, raw_inode, sizeof(sqfs_inode_header_t));
    if (ret != SQFS_OK) {
        return ret;
    }

    /* Parse header to determine inode size */
    parse_inode_header(raw_inode, &header);

    /* Validate inode type */
    if (header.type < SQFS_INODE_DIR || header.type > SQFS_INODE_LSOCKET) {
        return SQFS_ERR_BAD_INODE;
    }

    /* Calculate actual inode size based on type */
    switch (header.type) {
    case SQFS_INODE_DIR:
        inode_size = sizeof(sqfs_inode_dir_t);
        break;
    case SQFS_INODE_LDIR:
        inode_size = sizeof(sqfs_inode_ldir_t);
        break;
    case SQFS_INODE_FILE: {
        /* For basic files, we need header + fixed part + block_sizes */
        /* Read the fixed part first to get file_size */
        ret = read_inode_metadata(ctx, ref, raw_inode, sizeof(sqfs_inode_header_t) + 16);
        if (ret != SQFS_OK) {
            return ret;
        }
        /* The file size is at offset 28 (after header) */
        uint32_t file_size = sqfs_le32_to_cpu(raw_inode + 28);
        uint32_t frag_idx = sqfs_le32_to_cpu(raw_inode + 20);
        bool has_fragment = (frag_idx != FRAGMENT_INVALID);
        uint32_t block_count = sqfs_calc_block_count(file_size, ctx->sb->disk.block_size, has_fragment);
        inode_size = sizeof(sqfs_inode_header_t) + 16 + block_count * sizeof(uint32_t);
        break;
    }
    case SQFS_INODE_LFILE: {
        /* For extended files, we need header + fixed part + block_sizes */
        ret = read_inode_metadata(ctx, ref, raw_inode, sizeof(sqfs_inode_header_t) + 40);
        if (ret != SQFS_OK) {
            return ret;
        }
        uint64_t file_size = sqfs_le64_to_cpu(raw_inode + 24);
        uint32_t frag_idx = sqfs_le32_to_cpu(raw_inode + 44);
        bool has_fragment = (frag_idx != FRAGMENT_INVALID);
        uint32_t block_count = sqfs_calc_block_count(file_size, ctx->sb->disk.block_size, has_fragment);
        inode_size = sizeof(sqfs_inode_header_t) + 40 + block_count * sizeof(uint32_t);
        break;
    }
    case SQFS_INODE_SYMLINK: {
        /* For basic symlinks, read the target_size first */
        ret = read_inode_metadata(ctx, ref, raw_inode, sizeof(sqfs_inode_header_t) + 8);
        if (ret != SQFS_OK) {
            return ret;
        }
        uint32_t target_size = sqfs_le32_to_cpu(raw_inode + 20);  /* offset after header + link_count */
        inode_size = sizeof(sqfs_inode_header_t) + 8 + target_size;
        break;
    }
    case SQFS_INODE_LSYMLINK: {
        /* For extended symlinks, read target_size and account for xattr_idx */
        ret = read_inode_metadata(ctx, ref, raw_inode, sizeof(sqfs_inode_header_t) + 8);
        if (ret != SQFS_OK) {
            return ret;
        }
        uint32_t target_size = sqfs_le32_to_cpu(raw_inode + 20);
        inode_size = sizeof(sqfs_inode_header_t) + 8 + target_size + 4;  /* +4 for xattr_idx */
        break;
    }
    case SQFS_INODE_BLKDEV:
    case SQFS_INODE_CHRDEV:
        inode_size = sizeof(sqfs_inode_dev_t);
        break;
    case SQFS_INODE_LBLKDEV:
    case SQFS_INODE_LCHRDEV:
        inode_size = sizeof(sqfs_inode_ldev_t);
        break;
    case SQFS_INODE_FIFO:
    case SQFS_INODE_SOCKET:
        inode_size = sizeof(sqfs_inode_ipc_t);
        break;
    case SQFS_INODE_LFIFO:
    case SQFS_INODE_LSOCKET:
        inode_size = sizeof(sqfs_inode_lipc_t);
        break;
    default:
        return SQFS_ERR_BAD_INODE;
    }

    /* Read full inode data */
    if (inode_size > MAX_INODE_SIZE) {
        inode_size = MAX_INODE_SIZE;
    }
    ret = read_inode_metadata(ctx, ref, raw_inode, inode_size);
    if (ret != SQFS_OK) {
        return ret;
    }

    /* Allocate inode structure */
    new_inode = sqfs_calloc(1, sizeof(sqfs_inode_t));
    if (new_inode == NULL) {
        return SQFS_ERR_NO_MEMORY;
    }

    /* Parse the inode */
    ret = parse_inode(raw_inode, inode_size, ctx->sb->disk.block_size, new_inode);
    if (ret != SQFS_OK) {
        sqfs_free(new_inode);
        return ret;
    }

    *inode = new_inode;
    return SQFS_OK;
}

/*
 * Load an inode by inode number.
 *
 * In SquashFS, inode numbers are 1-based indices.
 * With the export table, we can directly look up the inode reference.
 *
 * Export table structure:
 * - export_table points to a lookup table of 64-bit block pointers
 * - Each block pointer points to a metadata block with up to 1024 inode refs
 * - Each inode ref is a 64-bit value (block_pos << 16 | offset)
 */
int sqfs_inode_load(sqfs_fuse_ctx_t *ctx, uint64_t inode_num,
                    sqfs_inode_t **inode)
{
    int ret;
    uint64_t inode_ref;
    static uint8_t *export_meta_block = NULL;  /* Cached metadata block */
    static size_t export_meta_size = 0;
    static uint64_t export_meta_block_addr = 0;

    if (inode == NULL) {
        return SQFS_ERR_BAD_INODE;
    }

    *inode = NULL;

    if (inode_num == 0) {
        return SQFS_ERR_NOT_FOUND;
    }

    /* Check if we have an export table */
    if (!ctx->sb->has_export || ctx->sb->disk.export_table == SQFS_INVALID_OFFSET) {
        SQFS_LOG_INODE_ERROR("No export table available for inode lookup");
        return SQFS_ERR_NOT_FOUND;
    }

    /* Calculate which metadata block and index within it
     * Each metadata block holds up to 1024 inode references
     */
    uint64_t block_index = (inode_num - 1) / 1024;
    uint64_t index_in_block = (inode_num - 1) % 1024;

    /* Read the lookup table entry (64-bit pointer) */
    uint64_t lookup_offset = ctx->sb->disk.export_table + block_index * sizeof(uint64_t);

    SQFS_LOG_INODE_DEBUG("Reading export lookup at 0x%lx for inode %lu (block_index=%lu)",
                         (unsigned long)lookup_offset, (unsigned long)inode_num,
                         (unsigned long)block_index);

    uint64_t meta_block_addr;
    ret = sqfs_pread_all(ctx->sb->fd, &meta_block_addr, sizeof(meta_block_addr), lookup_offset);
    if (ret != SQFS_OK) {
        SQFS_LOG_INODE_ERROR("Failed to read export lookup table");
        return ret;
    }
    meta_block_addr = sqfs_le64_to_cpu(&meta_block_addr);

    SQFS_LOG_INODE_DEBUG("Export lookup: metadata block at 0x%lx", (unsigned long)meta_block_addr);

    if (meta_block_addr == SQFS_INVALID_OFFSET || meta_block_addr == 0) {
        SQFS_LOG_INODE_ERROR("Invalid metadata block address in export table");
        return SQFS_ERR_NOT_FOUND;
    }

    /* Read and cache the metadata block if needed */
    if (export_meta_block == NULL || export_meta_block_addr != meta_block_addr) {
        /* Allocate buffer if needed */
        if (export_meta_block == NULL) {
            export_meta_block = sqfs_malloc(SQFS_META_BLOCK_SIZE);
            if (export_meta_block == NULL) {
                return SQFS_ERR_NO_MEMORY;
            }
        }

        SQFS_LOG_INODE_DEBUG("Reading export metadata block from 0x%lx",
                             (unsigned long)meta_block_addr);

        ret = sqfs_meta_read_block(ctx->sb->fd, meta_block_addr,
                                   export_meta_block, &export_meta_size, ctx->comp);
        if (ret != SQFS_OK) {
            SQFS_LOG_INODE_ERROR("Failed to read export metadata block at 0x%lx",
                                 (unsigned long)meta_block_addr);
            return ret;
        }

        export_meta_block_addr = meta_block_addr;
        SQFS_LOG_INODE_DEBUG("Export metadata block size: %zu bytes", export_meta_size);
    }

    /* Calculate offset in the metadata block
     * Each entry is a 64-bit inode reference
     */
    size_t offset = index_in_block * sizeof(uint64_t);
    if (offset + sizeof(uint64_t) > export_meta_size) {
        SQFS_LOG_INODE_ERROR("Inode %lu out of export metadata block bounds", (unsigned long)inode_num);
        return SQFS_ERR_NOT_FOUND;
    }

    /* Read the inode reference */
    memcpy(&inode_ref, export_meta_block + offset, sizeof(inode_ref));
    inode_ref = sqfs_le64_to_cpu(&inode_ref);

    SQFS_LOG_INODE_DEBUG("Inode %lu maps to ref 0x%lx",
                         (unsigned long)inode_num, (unsigned long)inode_ref);

    /* Load the inode by reference */
    return sqfs_inode_get_by_ref(ctx, inode_ref, inode);
}

/*
 * Free an inode structure.
 *
 * Frees all dynamically allocated memory associated with the inode.
 */
void sqfs_inode_free(sqfs_inode_t *inode)
{
    if (inode == NULL) {
        return;
    }

    /* Free block sizes array */
    if (inode->block_sizes != NULL) {
        sqfs_free(inode->block_sizes);
        inode->block_sizes = NULL;
    }

    /* Free symlink target */
    if (sqfs_inode_is_symlink(inode->type) && inode->symlink.target != NULL) {
        sqfs_free(inode->symlink.target);
        inode->symlink.target = NULL;
    }

    /* Free the inode structure itself */
    sqfs_free(inode);
}

/*
 * Print inode information for debugging.
 */
void sqfs_inode_print(const sqfs_inode_t *inode)
{
    if (inode == NULL) {
        printf("inode: NULL\n");
        return;
    }

    printf("Inode #%lu:\n", (unsigned long)inode->inode_number);
    printf("  type: %d (%s)\n", inode->type,
           sqfs_inode_type_name(inode->type));
    printf("  permissions: %04o\n", inode->permissions);
    printf("  uid: %u, gid: %u\n", inode->uid, inode->gid);
    printf("  mtime: %u\n", inode->mtime);
    printf("  link_count: %u\n", inode->link_count);

    switch (inode->type) {
    case SQFS_INODE_DIR:
    case SQFS_INODE_LDIR:
        printf("  [directory]\n");
        printf("    block_idx: %lu\n", (unsigned long)inode->dir.block_idx);
        printf("    size: %u\n", inode->dir.size);
        printf("    block_offset: %u\n", inode->dir.block_offset);
        printf("    parent_inode: %lu\n", (unsigned long)inode->dir.parent_inode);
        if (inode->type == SQFS_INODE_LDIR) {
            printf("    index_count: %u\n", inode->dir.index_count);
            printf("    xattr_idx: %u\n", inode->xattr_idx);
        }
        break;

    case SQFS_INODE_FILE:
    case SQFS_INODE_LFILE:
        printf("  [file]\n");
        printf("    blocks_start: %lu\n", (unsigned long)inode->file.blocks_start);
        printf("    file_size: %lu\n", (unsigned long)inode->file.file_size);
        printf("    sparse: %lu\n", (unsigned long)inode->file.sparse);
        printf("    frag_idx: %u\n", inode->file.frag_idx);
        printf("    block_offset: %u\n", inode->file.block_offset);
        printf("    block_count: %u\n", inode->file.block_count);
        if (inode->type == SQFS_INODE_LFILE) {
            printf("    xattr_idx: %u\n", inode->file.xattr_idx);
        }
        if (inode->block_sizes != NULL && inode->file.block_count > 0) {
            printf("    block_sizes: ");
            for (uint32_t i = 0; i < inode->file.block_count && i < 5; i++) {
                printf("%u ", inode->block_sizes[i]);
            }
            if (inode->file.block_count > 5) {
                printf("...");
            }
            printf("\n");
        }
        break;

    case SQFS_INODE_SYMLINK:
    case SQFS_INODE_LSYMLINK:
        printf("  [symlink]\n");
        printf("    target: %s\n", inode->symlink.target);
        if (inode->type == SQFS_INODE_LSYMLINK) {
            printf("    xattr_idx: %u\n", inode->xattr_idx);
        }
        break;

    case SQFS_INODE_BLKDEV:
    case SQFS_INODE_LBLKDEV:
        printf("  [block device]\n");
        printf("    major: %u, minor: %u\n", inode->dev.major, inode->dev.minor);
        if (inode->type == SQFS_INODE_LBLKDEV) {
            printf("    xattr_idx: %u\n", inode->xattr_idx);
        }
        break;

    case SQFS_INODE_CHRDEV:
    case SQFS_INODE_LCHRDEV:
        printf("  [char device]\n");
        printf("    major: %u, minor: %u\n", inode->dev.major, inode->dev.minor);
        if (inode->type == SQFS_INODE_LCHRDEV) {
            printf("    xattr_idx: %u\n", inode->xattr_idx);
        }
        break;

    case SQFS_INODE_FIFO:
    case SQFS_INODE_LFIFO:
        printf("  [fifo]\n");
        if (inode->type == SQFS_INODE_LFIFO) {
            printf("    xattr_idx: %u\n", inode->xattr_idx);
        }
        break;

    case SQFS_INODE_SOCKET:
    case SQFS_INODE_LSOCKET:
        printf("  [socket]\n");
        if (inode->type == SQFS_INODE_LSOCKET) {
            printf("    xattr_idx: %u\n", inode->xattr_idx);
        }
        break;

    default:
        printf("  [unknown type]\n");
        break;
    }
}

/*
 * Get human-readable name for inode type.
 */
const char *sqfs_inode_type_name(sqfs_inode_type_t type)
{
    switch (type) {
    case SQFS_INODE_DIR:      return "directory";
    case SQFS_INODE_FILE:     return "file";
    case SQFS_INODE_SYMLINK:  return "symlink";
    case SQFS_INODE_BLKDEV:   return "block device";
    case SQFS_INODE_CHRDEV:   return "char device";
    case SQFS_INODE_FIFO:     return "fifo";
    case SQFS_INODE_SOCKET:   return "socket";
    case SQFS_INODE_LDIR:     return "extended directory";
    case SQFS_INODE_LFILE:    return "extended file";
    case SQFS_INODE_LSYMLINK: return "extended symlink";
    case SQFS_INODE_LBLKDEV:  return "extended block device";
    case SQFS_INODE_LCHRDEV:  return "extended char device";
    case SQFS_INODE_LFIFO:    return "extended fifo";
    case SQFS_INODE_LSOCKET:  return "extended socket";
    default:                  return "unknown";
    }
}