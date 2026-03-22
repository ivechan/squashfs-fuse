/*
 * SquashFS - VFS Abstraction Layer
 *
 * Defines VFS-agnostic interfaces that can be implemented by different
 * backends (FUSE, Linux kernel module, etc.).
 *
 * Copyright (c) 2024 SquashFS Authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef SQFS_VFS_H
#define SQFS_VFS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * VFS Result Codes
 * ============================================================================ */

/*
 * VFS operation result codes.
 * These are negative errno values for compatibility with system calls.
 */
typedef enum {
    SQFS_VFS_OK              = 0,
    SQFS_VFS_ERR_NOT_FOUND   = -2,    /* ENOENT */
    SQFS_VFS_ERR_NOT_DIR     = -20,   /* ENOTDIR */
    SQFS_VFS_ERR_IS_DIR      = -21,   /* EISDIR */
    SQFS_VFS_ERR_ROFS        = -30,   /* EROFS */
    SQFS_VFS_ERR_NOMEM       = -12,   /* ENOMEM */
    SQFS_VFS_ERR_INVAL       = -22,   /* EINVAL */
    SQFS_VFS_ERR_IO          = -5,    /* EIO */
    SQFS_VFS_ERR_NOSUP       = -95,   /* EOPNOTSUPP */
    SQFS_VFS_ERR_NODATA      = -61,   /* ENODATA */
    SQFS_VFS_ERR_BAD_FD      = -9,    /* EBADF */
    SQFS_VFS_ERR_TOO_BIG     = -75,   /* EOVERFLOW */
} sqfs_vfs_result_t;

/* ============================================================================
 * VFS File Attributes
 * ============================================================================ */

/*
 * VFS-agnostic file attributes structure.
 * This can be converted to backend-specific structures (struct stat, inode, etc.)
 */
typedef struct {
    uint64_t    ino;            /* Inode number */
    uint32_t    mode;           /* File type and permissions (S_IFMT | mode) */
    uint32_t    nlink;          /* Number of hard links */
    uint32_t    uid;            /* User ID */
    uint32_t    gid;            /* Group ID */
    uint64_t    size;           /* File size in bytes */
    uint64_t    blocks;         /* Number of 512-byte blocks */
    uint32_t    blksize;        /* Block size for I/O */
    uint64_t    atime;          /* Access time (Unix timestamp) */
    uint64_t    mtime;          /* Modification time (Unix timestamp) */
    uint64_t    ctime;          /* Change time (Unix timestamp) */
    uint32_t    rdev_major;     /* Device major number (for block/char devices) */
    uint32_t    rdev_minor;     /* Device minor number (for block/char devices) */
} sqfs_vfs_attr_t;

/* ============================================================================
 * VFS Directory Entry
 * ============================================================================ */

/*
 * VFS-agnostic directory entry.
 * Returned by readdir operations.
 */
typedef struct {
    char           *name;       /* Entry name (null-terminated, caller must free) */
    uint64_t        ino;        /* Inode number */
    uint32_t        type;       /* File type (S_IFDIR, S_IFREG, etc.) */
} sqfs_vfs_dirent_t;

/* ============================================================================
 * VFS File Handle
 * ============================================================================ */

/*
 * VFS-agnostic file handle.
 * Used for open files.
 */
typedef struct sqfs_vfs_fh {
    void           *inode;      /* Pointer to internal inode structure */
    uint64_t        file_size;  /* File size in bytes */
} sqfs_vfs_fh_t;

/* ============================================================================
 * VFS Filesystem Statistics
 * ============================================================================ */

/*
 * VFS-agnostic filesystem statistics.
 * This can be converted to backend-specific structures (struct statvfs, etc.)
 */
typedef struct {
    uint64_t    bsize;          /* Block size */
    uint64_t    frsize;         /* Fragment size */
    uint64_t    blocks;         /* Total blocks */
    uint64_t    bfree;          /* Free blocks */
    uint64_t    bavail;         /* Available blocks for non-root */
    uint64_t    files;          /* Total inodes */
    uint64_t    ffree;          /* Free inodes */
    uint64_t    favail;         /* Available inodes for non-root */
    uint32_t    fsid;           /* Filesystem ID */
    uint32_t    flags;          /* Mount flags (ST_RDONLY, ST_NOSUID, etc.) */
    uint32_t    namemax;        /* Maximum filename length */
} sqfs_vfs_statfs_t;

/* ============================================================================
 * Forward Declarations
 * ============================================================================ */

struct sqfs_ctx;
struct sqfs_inode;

/* Type definition for context */
typedef struct sqfs_ctx sqfs_ctx_t;

/* ============================================================================
 * VFS Operations Interface
 * ============================================================================ */

/*
 * VFS operations function pointer table.
 * Each backend (FUSE, kernel) provides its own implementation.
 */
typedef struct sqfs_vfs_ops {
    /* Context lifecycle */
    int  (*init)(struct sqfs_ctx *ctx);
    void (*destroy)(struct sqfs_ctx *ctx);

    /* File operations */
    int  (*getattr)(struct sqfs_ctx *ctx, const char *path, sqfs_vfs_attr_t *attr);
    int  (*open)(struct sqfs_ctx *ctx, const char *path, sqfs_vfs_fh_t **fh);
    int  (*read)(struct sqfs_ctx *ctx, sqfs_vfs_fh_t *fh, void *buf,
                 size_t size, uint64_t offset);
    void (*release)(struct sqfs_ctx *ctx, sqfs_vfs_fh_t *fh);

    /* Directory operations */
    int  (*opendir)(struct sqfs_ctx *ctx, const char *path);
    int  (*readdir)(struct sqfs_ctx *ctx, const char *path,
                    sqfs_vfs_dirent_t **entries, size_t *count);
    void (*releasedir)(struct sqfs_ctx *ctx);

    /* Symbolic link */
    int  (*readlink)(struct sqfs_ctx *ctx, const char *path, char *buf, size_t size);

    /* Filesystem information */
    int  (*statfs)(struct sqfs_ctx *ctx, sqfs_vfs_statfs_t *stat);

    /* Extended attributes */
    int  (*getxattr)(struct sqfs_ctx *ctx, const char *path, const char *name,
                     void *value, size_t size);
    int  (*listxattr)(struct sqfs_ctx *ctx, const char *path,
                      char *list, size_t size);
} sqfs_vfs_ops_t;

/* ============================================================================
 * VFS Operations (Implementation)
 * ============================================================================ */

/*
 * Resolve a path to an inode.
 *
 * @param ctx       SquashFS context
 * @param path      Path to resolve (absolute, starting with /)
 * @param out_inode Output: pointer to allocated inode structure
 *
 * @return 0 on success, negative error code on failure
 */
int sqfs_vfs_resolve_path(sqfs_ctx_t *ctx, const char *path,
                          struct sqfs_inode **out_inode);

/*
 * Fill VFS attributes from an inode.
 *
 * @param ctx   SquashFS context
 * @param inode Parsed inode structure
 * @param attr  Output: VFS attributes structure
 *
 * @return 0 on success, negative error code on failure
 */
int sqfs_vfs_fill_attr(sqfs_ctx_t *ctx, struct sqfs_inode *inode,
                       sqfs_vfs_attr_t *attr);

/*
 * Get file attributes by path.
 *
 * @param ctx   SquashFS context
 * @param path  Path to the file
 * @param attr  Output: VFS attributes structure
 *
 * @return 0 on success, negative error code on failure
 */
int sqfs_vfs_getattr(struct sqfs_ctx *ctx, const char *path,
                     sqfs_vfs_attr_t *attr);

/*
 * Open a file.
 *
 * @param ctx   SquashFS context
 * @param path  Path to the file
 * @param fh    Output: file handle
 *
 * @return 0 on success, negative error code on failure
 */
int sqfs_vfs_open(struct sqfs_ctx *ctx, const char *path, sqfs_vfs_fh_t **fh);

/*
 * Read from an open file.
 *
 * @param ctx    SquashFS context
 * @param fh     File handle
 * @param buf    Output buffer
 * @param size   Number of bytes to read
 * @param offset File offset
 *
 * @return Number of bytes read, or negative error code on failure
 */
int sqfs_vfs_read(struct sqfs_ctx *ctx, sqfs_vfs_fh_t *fh,
                  void *buf, size_t size, uint64_t offset);

/*
 * Close an open file.
 *
 * @param ctx SquashFS context
 * @param fh  File handle to close
 */
void sqfs_vfs_release(struct sqfs_ctx *ctx, sqfs_vfs_fh_t *fh);

/*
 * Read directory entries.
 *
 * @param ctx     SquashFS context
 * @param path    Path to the directory
 * @param entries Output: array of directory entries (caller must free)
 * @param count   Output: number of entries
 *
 * @return 0 on success, negative error code on failure
 */
int sqfs_vfs_readdir(struct sqfs_ctx *ctx, const char *path,
                     sqfs_vfs_dirent_t **entries, size_t *count);

/*
 * Free directory entries array.
 *
 * @param entries Array of directory entries
 * @param count   Number of entries
 */
void sqfs_vfs_dirent_free(sqfs_vfs_dirent_t *entries, size_t count);

/*
 * Read symbolic link target.
 *
 * @param ctx   SquashFS context
 * @param path  Path to the symbolic link
 * @param buf   Output buffer for target path
 * @param size  Buffer size
 *
 * @return 0 on success, negative error code on failure
 */
int sqfs_vfs_readlink(struct sqfs_ctx *ctx, const char *path,
                      char *buf, size_t size);

/*
 * Get filesystem statistics.
 *
 * @param ctx   SquashFS context
 * @param stat  Output: filesystem statistics
 *
 * @return 0 on success, negative error code on failure
 */
int sqfs_vfs_statfs(struct sqfs_ctx *ctx, sqfs_vfs_statfs_t *stat);

/*
 * Get extended attribute value.
 *
 * @param ctx    SquashFS context
 * @param path   Path to the file
 * @param name   Attribute name
 * @param value  Output buffer for attribute value
 * @param size   Buffer size
 *
 * @return Attribute value size on success, negative error code on failure
 */
int sqfs_vfs_getxattr(struct sqfs_ctx *ctx, const char *path,
                      const char *name, void *value, size_t size);

/*
 * List extended attribute names.
 *
 * @param ctx   SquashFS context
 * @param path  Path to the file
 * @param list  Output buffer for attribute names (null-separated)
 * @param size  Buffer size
 *
 * @return Total size of attribute names on success, negative error code on failure
 */
int sqfs_vfs_listxattr(struct sqfs_ctx *ctx, const char *path,
                       char *list, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* SQFS_VFS_H */