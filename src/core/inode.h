/*
 * SquashFS-FUSE - Inode Header
 *
 * Inode data structures and interface definitions.
 */

#ifndef SQFS_INODE_H
#define SQFS_INODE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Inode Types
 * ============================================================================ */

typedef enum {
    SQFS_INODE_DIR      = 1,   /* Basic Directory */
    SQFS_INODE_FILE     = 2,   /* Basic File */
    SQFS_INODE_SYMLINK  = 3,   /* Basic Symlink */
    SQFS_INODE_BLKDEV   = 4,   /* Basic Block Device */
    SQFS_INODE_CHRDEV   = 5,   /* Basic Character Device */
    SQFS_INODE_FIFO     = 6,   /* Basic FIFO */
    SQFS_INODE_SOCKET   = 7,   /* Basic Socket */
    SQFS_INODE_LDIR     = 8,   /* Extended Directory */
    SQFS_INODE_LFILE    = 9,   /* Extended File */
    SQFS_INODE_LSYMLINK = 10,  /* Extended Symlink */
    SQFS_INODE_LBLKDEV  = 11,  /* Extended Block Device */
    SQFS_INODE_LCHRDEV  = 12,  /* Extended Character Device */
    SQFS_INODE_LFIFO    = 13,  /* Extended FIFO */
    SQFS_INODE_LSOCKET  = 14,  /* Extended Socket */
} sqfs_inode_type_t;

/* Check if inode type is extended */
static inline bool sqfs_inode_is_extended(sqfs_inode_type_t type) {
    return type >= SQFS_INODE_LDIR;
}

/* Get basic inode type from extended type */
static inline sqfs_inode_type_t sqfs_inode_basic_type(sqfs_inode_type_t type) {
    if (type >= SQFS_INODE_LDIR) {
        return (sqfs_inode_type_t)(type - 7);
    }
    return type;
}

/* Check if inode type is a directory */
static inline bool sqfs_inode_is_dir(sqfs_inode_type_t type) {
    return type == SQFS_INODE_DIR || type == SQFS_INODE_LDIR;
}

/* Check if inode type is a regular file */
static inline bool sqfs_inode_is_file(sqfs_inode_type_t type) {
    return type == SQFS_INODE_FILE || type == SQFS_INODE_LFILE;
}

/* Check if inode type is a symlink */
static inline bool sqfs_inode_is_symlink(sqfs_inode_type_t type) {
    return type == SQFS_INODE_SYMLINK || type == SQFS_INODE_LSYMLINK;
}

/* Check if inode type is a device */
static inline bool sqfs_inode_is_dev(sqfs_inode_type_t type) {
    return type == SQFS_INODE_BLKDEV || type == SQFS_INODE_CHRDEV ||
           type == SQFS_INODE_LBLKDEV || type == SQFS_INODE_LCHRDEV;
}

/* Check if inode type is IPC */
static inline bool sqfs_inode_is_ipc(sqfs_inode_type_t type) {
    return type == SQFS_INODE_FIFO || type == SQFS_INODE_SOCKET ||
           type == SQFS_INODE_LFIFO || type == SQFS_INODE_LSOCKET;
}

/* ============================================================================
 * Disk Format Inode Structures
 * ============================================================================ */

/* Common inode header (all inodes share this) */
typedef struct __attribute__((packed)) {
    uint16_t type;
    uint16_t permissions;
    uint16_t uid_idx;          /* ID Table index */
    uint16_t gid_idx;          /* ID Table index */
    uint32_t mtime;
    uint32_t inode_number;
} sqfs_inode_header_t;

/* Basic Directory Inode */
typedef struct __attribute__((packed)) {
    sqfs_inode_header_t header;
    uint32_t block_idx;        /* Directory table block index */
    uint32_t link_count;
    uint16_t file_size;        /* Directory entry size + 3 */
    uint16_t block_offset;     /* Block offset */
    uint32_t parent_inode;
} sqfs_inode_dir_t;

/* Extended Directory Inode */
typedef struct __attribute__((packed)) {
    sqfs_inode_header_t header;
    uint32_t link_count;
    uint32_t file_size;
    uint32_t block_idx;
    uint32_t parent_inode;
    uint16_t index_count;
    uint16_t block_offset;
    uint32_t xattr_idx;        /* Xattr Table index, 0xFFFFFFFF = none */
} sqfs_inode_ldir_t;

/* Basic File Inode */
typedef struct __attribute__((packed)) {
    sqfs_inode_header_t header;
    uint32_t blocks_start;     /* First data block position */
    uint32_t frag_idx;         /* Fragment index, 0xFFFFFFFF = none */
    uint32_t block_offset;     /* Fragment offset */
    uint32_t file_size;
    /* Followed by block_sizes[] array */
} sqfs_inode_file_t;

/* Extended File Inode */
typedef struct __attribute__((packed)) {
    sqfs_inode_header_t header;
    uint64_t blocks_start;
    uint64_t file_size;
    uint64_t sparse;           /* Sparse file hole size */
    uint32_t link_count;
    uint32_t frag_idx;
    uint32_t block_offset;
    uint32_t xattr_idx;
    /* Followed by block_sizes[] array */
} sqfs_inode_lfile_t;

/* Basic Symlink Inode */
typedef struct __attribute__((packed)) {
    sqfs_inode_header_t header;
    uint32_t link_count;
    uint32_t target_size;
    /* Followed by target_path[] bytes */
} sqfs_inode_symlink_t;

/* Extended Symlink Inode */
typedef struct __attribute__((packed)) {
    sqfs_inode_header_t header;
    uint32_t link_count;
    uint32_t target_size;
    /* Followed by target_path[] bytes */
    /* Followed by xattr_idx (uint32_t) */
} sqfs_inode_lsymlink_t;

/* Device Inode (Block/Char) */
typedef struct __attribute__((packed)) {
    sqfs_inode_header_t header;
    uint32_t link_count;
    uint32_t device_number;    /* major/minor encoded */
} sqfs_inode_dev_t;

/* Extended Device Inode */
typedef struct __attribute__((packed)) {
    sqfs_inode_header_t header;
    uint32_t link_count;
    uint32_t device_number;
    uint32_t xattr_idx;
} sqfs_inode_ldev_t;

/* IPC Inode (FIFO/Socket) */
typedef struct __attribute__((packed)) {
    sqfs_inode_header_t header;
    uint32_t link_count;
} sqfs_inode_ipc_t;

/* Extended IPC Inode */
typedef struct __attribute__((packed)) {
    sqfs_inode_header_t header;
    uint32_t link_count;
    uint32_t xattr_idx;
} sqfs_inode_lipc_t;

/* ============================================================================
 * Runtime Inode Structure
 * ============================================================================ */

struct sqfs_inode {
    uint64_t inode_number;
    sqfs_inode_type_t type;
    uint16_t permissions;
    uint16_t uid;
    uint16_t gid;
    uint32_t mtime;
    uint32_t link_count;

    union {
        /* Directory specific data */
        struct {
            uint64_t block_idx;
            uint32_t size;
            uint16_t block_offset;
            uint64_t parent_inode;
            uint16_t index_count;
        } dir;

        /* File specific data */
        struct {
            uint64_t blocks_start;
            uint64_t file_size;
            uint64_t sparse;
            uint32_t frag_idx;
            uint32_t block_offset;
            uint32_t block_count;
            uint32_t xattr_idx;
        } file;

        /* Symlink specific data */
        struct {
            char *target;
            uint32_t target_size;
        } symlink;

        /* Device specific data */
        struct {
            uint32_t major;
            uint32_t minor;
        } dev;

        /* Xattr index for types that only have this */
        uint32_t xattr_idx;
    };

    uint32_t *block_sizes;     /* Data block size array (dynamically allocated) */
};

typedef struct sqfs_inode sqfs_inode_t;

/* ============================================================================
 * Context Forward Declaration
 * ============================================================================ */

struct sqfs_ctx;
typedef struct sqfs_ctx sqfs_ctx_t;

/* Backward compatibility */
typedef sqfs_ctx_t sqfs_fuse_ctx_t;

/* ============================================================================
 * Inode Operations
 * ============================================================================ */

/*
 * Load an inode by inode number.
 *
 * Parameters:
 *   ctx       - FUSE context with superblock and caches
 *   inode_num - Inode number (1-based)
 *   inode     - Output pointer to allocated inode structure
 *
 * Returns:
 *   SQFS_OK on success
 *   SQFS_ERR_NOT_FOUND if inode number is invalid
 *   SQFS_ERR_BAD_INODE if inode data is corrupted
 *   SQFS_ERR_NO_MEMORY on allocation failure
 *   SQFS_ERR_IO on I/O error
 */
int sqfs_inode_load(sqfs_fuse_ctx_t *ctx, uint64_t inode_num,
                    sqfs_inode_t **inode);

/*
 * Free an inode structure.
 *
 * Parameters:
 *   inode - Inode to free
 */
void sqfs_inode_free(sqfs_inode_t *inode);

/*
 * Get inode from metadata reference.
 *
 * Parameters:
 *   ctx  - FUSE context
 *   ref  - 64-bit metadata reference (position << 16 | offset)
 *   inode - Output pointer to allocated inode structure
 *
 * Returns:
 *   SQFS_OK on success
 *   SQFS_ERR_BAD_INODE if inode data is corrupted
 *   SQFS_ERR_NO_MEMORY on allocation failure
 *   SQFS_ERR_IO on I/O error
 */
int sqfs_inode_get_by_ref(sqfs_fuse_ctx_t *ctx, uint64_t ref,
                          sqfs_inode_t **inode);

/*
 * Calculate the number of data blocks for a file.
 *
 * Parameters:
 *   file_size   - File size in bytes
 *   block_size  - Block size from superblock
 *   has_fragment - Whether file has a fragment (tail end)
 *
 * Returns:
 *   Number of full data blocks
 */
uint32_t sqfs_calc_block_count(uint64_t file_size, uint32_t block_size,
                               bool has_fragment);

/*
 * Print inode information (for debugging).
 */
void sqfs_inode_print(const sqfs_inode_t *inode);

/*
 * Get human-readable name for inode type.
 *
 * Parameters:
 *   type - Inode type enum value
 *
 * Returns:
 *   Static string describing the inode type
 */
const char *sqfs_inode_type_name(sqfs_inode_type_t type);

#ifdef __cplusplus
}
#endif

#endif /* SQFS_INODE_H */