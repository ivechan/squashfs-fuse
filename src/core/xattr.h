/*
 * SquashFS-FUSE - Extended Attributes (Xattr) Header
 *
 * Xattr table data structures and interface definitions.
 * Based on SquashFS format specification section 3.5.
 */

#ifndef SQFS_XATTR_H
#define SQFS_XATTR_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Xattr Prefix Types
 * ============================================================================ */

typedef enum {
    SQFS_XATTR_PREFIX_USER     = 0,  /* "user." prefix */
    SQFS_XATTR_PREFIX_TRUSTED  = 1,  /* "trusted." prefix */
    SQFS_XATTR_PREFIX_SECURITY = 2,  /* "security." prefix */
} sqfs_xattr_prefix_t;

/* Out-of-line flag for xattr type */
#define SQFS_XATTR_TYPE_OOL     0x0100

/* ============================================================================
 * Xattr Prefix Strings
 * ============================================================================ */

/* Get prefix string for xattr type */
static inline const char *sqfs_xattr_prefix_str(sqfs_xattr_prefix_t type) {
    switch (type) {
        case SQFS_XATTR_PREFIX_USER:     return "user.";
        case SQFS_XATTR_PREFIX_TRUSTED:  return "trusted.";
        case SQFS_XATTR_PREFIX_SECURITY: return "security.";
        default:                         return NULL;
    }
}

/* Get prefix length for xattr type */
static inline size_t sqfs_xattr_prefix_len(sqfs_xattr_prefix_t type) {
    switch (type) {
        case SQFS_XATTR_PREFIX_USER:     return 5;  /* "user." */
        case SQFS_XATTR_PREFIX_TRUSTED:  return 8;  /* "trusted." */
        case SQFS_XATTR_PREFIX_SECURITY: return 9;  /* "security." */
        default:                         return 0;
    }
}

/* ============================================================================
 * Disk Format Xattr Structures
 * ============================================================================ */

/* Xattr key structure (followed by name[] bytes) */
typedef struct __attribute__((packed)) {
    uint16_t type;             /* Prefix type | 0x0100 for out-of-line */
    uint16_t name_size;        /* Name length (without prefix) */
} sqfs_xattr_key_t;

/* Xattr value structure (followed by value[] bytes) */
typedef struct __attribute__((packed)) {
    uint32_t value_size;       /* Value length */
} sqfs_xattr_value_t;

/* Xattr ID table entry (16 bytes) */
typedef struct __attribute__((packed)) {
    uint64_t xattr_ref;        /* Metadata reference (position << 16 | offset) */
    uint32_t count;            /* Number of key-value pairs */
    uint32_t size;             /* Total size of all keys and values */
} sqfs_xattr_id_entry_t;

/* Invalid xattr index (indicates no xattrs) */
#define SQFS_XATTR_INVALID_IDX  0xFFFFFFFF

/* ============================================================================
 * Xattr Table Header (on disk)
 * ============================================================================ */

typedef struct __attribute__((packed)) {
    uint64_t xattr_table_start;  /* Xattr metadata table start */
    uint64_t xattr_ids;          /* Number of xattr ID entries */
    uint16_t unused;
} sqfs_xattr_table_header_t;

/* ============================================================================
 * Runtime Xattr Structure
 * ============================================================================ */

typedef struct {
    char *key;                 /* Complete key name (including prefix) */
    void *value;               /* Value data */
    size_t value_size;         /* Value size in bytes */
} sqfs_xattr_t;

/* ============================================================================
 * Runtime Xattr Table Structure
 * ============================================================================ */

struct sqfs_xattr_table {
    sqfs_xattr_id_entry_t *entries;  /* Array of xattr ID entries */
    size_t count;                    /* Number of entries */
    uint64_t table_start;            /* Xattr metadata start position */
    bool loaded;                     /* Whether table has been loaded */
};

typedef struct sqfs_xattr_table sqfs_xattr_table_t;

/* ============================================================================
 * Context Forward Declaration
 * ============================================================================ */

struct sqfs_ctx;
typedef struct sqfs_ctx sqfs_ctx_t;

/* Backward compatibility */
typedef sqfs_ctx_t sqfs_fuse_ctx_t;

/* ============================================================================
 * Xattr Operations
 * ============================================================================ */

/*
 * Load xattr table from SquashFS image.
 * Must be called before any other xattr operations.
 *
 * Parameters:
 *   ctx - FUSE context with superblock and file descriptor
 *
 * Returns:
 *   SQFS_OK on success
 *   SQFS_ERR_NOT_FOUND if xattr table doesn't exist
 *   SQFS_ERR_CORRUPT if table data is corrupted
 *   SQFS_ERR_NO_MEMORY on allocation failure
 *   SQFS_ERR_IO on I/O error
 */
int sqfs_xattr_table_load(sqfs_fuse_ctx_t *ctx);

/*
 * Free xattr table resources.
 *
 * Parameters:
 *   ctx - FUSE context
 */
void sqfs_xattr_table_destroy(sqfs_fuse_ctx_t *ctx);

/*
 * Get an extended attribute value by name.
 *
 * Parameters:
 *   ctx       - FUSE context with loaded xattr table
 *   xattr_idx - Xattr index from inode (0xFFFFFFFF = none)
 *   name      - Full attribute name (e.g., "user.comment")
 *   value     - Buffer to store value (can be NULL to get size)
 *   size      - Size of value buffer
 *   out_size  - Output: actual value size (or required size)
 *
 * Returns:
 *   SQFS_OK on success
 *   SQFS_ERR_NOT_FOUND if xattr doesn't exist or xattr_idx is invalid
 *   SQFS_ERR_OVERFLOW if buffer too small (out_size set to required size)
 *   SQFS_ERR_BAD_XATTR if xattr data is corrupted
 *   SQFS_ERR_NO_MEMORY on allocation failure
 *   SQFS_ERR_IO on I/O error
 */
int sqfs_xattr_get(sqfs_fuse_ctx_t *ctx, uint32_t xattr_idx,
                   const char *name, void *value, size_t size,
                   size_t *out_size);

/*
 * List all extended attribute names for an inode.
 *
 * Parameters:
 *   ctx       - FUSE context with loaded xattr table
 *   xattr_idx - Xattr index from inode (0xFFFFFFFF = none)
 *   list      - Buffer to store names (can be NULL to get size)
 *               Names are stored as null-terminated strings, one after another
 *   size      - Size of list buffer
 *   out_size  - Output: total size needed for list (or required size)
 *
 * Returns:
 *   SQFS_OK on success
 *   SQFS_ERR_NOT_FOUND if xattr_idx is invalid
 *   SQFS_ERR_OVERFLOW if buffer too small (out_size set to required size)
 *   SQFS_ERR_BAD_XATTR if xattr data is corrupted
 *   SQFS_ERR_NO_MEMORY on allocation failure
 *   SQFS_ERR_IO on I/O error
 */
int sqfs_xattr_list(sqfs_fuse_ctx_t *ctx, uint32_t xattr_idx,
                    char *list, size_t size, size_t *out_size);

/*
 * Parse xattr type field to extract prefix type and out-of-line flag.
 *
 * Parameters:
 *   type   - Raw type field from sqfs_xattr_key_t
 *   prefix - Output: prefix type
 *   is_ool - Output: true if value is stored out-of-line
 */
static inline void sqfs_xattr_parse_type(uint16_t type,
                                         sqfs_xattr_prefix_t *prefix,
                                         bool *is_ool) {
    *prefix = (sqfs_xattr_prefix_t)(type & 0x00FF);
    *is_ool = (type & SQFS_XATTR_TYPE_OOL) != 0;
}

#ifdef __cplusplus
}
#endif

#endif /* SQFS_XATTR_H */