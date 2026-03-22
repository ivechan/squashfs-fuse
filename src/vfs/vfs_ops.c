/*
 * SquashFS - VFS Operations Implementation
 *
 * Implements VFS-agnostic operations that can be used by different backends.
 *
 * Copyright (c) 2024 SquashFS Authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include "vfs.h"
#include "context.h"
#include "inode.h"
#include "directory.h"
#include "data.h"
#include "fragment.h"
#include "xattr.h"
#include "log.h"
#include "utils.h"

/* ============================================================================
 * Helper Functions
 * ============================================================================ */

/*
 * Get errno from SquashFS error code.
 */
static int sqfs_err_to_errno(int err) {
    switch (err) {
    case SQFS_OK:          return 0;
    case SQFS_ERR_NOT_FOUND: return ENOENT;
    case SQFS_ERR_CORRUPT:   return EIO;
    case SQFS_ERR_NO_MEMORY: return ENOMEM;
    case SQFS_ERR_IO:        return EIO;
    case SQFS_ERR_BAD_INODE: return EIO;
    case SQFS_ERR_BAD_DIR:   return EIO;
    default:                 return EIO;
    }
}

/* ============================================================================
 * Path Resolution
 * ============================================================================ */

/*
 * Resolve a path to an inode.
 * Walks the directory tree from root to find the inode for a given path.
 */
int sqfs_vfs_resolve_path(sqfs_ctx_t *ctx, const char *path,
                          struct sqfs_inode **out_inode) {
    int ret;
    sqfs_inode_t *inode = NULL;
    char *path_copy = NULL;
    char *component = NULL;
    char *saveptr = NULL;

    SQFS_LOG_DEBUG(SQFS_MOD_FUSE, "resolve_path: path=%s", path);

    if (path == NULL || out_inode == NULL) {
        return SQFS_VFS_ERR_INVAL;
    }

    *out_inode = NULL;

    /* Start from root inode */
    uint64_t root_ref = ctx->sb->disk.root_inode;
    SQFS_LOG_DEBUG(SQFS_MOD_FUSE, "resolve_path: root_ref=0x%lx", (unsigned long)root_ref);

    ret = sqfs_inode_get_by_ref(ctx, root_ref, &inode);
    if (ret != SQFS_OK) {
        SQFS_LOG_ERROR(SQFS_MOD_FUSE, "resolve_path: failed to get root inode, ret=%d", ret);
        return -sqfs_err_to_errno(ret);
    }

    SQFS_LOG_DEBUG(SQFS_MOD_FUSE, "resolve_path: got root inode, type=%d", inode->type);

    /* Handle root path */
    if (path[0] == '/' && path[1] == '\0') {
        SQFS_LOG_DEBUG(SQFS_MOD_FUSE, "resolve_path: returning root inode");
        *out_inode = inode;
        return SQFS_VFS_OK;
    }

    /* Make a copy for tokenization */
    path_copy = malloc(strlen(path) + 1);
    if (path_copy == NULL) {
        sqfs_inode_free(inode);
        return SQFS_VFS_ERR_NOMEM;
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
            free(path_copy);
            return SQFS_VFS_ERR_NOT_DIR;
        }

        /* Read directory entries */
        sqfs_dirent_t *entries = NULL;
        size_t entry_count = 0;

        ret = sqfs_dir_read(ctx, inode, &entries, &entry_count);
        sqfs_inode_free(inode);
        inode = NULL;

        if (ret != SQFS_OK) {
            SQFS_LOG_ERROR(SQFS_MOD_FUSE, "resolve_path: sqfs_dir_read failed, ret=%d", ret);
            free(path_copy);
            return -sqfs_err_to_errno(ret);
        }

        SQFS_LOG_DEBUG(SQFS_MOD_FUSE, "resolve_path: directory has %zu entries", entry_count);

        /* Find matching entry */
        int found = 0;
        for (size_t i = 0; i < entry_count; i++) {
            if (strcmp(entries[i].name, component) == 0) {
                /* Load the inode by reference */
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
            free(path_copy);
            return SQFS_VFS_ERR_NOT_FOUND;
        }

        component = strtok_r(NULL, "/", &saveptr);
    }

    free(path_copy);

    if (inode == NULL) {
        return SQFS_VFS_ERR_NOT_FOUND;
    }

    *out_inode = inode;
    return SQFS_VFS_OK;
}

/* ============================================================================
 * Attribute Operations
 * ============================================================================ */

/*
 * Fill VFS attributes from an inode.
 */
int sqfs_vfs_fill_attr(sqfs_ctx_t *ctx, struct sqfs_inode *inode,
                       sqfs_vfs_attr_t *attr) {
    if (inode == NULL || attr == NULL) {
        return SQFS_VFS_ERR_INVAL;
    }

    memset(attr, 0, sizeof(*attr));

    attr->ino = inode->inode_number;
    attr->mode = inode->permissions;
    attr->nlink = inode->link_count;
    attr->uid = inode->uid;
    attr->gid = inode->gid;
    attr->mtime = inode->mtime;
    attr->ctime = inode->mtime;
    attr->atime = inode->mtime;

    /* Set type-specific fields */
    switch (inode->type) {
    case SQFS_INODE_DIR:
    case SQFS_INODE_LDIR:
        attr->mode |= 0040000;  /* S_IFDIR */
        attr->size = inode->dir.size;
        attr->blksize = ctx->sb->disk.block_size;
        attr->blocks = (inode->dir.size + 511) / 512;
        break;

    case SQFS_INODE_FILE:
    case SQFS_INODE_LFILE:
        attr->mode |= 0100000;  /* S_IFREG */
        attr->size = inode->file.file_size;
        attr->blksize = ctx->sb->disk.block_size;
        attr->blocks = (inode->file.file_size + 511) / 512;
        break;

    case SQFS_INODE_SYMLINK:
    case SQFS_INODE_LSYMLINK:
        attr->mode |= 0120000;  /* S_IFLNK */
        attr->size = inode->symlink.target_size;
        attr->blksize = ctx->sb->disk.block_size;
        break;

    case SQFS_INODE_BLKDEV:
    case SQFS_INODE_LBLKDEV:
        attr->mode |= 0060000;  /* S_IFBLK */
        attr->rdev_major = inode->dev.major;
        attr->rdev_minor = inode->dev.minor;
        break;

    case SQFS_INODE_CHRDEV:
    case SQFS_INODE_LCHRDEV:
        attr->mode |= 0020000;  /* S_IFCHR */
        attr->rdev_major = inode->dev.major;
        attr->rdev_minor = inode->dev.minor;
        break;

    case SQFS_INODE_FIFO:
    case SQFS_INODE_LFIFO:
        attr->mode |= 0010000;  /* S_IFIFO */
        break;

    case SQFS_INODE_SOCKET:
    case SQFS_INODE_LSOCKET:
        attr->mode |= 0140000;  /* S_IFSOCK */
        break;

    default:
        return SQFS_VFS_ERR_IO;
    }

    return SQFS_VFS_OK;
}

/*
 * Get file attributes by path.
 */
int sqfs_vfs_getattr(sqfs_ctx_t *ctx, const char *path,
                     sqfs_vfs_attr_t *attr) {
    sqfs_inode_t *inode = NULL;
    int ret;

    if (attr == NULL) {
        return SQFS_VFS_ERR_INVAL;
    }

    ret = sqfs_vfs_resolve_path(ctx, path, &inode);
    if (ret != SQFS_VFS_OK) {
        return ret;
    }

    ret = sqfs_vfs_fill_attr(ctx, inode, attr);
    sqfs_inode_free(inode);

    return ret;
}

/* ============================================================================
 * File Operations
 * ============================================================================ */

/*
 * Open a file.
 */
int sqfs_vfs_open(sqfs_ctx_t *ctx, const char *path, sqfs_vfs_fh_t **fh) {
    sqfs_inode_t *inode = NULL;
    sqfs_vfs_fh_t *handle = NULL;
    int ret;

    if (fh == NULL) {
        return SQFS_VFS_ERR_INVAL;
    }

    *fh = NULL;

    ret = sqfs_vfs_resolve_path(ctx, path, &inode);
    if (ret != SQFS_VFS_OK) {
        return ret;
    }

    /* Verify it's a regular file */
    if (!sqfs_inode_is_file(inode->type)) {
        sqfs_inode_free(inode);
        return SQFS_VFS_ERR_IS_DIR;
    }

    /* Allocate file handle */
    handle = malloc(sizeof(sqfs_vfs_fh_t));
    if (handle == NULL) {
        sqfs_inode_free(inode);
        return SQFS_VFS_ERR_NOMEM;
    }

    handle->inode = inode;
    handle->file_size = inode->file.file_size;

    *fh = handle;
    return SQFS_VFS_OK;
}

/*
 * Read from an open file.
 */
int sqfs_vfs_read(sqfs_ctx_t *ctx, sqfs_vfs_fh_t *fh,
                  void *buf, size_t size, uint64_t offset) {
    sqfs_inode_t *inode;
    int ret;

    if (fh == NULL || fh->inode == NULL || buf == NULL) {
        return SQFS_VFS_ERR_INVAL;
    }

    inode = (sqfs_inode_t *)fh->inode;

    /* Check bounds */
    if (offset >= fh->file_size) {
        return 0;  /* EOF */
    }

    /* Limit read to file size */
    if (offset + size > fh->file_size) {
        size = (size_t)(fh->file_size - offset);
    }

    /* Read data */
    ret = sqfs_data_read(ctx, inode, offset, buf, size);
    if (ret < 0) {
        return -sqfs_err_to_errno(ret);
    }

    return ret;
}

/*
 * Close an open file.
 */
void sqfs_vfs_release(sqfs_ctx_t *ctx, sqfs_vfs_fh_t *fh) {
    (void)ctx;

    if (fh != NULL) {
        if (fh->inode != NULL) {
            sqfs_inode_free((sqfs_inode_t *)fh->inode);
        }
        free(fh);
    }
}

/* ============================================================================
 * Directory Operations
 * ============================================================================ */

/*
 * Read directory entries.
 */
int sqfs_vfs_readdir(sqfs_ctx_t *ctx, const char *path,
                     sqfs_vfs_dirent_t **entries, size_t *count) {
    sqfs_inode_t *inode = NULL;
    sqfs_dirent_t *internal_entries = NULL;
    size_t internal_count = 0;
    sqfs_vfs_dirent_t *vfs_entries = NULL;
    int ret;

    if (entries == NULL || count == NULL) {
        return SQFS_VFS_ERR_INVAL;
    }

    *entries = NULL;
    *count = 0;

    ret = sqfs_vfs_resolve_path(ctx, path, &inode);
    if (ret != SQFS_VFS_OK) {
        return ret;
    }

    /* Verify it's a directory */
    if (!sqfs_inode_is_dir(inode->type)) {
        sqfs_inode_free(inode);
        return SQFS_VFS_ERR_NOT_DIR;
    }

    /* Read directory entries */
    ret = sqfs_dir_read(ctx, inode, &internal_entries, &internal_count);
    sqfs_inode_free(inode);

    if (ret != SQFS_OK) {
        return -sqfs_err_to_errno(ret);
    }

    /* Convert to VFS entries */
    vfs_entries = malloc(internal_count * sizeof(sqfs_vfs_dirent_t));
    if (vfs_entries == NULL) {
        sqfs_dirent_free(internal_entries, internal_count);
        return SQFS_VFS_ERR_NOMEM;
    }

    for (size_t i = 0; i < internal_count; i++) {
        vfs_entries[i].name = internal_entries[i].name;  /* Transfer ownership */
        vfs_entries[i].ino = internal_entries[i].inode_number;
        vfs_entries[i].type = 0;  /* Will be set below */

        /* Set file type */
        switch (internal_entries[i].type) {
        case SQFS_INODE_DIR:
        case SQFS_INODE_LDIR:
            vfs_entries[i].type = 0040000;  /* S_IFDIR */
            break;
        case SQFS_INODE_FILE:
        case SQFS_INODE_LFILE:
            vfs_entries[i].type = 0100000;  /* S_IFREG */
            break;
        case SQFS_INODE_SYMLINK:
        case SQFS_INODE_LSYMLINK:
            vfs_entries[i].type = 0120000;  /* S_IFLNK */
            break;
        case SQFS_INODE_BLKDEV:
        case SQFS_INODE_LBLKDEV:
            vfs_entries[i].type = 0060000;  /* S_IFBLK */
            break;
        case SQFS_INODE_CHRDEV:
        case SQFS_INODE_LCHRDEV:
            vfs_entries[i].type = 0020000;  /* S_IFCHR */
            break;
        case SQFS_INODE_FIFO:
        case SQFS_INODE_LFIFO:
            vfs_entries[i].type = 0010000;  /* S_IFIFO */
            break;
        case SQFS_INODE_SOCKET:
        case SQFS_INODE_LSOCKET:
            vfs_entries[i].type = 0140000;  /* S_IFSOCK */
            break;
        default:
            vfs_entries[i].type = 0100000;  /* S_IFREG */
            break;
        }
    }

    /* Free the internal entries array but not the names (transferred ownership) */
    free(internal_entries);

    *entries = vfs_entries;
    *count = internal_count;
    return SQFS_VFS_OK;
}

/*
 * Free directory entries array.
 */
void sqfs_vfs_dirent_free(sqfs_vfs_dirent_t *entries, size_t count) {
    if (entries == NULL) {
        return;
    }

    for (size_t i = 0; i < count; i++) {
        free(entries[i].name);
    }
    free(entries);
}

/* ============================================================================
 * Symbolic Link Operations
 * ============================================================================ */

/*
 * Read symbolic link target.
 */
int sqfs_vfs_readlink(sqfs_ctx_t *ctx, const char *path,
                      char *buf, size_t size) {
    sqfs_inode_t *inode = NULL;
    int ret;

    if (buf == NULL || size == 0) {
        return SQFS_VFS_ERR_INVAL;
    }

    ret = sqfs_vfs_resolve_path(ctx, path, &inode);
    if (ret != SQFS_VFS_OK) {
        return ret;
    }

    /* Verify it's a symlink */
    if (!sqfs_inode_is_symlink(inode->type)) {
        sqfs_inode_free(inode);
        return SQFS_VFS_ERR_INVAL;
    }

    /* Copy target path */
    size_t copy_size = (inode->symlink.target_size < size - 1) ?
                       inode->symlink.target_size : size - 1;
    memcpy(buf, inode->symlink.target, copy_size);
    buf[copy_size] = '\0';

    sqfs_inode_free(inode);
    return SQFS_VFS_OK;
}

/* ============================================================================
 * Filesystem Information
 * ============================================================================ */

/*
 * Get filesystem statistics.
 */
int sqfs_vfs_statfs(sqfs_ctx_t *ctx, sqfs_vfs_statfs_t *stat) {
    if (stat == NULL) {
        return SQFS_VFS_ERR_INVAL;
    }

    memset(stat, 0, sizeof(*stat));

    stat->bsize = ctx->sb->disk.block_size;
    stat->frsize = ctx->sb->disk.block_size;
    stat->blocks = ctx->sb->disk.bytes_used / ctx->sb->disk.block_size;
    stat->bfree = 0;
    stat->bavail = 0;
    stat->files = ctx->sb->disk.inode_count;
    stat->ffree = 0;
    stat->favail = 0;
    stat->fsid = 0;
    stat->flags = 1;  /* ST_RDONLY */
    stat->namemax = 256;

    return SQFS_VFS_OK;
}

/* ============================================================================
 * Extended Attributes
 * ============================================================================ */

/*
 * Get xattr index from inode based on type.
 */
static uint32_t get_xattr_idx_from_inode(sqfs_inode_t *inode) {
    if (inode == NULL) {
        return 0xFFFFFFFF;
    }

    if (sqfs_inode_is_file(inode->type)) {
        return inode->file.xattr_idx;
    } else if (inode->type == SQFS_INODE_LDIR) {
        return inode->xattr_idx;
    } else if (inode->type == SQFS_INODE_LSYMLINK) {
        return inode->xattr_idx;
    } else {
        return inode->xattr_idx;
    }
}

/*
 * Get extended attribute value.
 */
int sqfs_vfs_getxattr(sqfs_ctx_t *ctx, const char *path,
                      const char *name, void *value, size_t size) {
    sqfs_inode_t *inode = NULL;
    uint32_t xattr_idx;
    size_t out_size;
    int ret;

    if (name == NULL) {
        return SQFS_VFS_ERR_INVAL;
    }

    /* Check if filesystem has xattrs */
    if (!ctx->sb->has_xattrs) {
        return SQFS_VFS_ERR_NOSUP;
    }

    ret = sqfs_vfs_resolve_path(ctx, path, &inode);
    if (ret != SQFS_VFS_OK) {
        return ret;
    }

    xattr_idx = get_xattr_idx_from_inode(inode);
    sqfs_inode_free(inode);

    /* No xattrs for this inode */
    if (xattr_idx == 0xFFFFFFFF) {
        return SQFS_VFS_ERR_NODATA;
    }

    /* Get xattr value */
    ret = sqfs_xattr_get(ctx, xattr_idx, name, value, size, &out_size);
    if (ret < 0) {
        return -sqfs_err_to_errno(ret);
    }

    return (int)out_size;
}

/*
 * List extended attribute names.
 */
int sqfs_vfs_listxattr(sqfs_ctx_t *ctx, const char *path,
                       char *list, size_t size) {
    sqfs_inode_t *inode = NULL;
    uint32_t xattr_idx;
    size_t out_size;
    int ret;

    /* Check if filesystem has xattrs */
    if (!ctx->sb->has_xattrs) {
        return SQFS_VFS_ERR_NOSUP;
    }

    ret = sqfs_vfs_resolve_path(ctx, path, &inode);
    if (ret != SQFS_VFS_OK) {
        return ret;
    }

    xattr_idx = get_xattr_idx_from_inode(inode);
    sqfs_inode_free(inode);

    /* No xattrs for this inode */
    if (xattr_idx == 0xFFFFFFFF) {
        return 0;
    }

    /* List xattrs */
    ret = sqfs_xattr_list(ctx, xattr_idx, list, size, &out_size);
    if (ret < 0) {
        return -sqfs_err_to_errno(ret);
    }

    return (int)out_size;
}