/*
 * SquashFS-FUSE - Utilities Header
 *
 * Common macros, error codes, and utility functions.
 */

#ifndef SQFS_UTILS_H
#define SQFS_UTILS_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <errno.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Error Codes
 * ============================================================================ */

typedef enum {
    SQFS_OK = 0,
    SQFS_ERR_CORRUPT      = -1001,  /* Archive corruption */
    SQFS_ERR_BAD_MAGIC    = -1002,  /* Invalid magic number */
    SQFS_ERR_BAD_VERSION  = -1003,  /* Unsupported version */
    SQFS_ERR_BAD_COMP     = -1004,  /* Unsupported compressor */
    SQFS_ERR_BAD_INODE    = -1005,  /* Invalid inode */
    SQFS_ERR_BAD_DIR      = -1006,  /* Invalid directory entry */
    SQFS_ERR_BAD_XATTR    = -1007,  /* Invalid extended attribute */
    SQFS_ERR_BAD_BLOCK    = -1008,  /* Invalid data block */
    SQFS_ERR_NOT_FOUND    = -1009,  /* Not found */
    SQFS_ERR_NO_MEMORY    = -1010,  /* Out of memory */
    SQFS_ERR_IO           = -1011,  /* I/O error */
    SQFS_ERR_OVERFLOW     = -1012,  /* Buffer overflow */
} sqfs_error_t;

/* Convert sqfs_error_t to errno */
static inline int sqfs_errno(sqfs_error_t err) {
    switch (err) {
        case SQFS_OK:              return 0;
        case SQFS_ERR_CORRUPT:
        case SQFS_ERR_BAD_MAGIC:
        case SQFS_ERR_BAD_VERSION:
        case SQFS_ERR_BAD_INODE:
        case SQFS_ERR_BAD_DIR:
        case SQFS_ERR_BAD_XATTR:
        case SQFS_ERR_BAD_BLOCK:   return EIO;
        case SQFS_ERR_NOT_FOUND:   return ENOENT;
        case SQFS_ERR_NO_MEMORY:   return ENOMEM;
        case SQFS_ERR_IO:          return EIO;
        case SQFS_ERR_OVERFLOW:    return EOVERFLOW;
        default:                   return EIO;
    }
}

/* ============================================================================
 * Error Checking Macros
 * ============================================================================ */

#define SQFS_CHECK(cond, err) do { \
    if (!(cond)) return (err); \
} while(0)

#define SQFS_CHECK_GOTO(cond, err, label) do { \
    if (!(cond)) { ret = (err); goto label; } \
} while(0)

/* ============================================================================
 * Debug Logging
 * ============================================================================ */

#ifdef SQFS_DEBUG
#define SQFS_LOG(fmt, ...) fprintf(stderr, "[sqfs] " fmt "\n", ##__VA_ARGS__)
#else
#define SQFS_LOG(fmt, ...) do {} while(0)
#endif

/* ============================================================================
 * Metadata Block Constants
 * ============================================================================ */

#define SQFS_META_UNCOMPRESSED_FLAG 0x8000
#define SQFS_META_MAX_SIZE          8192

/* ============================================================================
 * Metadata Reference Utilities
 * ============================================================================ */

/*
 * SquashFS uses 64-bit metadata references:
 * - High 48 bits: metadata block position (relative to table start)
 * - Low 16 bits: offset within uncompressed block
 */

static inline uint64_t sqfs_meta_block_pos(uint64_t ref) {
    return ref >> 16;
}

static inline uint16_t sqfs_meta_block_offset(uint64_t ref) {
    return (uint16_t)(ref & 0xFFFF);
}

static inline uint64_t sqfs_make_meta_ref(uint64_t pos, uint16_t offset) {
    return (pos << 16) | offset;
}

/* ============================================================================
 * Block Size Utilities
 * ============================================================================ */

/* Check if block is stored uncompressed (bit 24 set) */
static inline bool sqfs_block_is_uncompressed(uint32_t size) {
    return (size & (1U << 24)) != 0;
}

/* Get actual block size (lower 24 bits) */
static inline uint32_t sqfs_block_size(uint32_t size) {
    return size & 0x00FFFFFF;
}

/* ============================================================================
 * Device Number Utilities
 * ============================================================================ */

/* Extract major device number */
static inline uint32_t sqfs_dev_major(uint32_t dev) {
    return (dev & 0xFFF00) >> 8;
}

/* Extract minor device number */
static inline uint32_t sqfs_dev_minor(uint32_t dev) {
    return (dev & 0x000FF) | ((dev >> 12) & 0xFFF00);
}

/* ============================================================================
 * Memory Utilities
 * ============================================================================ */

/* Safe memory allocation with error checking */
void *sqfs_malloc(size_t size);
void *sqfs_calloc(size_t nmemb, size_t size);
void *sqfs_realloc(void *ptr, size_t size);
void sqfs_free(void *ptr);

/* ============================================================================
 * I/O Utilities
 * ============================================================================ */

/* Read exactly 'count' bytes from file descriptor */
int sqfs_read_all(int fd, void *buf, size_t count);

/* Read at specific offset */
int sqfs_pread_all(int fd, void *buf, size_t count, uint64_t offset);

/* ============================================================================
 * Little-Endian Reading Utilities
 * ============================================================================ */

static inline uint16_t sqfs_le16_to_cpu(const void *ptr) {
    const uint8_t *p = (const uint8_t *)ptr;
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t sqfs_le32_to_cpu(const void *ptr) {
    const uint8_t *p = (const uint8_t *)ptr;
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static inline uint64_t sqfs_le64_to_cpu(const void *ptr) {
    const uint8_t *p = (const uint8_t *)ptr;
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) |
           ((uint64_t)p[2] << 16) | ((uint64_t)p[3] << 24) |
           ((uint64_t)p[4] << 32) | ((uint64_t)p[5] << 40) |
           ((uint64_t)p[6] << 48) | ((uint64_t)p[7] << 56);
}

/* ============================================================================
 * Compression Constants
 * ============================================================================ */

/* Compressor return values */
#define SQFS_COMP_OK           0
#define SQFS_COMP_ERROR       -1
#define SQFS_COMP_OVERFLOW    -2
#define SQFS_COMP_CORRUPT     -3
#define SQFS_COMP_UNSUPPORTED -4

#ifdef __cplusplus
}
#endif

#endif /* SQFS_UTILS_H */