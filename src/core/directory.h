/*
 * SquashFS-FUSE - Directory Table Interface
 *
 * Copyright (c) 2024 SquashFS-FUSE Authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This file defines the directory table data structures and interface
 * for reading directory entries from a SquashFS filesystem.
 */

#ifndef SQFS_DIRECTORY_H
#define SQFS_DIRECTORY_H

#include <stdint.h>
#include <stddef.h>

#include "inode.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Directory table header
 *
 * Each directory in the directory table starts with this header,
 * followed by one or more directory entries.
 */
typedef struct __attribute__((packed)) {
    uint32_t count;            /* Entry count - 1 (actual entries = count + 1) */
    uint32_t start;            /* Inode metadata block position */
    uint32_t inode_number;     /* Reference inode number (base for relative offsets) */
} sqfs_dir_header_t;

/*
 * Directory entry
 *
 * Each entry describes one file/subdirectory within a directory.
 * The actual inode number is calculated as:
 *   inode_number = header.inode_number + entry.inode_offset
 */
typedef struct __attribute__((packed)) {
    uint16_t offset;           /* Offset within inode metadata block */
    int16_t inode_offset;      /* Relative inode number offset (can be negative) */
    uint16_t type;             /* Inode type (basic types only: 1-7) */
    uint16_t name_size;        /* Name length - 1 (actual length = name_size + 1) */
    /* Followed by name[] byte array (not null-terminated on disk) */
} sqfs_dir_entry_t;

/*
 * Directory index entry (for extended directories only)
 *
 * Extended directories (SQFS_INODE_LDIR) contain an index that
 * allows fast seeking within large directories.
 */
typedef struct __attribute__((packed)) {
    uint32_t index;            /* Byte offset within directory table */
    uint32_t start;            /* Directory table block position */
    uint32_t name_size;        /* Name length - 1 */
    /* Followed by name[] byte array */
} sqfs_dir_index_t;

/*
 * Runtime directory entry
 *
 * This is the parsed, usable form of a directory entry.
 * The name is null-terminated and allocated memory is owned by this struct.
 */
typedef struct {
    char *name;                /* Null-terminated entry name (caller must free) */
    uint64_t inode_number;     /* Absolute inode number */
    uint64_t inode_ref;        /* Inode reference (block_pos << 16 | offset) */
    sqfs_inode_type_t type;    /* Inode type */
} sqfs_dirent_t;

/*
 * Forward declarations
 */
struct sqfs_ctx;
typedef struct sqfs_ctx sqfs_ctx_t;

/* Backward compatibility */
typedef sqfs_ctx_t sqfs_fuse_ctx_t;

/*
 * Read directory entries from a directory inode
 *
 * This function reads all entries from a directory inode and returns
 * them as an array of sqfs_dirent_t structures.
 *
 * @param ctx       FUSE context containing superblock and compressor
 * @param dir_inode Directory inode (must be SQFS_INODE_DIR or SQFS_INODE_LDIR)
 * @param entries   Output: pointer to array of directory entries (caller must free)
 * @param count     Output: number of entries returned
 *
 * @return SQFS_OK on success, negative error code on failure:
 *         SQFS_ERR_BAD_DIR if directory data is corrupted
 *         SQFS_ERR_NO_MEMORY on allocation failure
 *         SQFS_ERR_IO on I/O error
 *
 * Memory management:
 * - On success, *entries is allocated and must be freed by caller
 * - Each entry's name is allocated and must be freed individually
 * - Use sqfs_dirent_free() to properly free all entries
 */
int sqfs_dir_read(sqfs_fuse_ctx_t *ctx, sqfs_inode_t *dir_inode,
                  sqfs_dirent_t **entries, size_t *count);

/*
 * Free an array of directory entries
 *
 * @param entries   Array of directory entries
 * @param count     Number of entries
 */
void sqfs_dirent_free(sqfs_dirent_t *entries, size_t count);

/*
 * Look up a directory entry by name
 *
 * @param entries   Array of directory entries
 * @param count     Number of entries
 * @param name      Name to look up
 *
 * @return Pointer to entry if found, NULL otherwise
 */
sqfs_dirent_t *sqfs_dirent_lookup(sqfs_dirent_t *entries, size_t count,
                                   const char *name);

/*
 * Print directory entries (for debugging)
 *
 * @param entries   Array of directory entries
 * @param count     Number of entries
 */
void sqfs_directory_print(const sqfs_dirent_t *entries, size_t count);

#ifdef __cplusplus
}
#endif

#endif /* SQFS_DIRECTORY_H */