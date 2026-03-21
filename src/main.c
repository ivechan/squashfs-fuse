/*
 * SquashFS-FUSE - Main Program
 *
 * FUSE 3.x operations implementation and main entry point.
 *
 * Copyright (c) 2024 SquashFS-FUSE Authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define _POSIX_C_SOURCE 200809L
#define FUSE_USE_VERSION 31

#include <fuse3/fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/sysmacros.h>
#include <sys/vfs.h>
#include <unistd.h>
#include <fcntl.h>

#include "utils.h"
#include "superblock.h"
#include "inode.h"
#include "directory.h"
#include "compressor.h"
#include "cache.h"
#include "data.h"
#include "fragment.h"
#include "xattr.h"
#include "log.h"
#include "stats.h"
#include "context.h"

/* ============================================================================
 * Version and Build Information
 * ============================================================================ */

#define SQFS_FUSE_VERSION_MAJOR 1
#define SQFS_FUSE_VERSION_MINOR 0
#define SQFS_FUSE_VERSION_STRING "1.0.0"

/* ============================================================================
 * Cache Configuration Defaults
 * ============================================================================ */

#define DEFAULT_CACHE_SIZE_MB    32

/* File handle for open files */
typedef struct {
    sqfs_inode_t *inode;
    uint64_t      file_size;
} sqfs_file_handle_t;

/* ============================================================================
 * Command Line Options
 * ============================================================================ */

static const char *usage_string =
    "Usage: squashfs-fuse [OPTIONS] <image_file> <mount_point>\n"
    "\n"
    "Options:\n"
    "  -h, --help          Show this help message\n"
    "  -V, --version       Show version information\n"
    "  -d, --debug LEVEL   Set debug level (0-3)\n"
    "  -o, --options OPTS  FUSE mount options\n"
    "  -f, --foreground    Run in foreground\n"
    "  -s, --single        Single-threaded mode\n"
    "  --nocache           Disable all caching\n"
    "  --cache-size SIZE   Set cache size in MiB (default: %d)\n"
    "  -l, --log PATH      Log file path (default: /tmp/squashfs-fuse.log)\n"
    "\n";

/* ============================================================================
 * ID Table Loading
 * ============================================================================ */

static int load_id_table(sqfs_fuse_ctx_t *ctx) {
    int ret;
    uint64_t id_table_pos = ctx->sb->disk.id_table;
    size_t id_count = ctx->sb->disk.id_count;

    if (id_count == 0) {
        ctx->id_table = NULL;
        ctx->id_count = 0;
        return SQFS_OK;
    }

    /* Read ID table lookup table (array of 64-bit positions) */
    uint64_t *id_pos_table = sqfs_malloc(id_count * sizeof(uint64_t));
    if (id_pos_table == NULL) {
        return SQFS_ERR_NO_MEMORY;
    }

    /* Read the lookup table that points to metadata blocks containing IDs */
    ret = sqfs_pread_all(ctx->sb->fd, id_pos_table,
                         id_count * sizeof(uint64_t), id_table_pos);
    if (ret != SQFS_OK) {
        sqfs_free(id_pos_table);
        return ret;
    }

    /* Allocate ID table */
    ctx->id_table = sqfs_malloc(id_count * sizeof(uint32_t));
    if (ctx->id_table == NULL) {
        sqfs_free(id_pos_table);
        return SQFS_ERR_NO_MEMORY;
    }
    ctx->id_count = id_count;

    /* Read IDs from metadata block */
    /* For simplicity, read the first metadata block containing IDs */
    /* In practice, IDs are stored in one or more metadata blocks */
    uint8_t *meta_block = sqfs_malloc(SQFS_META_MAX_SIZE);
    if (meta_block == NULL) {
        sqfs_free(id_pos_table);
        sqfs_free(ctx->id_table);
        ctx->id_table = NULL;
        return SQFS_ERR_NO_MEMORY;
    }

    size_t out_size;
    uint64_t first_block_pos = sqfs_le64_to_cpu(id_pos_table);

    ret = sqfs_meta_read_block(ctx->sb->fd, first_block_pos,
                               meta_block, &out_size, ctx->comp);
    if (ret != SQFS_OK) {
        sqfs_free(meta_block);
        sqfs_free(id_pos_table);
        sqfs_free(ctx->id_table);
        ctx->id_table = NULL;
        return ret;
    }

    /* Copy IDs from metadata block */
    size_t ids_to_copy = (id_count * sizeof(uint32_t) < out_size) ?
                         id_count : out_size / sizeof(uint32_t);
    memcpy(ctx->id_table, meta_block, ids_to_copy * sizeof(uint32_t));

    sqfs_free(meta_block);
    sqfs_free(id_pos_table);

    SQFS_LOG("Loaded %zu IDs from table", ids_to_copy);

    return SQFS_OK;
}

/* ============================================================================
 * Path Resolution
 * ============================================================================ */

/*
 * Resolve a path to an inode.
 * Walks the directory tree from root to find the inode for a given path.
 */
static int resolve_path(sqfs_fuse_ctx_t *ctx, const char *path,
                        sqfs_inode_t **out_inode) {
    int ret;
    sqfs_inode_t *inode = NULL;
    char *path_copy = NULL;
    char *component = NULL;
    char *saveptr = NULL;

    SQFS_LOG_DEBUG(SQFS_MOD_FUSE, "resolve_path: path=%s", path);

    if (path == NULL || out_inode == NULL) {
        return SQFS_ERR_CORRUPT;
    }

    *out_inode = NULL;

    /* Start from root inode */
    uint64_t root_ref = ctx->sb->disk.root_inode;
    SQFS_LOG_DEBUG(SQFS_MOD_FUSE, "resolve_path: root_ref=0x%lx", (unsigned long)root_ref);

    ret = sqfs_inode_get_by_ref(ctx, root_ref, &inode);
    if (ret != SQFS_OK) {
        SQFS_LOG_ERROR(SQFS_MOD_FUSE, "resolve_path: failed to get root inode, ret=%d", ret);
        return ret;
    }

    SQFS_LOG_DEBUG(SQFS_MOD_FUSE, "resolve_path: got root inode, type=%d", inode->type);

    /* Handle root path */
    if (path[0] == '/' && path[1] == '\0') {
        SQFS_LOG_DEBUG(SQFS_MOD_FUSE, "resolve_path: returning root inode");
        *out_inode = inode;
        return SQFS_OK;
    }

    /* Make a copy for tokenization */
    path_copy = sqfs_malloc(strlen(path) + 1);
    if (path_copy == NULL) {
        sqfs_inode_free(inode);
        return SQFS_ERR_NO_MEMORY;
    }
    strcpy(path_copy, path);

    /* Walk path components */
    component = strtok_r(path_copy + 1, "/", &saveptr);
    while (component != NULL && *component != '\0') {
        SQFS_LOG_DEBUG(SQFS_MOD_FUSE, "resolve_path: looking for component '%s'", component);

        /* Must be a directory to continue */
        if (!sqfs_inode_is_dir(inode->type)) {
            SQFS_LOG_DEBUG(SQFS_MOD_FUSE, "resolve_path: not a directory");
            sqfs_inode_free(inode);
            sqfs_free(path_copy);
            return SQFS_ERR_NOT_FOUND;
        }

        /* Read directory entries */
        sqfs_dirent_t *entries = NULL;
        size_t entry_count = 0;

        ret = sqfs_dir_read(ctx, inode, &entries, &entry_count);
        sqfs_inode_free(inode);
        inode = NULL;

        if (ret != SQFS_OK) {
            SQFS_LOG_ERROR(SQFS_MOD_FUSE, "resolve_path: sqfs_dir_read failed, ret=%d", ret);
            sqfs_free(path_copy);
            return ret;
        }

        SQFS_LOG_DEBUG(SQFS_MOD_FUSE, "resolve_path: directory has %zu entries", entry_count);

        /* Find matching entry */
        int found = 0;
        for (size_t i = 0; i < entry_count; i++) {
            if (strcmp(entries[i].name, component) == 0) {
                /* Load the inode by reference (more reliable than export table lookup) */
                ret = sqfs_inode_get_by_ref(ctx, entries[i].inode_ref, &inode);
                if (ret == SQFS_OK) {
                    found = 1;
                }
                break;
            }
        }

        sqfs_dirent_free(entries, entry_count);

        if (!found) {
            SQFS_LOG_DEBUG(SQFS_MOD_FUSE, "resolve_path: component '%s' not found", component);
            sqfs_free(path_copy);
            return SQFS_ERR_NOT_FOUND;
        }

        component = strtok_r(NULL, "/", &saveptr);
    }

    sqfs_free(path_copy);

    if (inode == NULL) {
        return SQFS_ERR_NOT_FOUND;
    }

    *out_inode = inode;
    return SQFS_OK;
}

/* ============================================================================
 * FUSE Operations
 * ============================================================================ */

/*
 * getattr - Get file attributes
 */
static int sqfs_fuse_getattr(const char *path, struct stat *stbuf,
                             struct fuse_file_info *fi) {
    (void)fi;
    sqfs_fuse_ctx_t *ctx = (sqfs_fuse_ctx_t *)fuse_get_context()->private_data;
    int ret;
    sqfs_inode_t *inode = NULL;

    SQFS_LOG_DEBUG(SQFS_MOD_FUSE, "getattr called: path=%s", path);

    if (stbuf == NULL) {
        SQFS_LOG_ERROR(SQFS_MOD_FUSE, "getattr: stbuf is NULL");
        return -EINVAL;
    }

    memset(stbuf, 0, sizeof(struct stat));

    /* Resolve path to inode */
    ret = resolve_path(ctx, path, &inode);
    if (ret != SQFS_OK) {
        SQFS_LOG_DEBUG(SQFS_MOD_FUSE, "getattr: resolve_path failed for %s, ret=%d", path, ret);
        return -sqfs_errno(ret);
    }

    SQFS_LOG_DEBUG(SQFS_MOD_FUSE, "getattr: resolved %s to inode %lu, type=%d",
                   path, (unsigned long)inode->inode_number, inode->type);

    /* Fill stat structure */
    stbuf->st_ino = inode->inode_number;
    stbuf->st_mode = inode->permissions;
    stbuf->st_nlink = inode->link_count;
    stbuf->st_uid = inode->uid;
    stbuf->st_gid = inode->gid;
    stbuf->st_mtime = (time_t)inode->mtime;
    stbuf->st_ctime = (time_t)inode->mtime;
    stbuf->st_atime = (time_t)inode->mtime;

    /* Set type-specific fields */
    switch (inode->type) {
    case SQFS_INODE_DIR:
    case SQFS_INODE_LDIR:
        stbuf->st_mode |= S_IFDIR;
        stbuf->st_size = inode->dir.size;
        stbuf->st_blksize = ctx->sb->disk.block_size;
        stbuf->st_blocks = (inode->dir.size + 511) / 512;
        break;

    case SQFS_INODE_FILE:
    case SQFS_INODE_LFILE:
        stbuf->st_mode |= S_IFREG;
        stbuf->st_size = (off_t)inode->file.file_size;
        stbuf->st_blksize = ctx->sb->disk.block_size;
        stbuf->st_blocks = (inode->file.file_size + 511) / 512;
        break;

    case SQFS_INODE_SYMLINK:
    case SQFS_INODE_LSYMLINK:
        stbuf->st_mode |= S_IFLNK;
        stbuf->st_size = inode->symlink.target_size;
        stbuf->st_blksize = ctx->sb->disk.block_size;
        break;

    case SQFS_INODE_BLKDEV:
    case SQFS_INODE_LBLKDEV:
        stbuf->st_mode |= S_IFBLK;
        stbuf->st_rdev = makedev(inode->dev.major, inode->dev.minor);
        break;

    case SQFS_INODE_CHRDEV:
    case SQFS_INODE_LCHRDEV:
        stbuf->st_mode |= S_IFCHR;
        stbuf->st_rdev = makedev(inode->dev.major, inode->dev.minor);
        break;

    case SQFS_INODE_FIFO:
    case SQFS_INODE_LFIFO:
        stbuf->st_mode |= S_IFIFO;
        break;

    case SQFS_INODE_SOCKET:
    case SQFS_INODE_LSOCKET:
        stbuf->st_mode |= S_IFSOCK;
        break;

    default:
        sqfs_inode_free(inode);
        return -EIO;
    }

    sqfs_inode_free(inode);
    return 0;
}

/*
 * readdir - Read directory contents
 */
static int sqfs_fuse_readdir(const char *path, void *buf,
                             fuse_fill_dir_t filler, off_t offset,
                             struct fuse_file_info *fi,
                             enum fuse_readdir_flags flags) {
    (void)offset;
    (void)fi;
    (void)flags;
    sqfs_fuse_ctx_t *ctx = (sqfs_fuse_ctx_t *)fuse_get_context()->private_data;
    int ret;
    sqfs_inode_t *inode = NULL;
    sqfs_dirent_t *entries = NULL;
    size_t entry_count = 0;

    /* Resolve path to inode */
    ret = resolve_path(ctx, path, &inode);
    if (ret != SQFS_OK) {
        return -sqfs_errno(ret);
    }

    /* Verify it's a directory */
    if (!sqfs_inode_is_dir(inode->type)) {
        sqfs_inode_free(inode);
        return -ENOTDIR;
    }

    /* Read directory entries */
    ret = sqfs_dir_read(ctx, inode, &entries, &entry_count);
    sqfs_inode_free(inode);

    if (ret != SQFS_OK) {
        return -sqfs_errno(ret);
    }

    /* Fill directory entries */
    for (size_t i = 0; i < entry_count; i++) {
        struct stat st;
        memset(&st, 0, sizeof(st));
        st.st_ino = entries[i].inode_number;

        /* Set file type in mode */
        switch (entries[i].type) {
        case SQFS_INODE_DIR:
        case SQFS_INODE_LDIR:
            st.st_mode = S_IFDIR;
            break;
        case SQFS_INODE_FILE:
        case SQFS_INODE_LFILE:
            st.st_mode = S_IFREG;
            break;
        case SQFS_INODE_SYMLINK:
        case SQFS_INODE_LSYMLINK:
            st.st_mode = S_IFLNK;
            break;
        case SQFS_INODE_BLKDEV:
        case SQFS_INODE_LBLKDEV:
            st.st_mode = S_IFBLK;
            break;
        case SQFS_INODE_CHRDEV:
        case SQFS_INODE_LCHRDEV:
            st.st_mode = S_IFCHR;
            break;
        case SQFS_INODE_FIFO:
        case SQFS_INODE_LFIFO:
            st.st_mode = S_IFIFO;
            break;
        case SQFS_INODE_SOCKET:
        case SQFS_INODE_LSOCKET:
            st.st_mode = S_IFSOCK;
            break;
        default:
            st.st_mode = S_IFREG;
            break;
        }

        filler(buf, entries[i].name, &st, 0, 0);
    }

    sqfs_dirent_free(entries, entry_count);
    return 0;
}

/*
 * open - Open a file
 */
static int sqfs_fuse_open(const char *path, struct fuse_file_info *fi) {
    sqfs_fuse_ctx_t *ctx = (sqfs_fuse_ctx_t *)fuse_get_context()->private_data;
    int ret;
    sqfs_inode_t *inode = NULL;
    sqfs_file_handle_t *fh = NULL;

    SQFS_LOG_DEBUG(SQFS_MOD_FUSE, "open called: path=%s, flags=0x%x", path, fi->flags);

    /* Resolve path to inode */
    ret = resolve_path(ctx, path, &inode);
    if (ret != SQFS_OK) {
        SQFS_LOG_DEBUG(SQFS_MOD_FUSE, "open: resolve_path failed for %s, ret=%d", path, ret);
        return -sqfs_errno(ret);
    }

    /* Verify it's a regular file */
    if (!sqfs_inode_is_file(inode->type)) {
        SQFS_LOG_DEBUG(SQFS_MOD_FUSE, "open: not a regular file, type=%d", inode->type);
        sqfs_inode_free(inode);
        return -EISDIR;
    }

    /* Check access mode (squashfs is read-only) */
    if ((fi->flags & O_ACCMODE) != O_RDONLY) {
        SQFS_LOG_DEBUG(SQFS_MOD_FUSE, "open: not read-only access");
        sqfs_inode_free(inode);
        return -EROFS;
    }

    /* Allocate file handle */
    fh = sqfs_malloc(sizeof(sqfs_file_handle_t));
    if (fh == NULL) {
        SQFS_LOG_ERROR(SQFS_MOD_FUSE, "open: failed to allocate file handle");
        sqfs_inode_free(inode);
        return -ENOMEM;
    }

    fh->inode = inode;
    fh->file_size = inode->file.file_size;

    fi->fh = (uint64_t)(uintptr_t)fh;

    SQFS_LOG_DEBUG(SQFS_MOD_FUSE, "open: success, file_size=%lu", (unsigned long)fh->file_size);

    return 0;
}

/*
 * read - Read from a file
 */
static int sqfs_fuse_read(const char *path, char *buf, size_t size,
                          off_t offset, struct fuse_file_info *fi) {
    (void)path;
    sqfs_fuse_ctx_t *ctx = (sqfs_fuse_ctx_t *)fuse_get_context()->private_data;
    sqfs_file_handle_t *fh = (sqfs_file_handle_t *)(uintptr_t)fi->fh;
    int ret;

    SQFS_LOG_DEBUG(SQFS_MOD_FUSE, "read called: size=%zu, offset=%lld", size, (long long)offset);

    if (fh == NULL || fh->inode == NULL) {
        SQFS_LOG_ERROR(SQFS_MOD_FUSE, "read: invalid file handle");
        return -EINVAL;
    }

    SQFS_LOG_DEBUG(SQFS_MOD_FUSE, "read: file_size=%lu", (unsigned long)fh->file_size);

    /* Check bounds */
    if ((uint64_t)offset >= fh->file_size) {
        SQFS_LOG_DEBUG(SQFS_MOD_FUSE, "read: offset past end of file");
        return 0;
    }

    /* Limit read to file size */
    if ((uint64_t)(offset + size) > fh->file_size) {
        size = (size_t)(fh->file_size - offset);
    }

    SQFS_LOG_DEBUG(SQFS_MOD_FUSE, "read: calling sqfs_data_read with size=%zu", size);

    /* Read data */
    ret = sqfs_data_read(ctx, fh->inode, (uint64_t)offset, buf, size);
    if (ret < 0) {
        SQFS_LOG_ERROR(SQFS_MOD_FUSE, "read: sqfs_data_read failed, ret=%d", ret);
        return -sqfs_errno(ret);
    }

    SQFS_LOG_DEBUG(SQFS_MOD_FUSE, "read: returned %d bytes", ret);
    return ret;
}

/*
 * release - Close a file
 */
static int sqfs_fuse_release(const char *path, struct fuse_file_info *fi) {
    (void)path;
    sqfs_file_handle_t *fh = (sqfs_file_handle_t *)(uintptr_t)fi->fh;

    if (fh != NULL) {
        if (fh->inode != NULL) {
            sqfs_inode_free(fh->inode);
        }
        sqfs_free(fh);
        fi->fh = 0;
    }

    return 0;
}

/*
 * readlink - Read symbolic link target
 */
static int sqfs_fuse_readlink(const char *path, char *buf, size_t size) {
    sqfs_fuse_ctx_t *ctx = (sqfs_fuse_ctx_t *)fuse_get_context()->private_data;
    int ret;
    sqfs_inode_t *inode = NULL;

    /* Resolve path to inode */
    ret = resolve_path(ctx, path, &inode);
    if (ret != SQFS_OK) {
        return -sqfs_errno(ret);
    }

    /* Verify it's a symlink */
    if (!sqfs_inode_is_symlink(inode->type)) {
        sqfs_inode_free(inode);
        return -EINVAL;
    }

    /* Copy target path */
    size_t copy_size = (inode->symlink.target_size < size - 1) ?
                       inode->symlink.target_size : size - 1;
    memcpy(buf, inode->symlink.target, copy_size);
    buf[copy_size] = '\0';

    sqfs_inode_free(inode);
    return 0;
}

/*
 * statfs - Get filesystem statistics
 */
static int sqfs_fuse_statfs(const char *path, struct statvfs *stbuf) {
    (void)path;
    sqfs_fuse_ctx_t *ctx = (sqfs_fuse_ctx_t *)fuse_get_context()->private_data;

    memset(stbuf, 0, sizeof(struct statvfs));

    stbuf->f_bsize = ctx->sb->disk.block_size;
    stbuf->f_frsize = ctx->sb->disk.block_size;
    stbuf->f_blocks = ctx->sb->disk.bytes_used / ctx->sb->disk.block_size;
    stbuf->f_bfree = 0;
    stbuf->f_bavail = 0;
    stbuf->f_files = ctx->sb->disk.inode_count;
    stbuf->f_ffree = 0;
    stbuf->f_favail = 0;
    stbuf->f_fsid = 0;
    stbuf->f_flag = ST_RDONLY | ST_NOSUID;
    stbuf->f_namemax = 256;

    return 0;
}

/*
 * getxattr - Get extended attribute
 */
static int sqfs_fuse_getxattr(const char *path, const char *name,
                              char *value, size_t size) {
    sqfs_fuse_ctx_t *ctx = (sqfs_fuse_ctx_t *)fuse_get_context()->private_data;
    int ret;
    sqfs_inode_t *inode = NULL;

    /* Check if filesystem has xattrs */
    if (!ctx->sb->has_xattrs) {
        return -ENOTSUP;
    }

    /* Resolve path to inode */
    ret = resolve_path(ctx, path, &inode);
    if (ret != SQFS_OK) {
        return -sqfs_errno(ret);
    }

    /* Get xattr index based on inode type */
    uint32_t xattr_idx = 0xFFFFFFFF;
    if (sqfs_inode_is_file(inode->type)) {
        xattr_idx = inode->file.xattr_idx;
    } else if (sqfs_inode_is_dir(inode->type)) {
        /* Extended directories have xattr_idx in their structure */
        if (inode->type == SQFS_INODE_LDIR) {
            /* xattr_idx is stored in the xattr union field */
            xattr_idx = inode->xattr_idx;
        }
    } else if (sqfs_inode_is_symlink(inode->type)) {
        if (inode->type == SQFS_INODE_LSYMLINK) {
            xattr_idx = inode->xattr_idx;
        }
    } else {
        xattr_idx = inode->xattr_idx;
    }

    sqfs_inode_free(inode);

    /* No xattrs for this inode */
    if (xattr_idx == 0xFFFFFFFF) {
        return -ENODATA;
    }

    /* Get xattr value */
    size_t out_size;
    ret = sqfs_xattr_get(ctx, xattr_idx, name, value, size, &out_size);
    if (ret < 0) {
        return -sqfs_errno(ret);
    }

    return (int)out_size;
}

/*
 * listxattr - List extended attributes
 */
static int sqfs_fuse_listxattr(const char *path, char *list, size_t size) {
    sqfs_fuse_ctx_t *ctx = (sqfs_fuse_ctx_t *)fuse_get_context()->private_data;
    int ret;
    sqfs_inode_t *inode = NULL;

    /* Check if filesystem has xattrs */
    if (!ctx->sb->has_xattrs) {
        return -ENOTSUP;
    }

    /* Resolve path to inode */
    ret = resolve_path(ctx, path, &inode);
    if (ret != SQFS_OK) {
        return -sqfs_errno(ret);
    }

    /* Get xattr index based on inode type */
    uint32_t xattr_idx = 0xFFFFFFFF;
    if (sqfs_inode_is_file(inode->type)) {
        xattr_idx = inode->file.xattr_idx;
    } else if (sqfs_inode_is_dir(inode->type)) {
        if (inode->type == SQFS_INODE_LDIR) {
            xattr_idx = inode->xattr_idx;
        }
    } else if (sqfs_inode_is_symlink(inode->type)) {
        if (inode->type == SQFS_INODE_LSYMLINK) {
            xattr_idx = inode->xattr_idx;
        }
    } else {
        xattr_idx = inode->xattr_idx;
    }

    sqfs_inode_free(inode);

    /* No xattrs for this inode */
    if (xattr_idx == 0xFFFFFFFF) {
        return 0;
    }

    /* List xattrs */
    size_t out_size;
    ret = sqfs_xattr_list(ctx, xattr_idx, list, size, &out_size);
    if (ret < 0) {
        return -sqfs_errno(ret);
    }

    return (int)out_size;
}

/* ============================================================================
 * FUSE Operations Structure
 * ============================================================================ */

static const struct fuse_operations sqfs_fuse_oper = {
    .getattr    = sqfs_fuse_getattr,
    .readdir    = sqfs_fuse_readdir,
    .open       = sqfs_fuse_open,
    .read       = sqfs_fuse_read,
    .release    = sqfs_fuse_release,
    .readlink   = sqfs_fuse_readlink,
    .statfs     = sqfs_fuse_statfs,
    .getxattr   = sqfs_fuse_getxattr,
    .listxattr  = sqfs_fuse_listxattr,
};

/* ============================================================================
 * Context Initialization and Cleanup
 * ============================================================================ */

static int init_context(sqfs_fuse_ctx_t *ctx, int fd,
                        size_t cache_size_mb, int no_cache) {
    int ret;

    memset(ctx, 0, sizeof(*ctx));
    ctx->no_cache = no_cache;
    ctx->debug_level = 0;

    /* Load superblock */
    ctx->sb = sqfs_malloc(sizeof(sqfs_superblock_t));
    if (ctx->sb == NULL) {
        return SQFS_ERR_NO_MEMORY;
    }

    ret = sqfs_superblock_load(fd, ctx->sb);
    if (ret != SQFS_OK) {
        sqfs_free(ctx->sb);
        ctx->sb = NULL;
        return ret;
    }

    /* Initialize compressor */
    ctx->comp = sqfs_compressor_create((sqfs_compressor_id_t)ctx->sb->disk.compressor);
    if (ctx->comp == NULL) {
        sqfs_superblock_destroy(ctx->sb);
        sqfs_free(ctx->sb);
        ctx->sb = NULL;
        return SQFS_ERR_BAD_COMP;
    }

    /* Calculate cache sizes */
    size_t total_cache = cache_size_mb * 1024 * 1024;
    size_t inode_mem = total_cache / 8;
    size_t dir_mem = total_cache / 16;
    size_t meta_mem = total_cache / 4;
    size_t data_mem = total_cache / 2;

    /* Initialize caches if not disabled */
    if (!no_cache) {
        ret = sqfs_cache_init(&ctx->inode_cache,
                              CACHE_INODE_MAX_ENTRIES, inode_mem,
                              (cache_free_fn)sqfs_inode_free);
        if (ret != 0) {
            goto error;
        }

        ret = sqfs_cache_init(&ctx->dir_cache,
                              CACHE_DIR_MAX_ENTRIES, dir_mem, NULL);
        if (ret != 0) {
            goto error;
        }

        ret = sqfs_cache_init(&ctx->meta_cache,
                              CACHE_META_MAX_ENTRIES, meta_mem, NULL);
        if (ret != 0) {
            goto error;
        }

        ret = sqfs_cache_init(&ctx->data_cache,
                              CACHE_DATA_MAX_ENTRIES, data_mem, NULL);
        if (ret != 0) {
            goto error;
        }
    }

    /* Load ID table */
    ret = load_id_table(ctx);
    if (ret != SQFS_OK) {
        goto error;
    }

    /* Initialize and load fragment table if filesystem has fragments */
    if (ctx->sb->has_fragments) {
        ctx->fragment_table = sqfs_malloc(sizeof(sqfs_fragment_table_t));
        if (ctx->fragment_table == NULL) {
            ret = SQFS_ERR_NO_MEMORY;
            goto error;
        }
        sqfs_fragment_table_init(ctx->fragment_table);

        ret = sqfs_fragment_table_load(ctx);
        if (ret != SQFS_OK) {
            SQFS_LOG("Warning: Failed to load fragment table: %d", ret);
            /* Non-fatal - some operations may fail for fragmented files */
            ctx->fragment_table_loaded = false;
        } else {
            ctx->fragment_table_loaded = true;
        }
    }

    SQFS_LOG("Context initialized: block_size=%u, compressor=%d",
             ctx->sb->disk.block_size, ctx->sb->disk.compressor);

    return SQFS_OK;

error:
    if (!no_cache) {
        sqfs_cache_destroy(&ctx->inode_cache);
        sqfs_cache_destroy(&ctx->dir_cache);
        sqfs_cache_destroy(&ctx->meta_cache);
        sqfs_cache_destroy(&ctx->data_cache);
    }
    sqfs_compressor_destroy(ctx->comp);
    sqfs_superblock_destroy(ctx->sb);
    sqfs_free(ctx->sb);
    ctx->sb = NULL;
    return ret;
}

static void destroy_context(sqfs_fuse_ctx_t *ctx) {
    if (ctx == NULL) {
        return;
    }

    if (ctx->id_table != NULL) {
        sqfs_free(ctx->id_table);
        ctx->id_table = NULL;
    }

    if (!ctx->no_cache) {
        sqfs_cache_destroy(&ctx->inode_cache);
        sqfs_cache_destroy(&ctx->dir_cache);
        sqfs_cache_destroy(&ctx->meta_cache);
        sqfs_cache_destroy(&ctx->data_cache);
    }

    if (ctx->comp != NULL) {
        sqfs_compressor_destroy(ctx->comp);
        ctx->comp = NULL;
    }

    if (ctx->sb != NULL) {
        sqfs_superblock_destroy(ctx->sb);
        sqfs_free(ctx->sb);
        ctx->sb = NULL;
    }
}

/* ============================================================================
 * Main Function
 * ============================================================================ */

static void print_usage(void) {
    fprintf(stderr, usage_string, DEFAULT_CACHE_SIZE_MB);
}

static void print_version(void) {
    printf("squashfs-fuse version %s\n", SQFS_FUSE_VERSION_STRING);
    printf("Using FUSE %d.%d\n", FUSE_MAJOR_VERSION, FUSE_MINOR_VERSION);
    printf("Supported compressors: gzip, zstd\n");
}

/* Global context for FUSE operations */
static sqfs_fuse_ctx_t g_ctx;
static int g_fd = -1;
static char *g_image_file = NULL;
static size_t g_cache_size_mb = DEFAULT_CACHE_SIZE_MB;
static int g_no_cache = 0;

/* Initialization function called before FUSE main */
static void *sqfs_fuse_init(struct fuse_conn_info *conn,
                            struct fuse_config *cfg) {
    (void)conn;
    cfg->kernel_cache = 1;
    cfg->use_ino = 1;  /* Honor st_ino field for correct hardlink support */
    return &g_ctx;
}

/* Cleanup on exit */
static void sqfs_fuse_destroy(void *private_data) {
    (void)private_data;
    SQFS_LOG_INFO(SQFS_MOD_FUSE, "unmounting filesystem");
    sqfs_stats_report();
    destroy_context(&g_ctx);
    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
    }
    sqfs_log_destroy();
}

int main(int argc, char *argv[]) {
    struct fuse_args args = FUSE_ARGS_INIT(0, NULL);
    int ret;
    int i;
    char *mount_point = NULL;

    /* Initialize logging early */
    sqfs_log_config_t log_config = {
        .path = NULL,
        .max_size = 0,
        .level = SQFS_LOG_LEVEL,
    };
    sqfs_log_init(&log_config);
    sqfs_stats_init();

    /* First pass: identify image file and mount point, handle custom options */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--nocache") == 0) {
            g_no_cache = 1;
        } else if (strncmp(argv[i], "--cache-size=", 13) == 0) {
            g_cache_size_mb = (size_t)atoi(argv[i] + 13);
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage();
            sqfs_log_destroy();
            return 0;
        } else if (strcmp(argv[i], "-V") == 0 || strcmp(argv[i], "--version") == 0) {
            print_version();
            sqfs_log_destroy();
            return 0;
        } else if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--log") == 0) {
            i++;  /* Skip the log path argument */
        } else if (strncmp(argv[i], "-l", 2) == 0 && strlen(argv[i]) > 2) {
            /* -l/path format, skip */
        } else if (argv[i][0] != '-') {
            /* Positional argument - could be image or mount point */
            if (g_image_file == NULL) {
                g_image_file = strdup(argv[i]);
            } else if (mount_point == NULL) {
                mount_point = strdup(argv[i]);
            }
        }
    }

    /* Check that we have an image file and mount point */
    if (g_image_file == NULL) {
        fprintf(stderr, "Error: missing image file\n");
        print_usage();
        sqfs_log_destroy();
        return 1;
    }
    if (mount_point == NULL) {
        fprintf(stderr, "Error: missing mount point\n");
        print_usage();
        sqfs_log_destroy();
        return 1;
    }

    /* Second pass: build FUSE args - skip our custom options and the image file */
    fuse_opt_add_arg(&args, argv[0]);  /* Program name */

    for (i = 1; i < argc; i++) {
        /* Skip our custom options */
        if (strcmp(argv[i], "--nocache") == 0 ||
            strncmp(argv[i], "--cache-size=", 13) == 0) {
            continue;
        }
        if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--log") == 0) {
            i++;  /* Skip the log path argument */
            continue;
        }
        if (strncmp(argv[i], "-l", 2) == 0 && strlen(argv[i]) > 2) {
            continue;
        }

        /* Skip the image file - only pass mount point to FUSE */
        if (strcmp(argv[i], g_image_file) == 0) {
            continue;
        }

        /* This is a FUSE argument - add it */
        fuse_opt_add_arg(&args, argv[i]);
    }

    SQFS_LOG_INFO(SQFS_MOD_FUSE, "opening image: %s", g_image_file);

    /* Open the SquashFS image */
    g_fd = open(g_image_file, O_RDONLY);
    if (g_fd < 0) {
        fprintf(stderr, "Error: cannot open '%s': %s\n", g_image_file, strerror(errno));
        SQFS_LOG_ERROR(SQFS_MOD_FUSE, "cannot open image: %s", strerror(errno));
        fuse_opt_free_args(&args);
        sqfs_log_destroy();
        return 1;
    }

    /* Initialize context */
    ret = init_context(&g_ctx, g_fd, g_cache_size_mb, g_no_cache);
    if (ret != SQFS_OK) {
        fprintf(stderr, "Error: failed to initialize context: %s\n",
                strerror(sqfs_errno(ret)));
        close(g_fd);
        fuse_opt_free_args(&args);
        sqfs_log_destroy();
        return 1;
    }

    SQFS_LOG_INFO(SQFS_MOD_FUSE, "filesystem initialized");

    /* Add init and destroy operations */
    struct fuse_operations sqfs_oper = sqfs_fuse_oper;
    sqfs_oper.init = sqfs_fuse_init;
    sqfs_oper.destroy = sqfs_fuse_destroy;

    /* Add default mount options */
    fuse_opt_add_arg(&args, "-oro");
    fuse_opt_add_arg(&args, "-onosuid");
    fuse_opt_add_arg(&args, "-onodev");

    /* Print info */
    printf("SquashFS-FUSE %s\n", SQFS_FUSE_VERSION_STRING);
    printf("Image: %s\n", g_image_file);
    printf("Block size: %u bytes\n", g_ctx.sb->disk.block_size);
    printf("Inodes: %u\n", g_ctx.sb->disk.inode_count);
    printf("Press Ctrl+C to unmount\n\n");
    fflush(stdout);

    /* Run FUSE */
    ret = fuse_main(args.argc, args.argv, &sqfs_oper, NULL);

    fuse_opt_free_args(&args);
    free(g_image_file);
    free(mount_point);

    return ret;
}