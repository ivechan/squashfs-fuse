/*
 * SquashFS-FUSE - Superblock Header
 *
 * Superblock data structure definitions.
 */

#ifndef SQFS_SUPERBLOCK_H
#define SQFS_SUPERBLOCK_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Magic and Version Constants
 * ============================================================================ */

#define SQUASHFS_MAGIC        0x73717368  /* "hsqs" on disk */
#define SQUASHFS_VERSION_MAJOR 4
#define SQUASHFS_VERSION_MINOR 0

/* ============================================================================
 * Compressor IDs
 * ============================================================================ */

typedef enum {
    SQUASHFS_COMP_GZIP = 1,
    SQUASHFS_COMP_LZMA = 2,
    SQUASHFS_COMP_LZO  = 3,
    SQUASHFS_COMP_XZ   = 4,
    SQUASHFS_COMP_LZ4  = 5,
    SQUASHFS_COMP_ZSTD = 6,
} squashfs_compressor_t;

/* ============================================================================
 * Superblock Flags
 * ============================================================================ */

typedef enum {
    SQUASHFS_FLAG_UNCOMP_INODES   = 0x0001,
    SQUASHFS_FLAG_UNCOMP_DATA     = 0x0002,
    SQUASHFS_FLAG_UNUSED          = 0x0004,
    SQUASHFS_FLAG_UNCOMP_FRAGS    = 0x0008,
    SQUASHFS_FLAG_NO_FRAGS        = 0x0010,
    SQUASHFS_FLAG_ALWAYS_FRAGS    = 0x0020,
    SQUASHFS_FLAG_DEDUPE          = 0x0040,
    SQUASHFS_FLAG_EXPORT          = 0x0080,
    SQUASHFS_FLAG_UNCOMP_XATTRS   = 0x0100,
    SQUASHFS_FLAG_NO_XATTRS       = 0x0200,
    SQUASHFS_FLAG_COMP_OPTS       = 0x0400,
    SQUASHFS_FLAG_UNCOMP_IDS      = 0x0800,
} squashfs_flag_t;

/* ============================================================================
 * Disk Format Superblock (96 bytes, little-endian)
 * ============================================================================ */

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t inode_count;
    uint32_t mod_time;
    uint32_t block_size;
    uint32_t frag_count;
    uint16_t compressor;
    uint16_t block_log;
    uint16_t flags;
    uint16_t id_count;
    uint16_t version_major;
    uint16_t version_minor;
    uint64_t root_inode;
    uint64_t bytes_used;
    uint64_t id_table;
    uint64_t xattr_table;
    uint64_t inode_table;
    uint64_t dir_table;
    uint64_t frag_table;
    uint64_t export_table;
} squashfs_superblock_t;

/* ============================================================================
 * Runtime Superblock
 * ============================================================================ */

typedef struct {
    squashfs_superblock_t disk;
    int fd;                    /* File descriptor */
    size_t file_size;          /* Image file size */
    bool has_xattrs;
    bool has_fragments;
    bool has_export;
} sqfs_superblock_t;

/* ============================================================================
 * Helper Macros
 * ============================================================================ */

#define SQFS_INVALID_OFFSET ((uint64_t)-1)

/* Check if a table offset is valid (not 0xFFFFFFFFFFFFFFFF) */
static inline bool sqfs_table_valid(uint64_t offset) {
    return offset != SQFS_INVALID_OFFSET;
}

/* ============================================================================
 * Superblock Operations
 * ============================================================================ */

/*
 * Load and validate superblock from file descriptor.
 *
 * Parameters:
 *   fd  - Open file descriptor for the SquashFS image
 *   sb  - Pointer to superblock structure to fill
 *
 * Returns:
 *   SQFS_OK on success
 *   SQFS_ERR_BAD_MAGIC if magic number doesn't match
 *   SQFS_ERR_BAD_VERSION if version is not 4.0
 *   SQFS_ERR_BAD_COMP if compressor is not supported
 *   SQFS_ERR_CORRUPT if block_size and block_log don't match
 *   SQFS_ERR_IO on I/O error
 */
int sqfs_superblock_load(int fd, sqfs_superblock_t *sb);

/*
 * Free any resources associated with superblock.
 */
void sqfs_superblock_destroy(sqfs_superblock_t *sb);

/*
 * Print superblock information (for debugging).
 */
void sqfs_superblock_print(const sqfs_superblock_t *sb);

#ifdef __cplusplus
}
#endif

#endif /* SQFS_SUPERBLOCK_H */