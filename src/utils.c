/*
 * SquashFS-FUSE - Utilities Implementation
 *
 * Common utility functions implementation.
 */

#define _POSIX_C_SOURCE 200809L
#include "utils.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

/* ============================================================================
 * Memory Utilities
 * ============================================================================ */

void *sqfs_malloc(size_t size) {
    void *ptr = malloc(size);
    if (ptr == NULL && size > 0) {
        SQFS_LOG("malloc failed for %zu bytes", size);
    }
    return ptr;
}

void *sqfs_calloc(size_t nmemb, size_t size) {
    void *ptr = calloc(nmemb, size);
    if (ptr == NULL && nmemb > 0 && size > 0) {
        SQFS_LOG("calloc failed for %zu x %zu bytes", nmemb, size);
    }
    return ptr;
}

void *sqfs_realloc(void *ptr, size_t size) {
    void *new_ptr = realloc(ptr, size);
    if (new_ptr == NULL && size > 0) {
        SQFS_LOG("realloc failed for %zu bytes", size);
    }
    return new_ptr;
}

void sqfs_free(void *ptr) {
    free(ptr);
}

/* ============================================================================
 * I/O Utilities
 * ============================================================================ */

int sqfs_read_all(int fd, void *buf, size_t count) {
    size_t total = 0;
    ssize_t n;

    while (total < count) {
        n = read(fd, (char *)buf + total, count - total);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            SQFS_LOG("read error: %s", strerror(errno));
            return SQFS_ERR_IO;
        }
        if (n == 0) {
            SQFS_LOG("unexpected end of file after %zu of %zu bytes",
                     total, count);
            return SQFS_ERR_CORRUPT;
        }
        total += (size_t)n;
    }

    return SQFS_OK;
}

int sqfs_pread_all(int fd, void *buf, size_t count, uint64_t offset) {
    size_t total = 0;
    ssize_t n;

    while (total < count) {
        n = pread(fd, (char *)buf + total, count - total,
                  (off_t)(offset + total));
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            SQFS_LOG("pread error at offset %lu: %s",
                     (unsigned long)offset, strerror(errno));
            return SQFS_ERR_IO;
        }
        if (n == 0) {
            SQFS_LOG("unexpected end of file at offset %lu after %zu of %zu bytes",
                     (unsigned long)(offset + total), total, count);
            return SQFS_ERR_CORRUPT;
        }
        total += (size_t)n;
    }

    return SQFS_OK;
}