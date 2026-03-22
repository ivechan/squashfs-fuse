/*
 * SquashFS - FUSE VFS Adapter
 *
 * Implements FUSE operations using the VFS abstraction layer.
 *
 * Copyright (c) 2024 SquashFS Authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define FUSE_USE_VERSION 31
#define _POSIX_C_SOURCE 200809L

#include <fuse3/fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/sysmacros.h>

#include "vfs.h"
#include "context.h"
#include "log.h"

/* ============================================================================
 * FUSE Operation Wrappers
 * ============================================================================ */

/*
 * getattr - Get file attributes
 */
int sqfs_fuse_getattr(const char *path, struct stat *stbuf,
                      struct fuse_file_info *fi) {
    (void)fi;
    sqfs_ctx_t *ctx = (sqfs_ctx_t *)fuse_get_context()->private_data;
    sqfs_vfs_attr_t attr;
    int ret;

    SQFS_LOG_DEBUG(SQFS_MOD_FUSE, "getattr called: path=%s", path);

    if (stbuf == NULL) {
        return -EINVAL;
    }

    memset(stbuf, 0, sizeof(struct stat));

    ret = sqfs_vfs_getattr(ctx, path, &attr);
    if (ret != 0) {
        SQFS_LOG_DEBUG(SQFS_MOD_FUSE, "getattr: sqfs_vfs_getattr failed for %s, ret=%d", path, ret);
        return ret;
    }

    /* Convert to struct stat */
    stbuf->st_ino = attr.ino;
    stbuf->st_mode = attr.mode;
    stbuf->st_nlink = attr.nlink;
    stbuf->st_uid = attr.uid;
    stbuf->st_gid = attr.gid;
    stbuf->st_size = (off_t)attr.size;
    stbuf->st_blksize = (blksize_t)attr.blksize;
    stbuf->st_blocks = (blkcnt_t)attr.blocks;
    stbuf->st_mtime = (time_t)attr.mtime;
    stbuf->st_ctime = (time_t)attr.ctime;
    stbuf->st_atime = (time_t)attr.atime;

    if (S_ISBLK(attr.mode) || S_ISCHR(attr.mode)) {
        stbuf->st_rdev = makedev(attr.rdev_major, attr.rdev_minor);
    }

    return 0;
}

/*
 * readdir - Read directory contents
 */
int sqfs_fuse_readdir(const char *path, void *buf,
                      fuse_fill_dir_t filler, off_t offset,
                      struct fuse_file_info *fi,
                      enum fuse_readdir_flags flags) {
    (void)offset;
    (void)fi;
    (void)flags;
    sqfs_ctx_t *ctx = (sqfs_ctx_t *)fuse_get_context()->private_data;
    sqfs_vfs_dirent_t *entries = NULL;
    size_t count = 0;
    int ret;

    ret = sqfs_vfs_readdir(ctx, path, &entries, &count);
    if (ret != 0) {
        return ret;
    }

    for (size_t i = 0; i < count; i++) {
        struct stat st;
        memset(&st, 0, sizeof(st));
        st.st_ino = entries[i].ino;
        st.st_mode = entries[i].type;

        filler(buf, entries[i].name, &st, 0, 0);
    }

    sqfs_vfs_dirent_free(entries, count);
    return 0;
}

/*
 * open - Open a file
 */
int sqfs_fuse_open(const char *path, struct fuse_file_info *fi) {
    sqfs_ctx_t *ctx = (sqfs_ctx_t *)fuse_get_context()->private_data;
    sqfs_vfs_fh_t *fh = NULL;
    int ret;

    SQFS_LOG_DEBUG(SQFS_MOD_FUSE, "open called: path=%s, flags=0x%x", path, fi->flags);

    ret = sqfs_vfs_open(ctx, path, &fh);
    if (ret != 0) {
        SQFS_LOG_DEBUG(SQFS_MOD_FUSE, "open: sqfs_vfs_open failed for %s, ret=%d", path, ret);
        return ret;
    }

    fi->fh = (uint64_t)(uintptr_t)fh;
    SQFS_LOG_DEBUG(SQFS_MOD_FUSE, "open: success, file_size=%lu", (unsigned long)fh->file_size);

    return 0;
}

/*
 * read - Read from a file
 */
int sqfs_fuse_read(const char *path, char *buf, size_t size,
                   off_t offset, struct fuse_file_info *fi) {
    (void)path;
    sqfs_ctx_t *ctx = (sqfs_ctx_t *)fuse_get_context()->private_data;
    sqfs_vfs_fh_t *fh = (sqfs_vfs_fh_t *)(uintptr_t)fi->fh;

    SQFS_LOG_DEBUG(SQFS_MOD_FUSE, "read called: size=%zu, offset=%lld", size, (long long)offset);

    if (fh == NULL) {
        SQFS_LOG_ERROR(SQFS_MOD_FUSE, "read: invalid file handle");
        return -EINVAL;
    }

    return sqfs_vfs_read(ctx, fh, buf, size, (uint64_t)offset);
}

/*
 * release - Close a file
 */
int sqfs_fuse_release(const char *path, struct fuse_file_info *fi) {
    (void)path;
    sqfs_ctx_t *ctx = (sqfs_ctx_t *)fuse_get_context()->private_data;
    sqfs_vfs_fh_t *fh = (sqfs_vfs_fh_t *)(uintptr_t)fi->fh;

    sqfs_vfs_release(ctx, fh);
    fi->fh = 0;

    return 0;
}

/*
 * readlink - Read symbolic link target
 */
int sqfs_fuse_readlink(const char *path, char *buf, size_t size) {
    sqfs_ctx_t *ctx = (sqfs_ctx_t *)fuse_get_context()->private_data;
    int ret;

    ret = sqfs_vfs_readlink(ctx, path, buf, size);
    if (ret != 0) {
        return ret;
    }

    return 0;
}

/*
 * statfs - Get filesystem statistics
 */
int sqfs_fuse_statfs(const char *path, struct statvfs *stbuf) {
    (void)path;
    sqfs_ctx_t *ctx = (sqfs_ctx_t *)fuse_get_context()->private_data;
    sqfs_vfs_statfs_t stat;
    int ret;

    ret = sqfs_vfs_statfs(ctx, &stat);
    if (ret != 0) {
        return ret;
    }

    memset(stbuf, 0, sizeof(struct statvfs));
    stbuf->f_bsize = (unsigned long)stat.bsize;
    stbuf->f_frsize = (unsigned long)stat.frsize;
    stbuf->f_blocks = stat.blocks;
    stbuf->f_bfree = 0;
    stbuf->f_bavail = 0;
    stbuf->f_files = stat.files;
    stbuf->f_ffree = 0;
    stbuf->f_favail = 0;
    stbuf->f_fsid = (unsigned long)stat.fsid;
    stbuf->f_flag = (unsigned long)stat.flags;
    stbuf->f_namemax = (unsigned long)stat.namemax;

    return 0;
}

/*
 * getxattr - Get extended attribute
 */
int sqfs_fuse_getxattr(const char *path, const char *name,
                       char *value, size_t size) {
    sqfs_ctx_t *ctx = (sqfs_ctx_t *)fuse_get_context()->private_data;
    int ret;

    ret = sqfs_vfs_getxattr(ctx, path, name, value, size);
    if (ret < 0) {
        return ret;
    }

    return ret;
}

/*
 * listxattr - List extended attributes
 */
int sqfs_fuse_listxattr(const char *path, char *list, size_t size) {
    sqfs_ctx_t *ctx = (sqfs_ctx_t *)fuse_get_context()->private_data;
    int ret;

    ret = sqfs_vfs_listxattr(ctx, path, list, size);
    if (ret < 0) {
        return ret;
    }

    return ret;
}

/* ============================================================================
 * FUSE Operations Structure
 * ============================================================================ */

struct fuse_operations sqfs_fuse_operations = {
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