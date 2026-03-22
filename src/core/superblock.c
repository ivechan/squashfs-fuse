/*
 * SquashFS-FUSE - Superblock Implementation
 *
 * Superblock loading and validation.
 */

#define _POSIX_C_SOURCE 200809L
#include "superblock.h"
#include "utils.h"
#include "log.h"
#include "stats.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>

/* ============================================================================
 * Internal Helpers
 * ============================================================================ */

/* Validate block_size and block_log consistency */
static int validate_block_size(uint32_t block_size, uint16_t block_log) {
    /* Check block_size is a power of 2 */
    if (block_size == 0 || (block_size & (block_size - 1)) != 0) {
        SQFS_LOG_SB_ERROR("block_size %u is not a power of 2", block_size);
        return SQFS_ERR_CORRUPT;
    }

    /* Check block_size is within valid range (4KiB to 1MiB) */
    if (block_size < 4096 || block_size > 1048576) {
        SQFS_LOG_SB_ERROR("block_size %u out of range [4096, 1048576]", block_size);
        return SQFS_ERR_CORRUPT;
    }

    /* Verify block_log matches block_size */
    uint16_t computed_log = 0;
    uint32_t temp = block_size;
    while (temp > 1) {
        temp >>= 1;
        computed_log++;
    }

    if (computed_log != block_log) {
        SQFS_LOG_SB_ERROR("block_log %u doesn't match block_size %u (expected %u)",
                          block_log, block_size, computed_log);
        return SQFS_ERR_CORRUPT;
    }

    return SQFS_OK;
}

/* Check if compressor is supported by this implementation */
static bool is_compressor_supported(uint16_t comp_id) {
    switch (comp_id) {
        case SQUASHFS_COMP_GZIP:
        case SQUASHFS_COMP_ZSTD:
            return true;
        case SQUASHFS_COMP_LZMA:
        case SQUASHFS_COMP_LZO:
        case SQUASHFS_COMP_XZ:
        case SQUASHFS_COMP_LZ4:
            return false;
        default:
            return false;
    }
}

/* Get compressor name string */
static const char *compressor_name(uint16_t comp_id) {
    switch (comp_id) {
        case SQUASHFS_COMP_GZIP: return "gzip";
        case SQUASHFS_COMP_LZMA: return "lzma";
        case SQUASHFS_COMP_LZO:  return "lzo";
        case SQUASHFS_COMP_XZ:   return "xz";
        case SQUASHFS_COMP_LZ4:  return "lz4";
        case SQUASHFS_COMP_ZSTD: return "zstd";
        default: return "unknown";
    }
}

/* ============================================================================
 * Public API
 * ============================================================================ */

int sqfs_superblock_load(int fd, sqfs_superblock_t *sb) {
    int ret;
    struct stat st;
    squashfs_superblock_t disk_sb;

    if (sb == NULL) {
        return SQFS_ERR_CORRUPT;
    }

    memset(sb, 0, sizeof(*sb));

    /* Get file size */
    if (fstat(fd, &st) < 0) {
        SQFS_LOG_SB_ERROR("fstat failed: %s", strerror(errno));
        SQFS_STATS_ERROR_IO();
        return SQFS_ERR_IO;
    }
    sb->file_size = (size_t)st.st_size;
    sb->fd = fd;

    SQFS_LOG_SB_DEBUG("parsing superblock, file_size=%zu", sb->file_size);

    /* Read raw superblock (96 bytes) */
    ret = sqfs_pread_all(fd, &disk_sb, sizeof(disk_sb), 0);
    if (ret != SQFS_OK) {
        SQFS_LOG_SB_ERROR("failed to read superblock");
        SQFS_STATS_ERROR_IO();
        return ret;
    }

    /* Validate magic number */
    if (disk_sb.magic != SQUASHFS_MAGIC) {
        SQFS_LOG_SB_ERROR("invalid magic: 0x%08x (expected 0x%08x)",
                          disk_sb.magic, SQUASHFS_MAGIC);
        SQFS_STATS_ERROR_CORRUPT();
        return SQFS_ERR_BAD_MAGIC;
    }

    /* Validate version */
    if (disk_sb.version_major != SQUASHFS_VERSION_MAJOR ||
        disk_sb.version_minor != SQUASHFS_VERSION_MINOR) {
        SQFS_LOG_SB_ERROR("unsupported version: %u.%u (expected %u.%u)",
                          disk_sb.version_major, disk_sb.version_minor,
                          SQUASHFS_VERSION_MAJOR, SQUASHFS_VERSION_MINOR);
        return SQFS_ERR_BAD_VERSION;
    }

    /* Validate compressor */
    if (!is_compressor_supported(disk_sb.compressor)) {
        SQFS_LOG_SB_ERROR("unsupported compressor: %u (%s)",
                          disk_sb.compressor, compressor_name(disk_sb.compressor));
        return SQFS_ERR_BAD_COMP;
    }

    /* Validate block_size and block_log */
    ret = validate_block_size(disk_sb.block_size, disk_sb.block_log);
    if (ret != SQFS_OK) {
        SQFS_STATS_ERROR_CORRUPT();
        return ret;
    }

    /* Check unused flag */
    if (disk_sb.flags & SQUASHFS_FLAG_UNUSED) {
        SQFS_LOG_SB_WARN("unused flag is set");
    }

    /* Validate bytes_used doesn't exceed file size */
    if (disk_sb.bytes_used > (uint64_t)sb->file_size) {
        SQFS_LOG_SB_ERROR("bytes_used (%lu) exceeds file size (%zu)",
                          (unsigned long)disk_sb.bytes_used, sb->file_size);
        SQFS_STATS_ERROR_CORRUPT();
        return SQFS_ERR_CORRUPT;
    }

    /* Copy disk superblock */
    sb->disk = disk_sb;

    /* Set convenience flags */
    sb->has_fragments = (disk_sb.flags & SQUASHFS_FLAG_NO_FRAGS) == 0 &&
                        sqfs_table_valid(disk_sb.frag_table);
    sb->has_xattrs = (disk_sb.flags & SQUASHFS_FLAG_NO_XATTRS) == 0 &&
                     sqfs_table_valid(disk_sb.xattr_table);
    sb->has_export = sqfs_table_valid(disk_sb.export_table);

    SQFS_LOG_SB_INFO("superblock loaded: %u inodes, block_size=%u, compressor=%s",
                     disk_sb.inode_count, disk_sb.block_size,
                     compressor_name(disk_sb.compressor));

    return SQFS_OK;
}

void sqfs_superblock_destroy(sqfs_superblock_t *sb) {
    if (sb != NULL) {
        /* Nothing dynamically allocated yet */
        memset(sb, 0, sizeof(*sb));
    }
}

void sqfs_superblock_print(const sqfs_superblock_t *sb) {
    if (sb == NULL) {
        printf("Superblock: NULL\n");
        return;
    }

    printf("=== SquashFS Superblock ===\n");
    printf("Magic:           0x%08x\n", sb->disk.magic);
    printf("Version:         %u.%u\n",
           sb->disk.version_major, sb->disk.version_minor);
    printf("Inode Count:     %u\n", sb->disk.inode_count);
    printf("Block Size:      %u (log2=%u)\n",
           sb->disk.block_size, sb->disk.block_log);
    printf("Frag Count:      %u\n", sb->disk.frag_count);
    printf("Compressor:      %u (%s)\n",
           sb->disk.compressor, compressor_name(sb->disk.compressor));
    printf("Flags:           0x%04x\n", sb->disk.flags);
    printf("  - Uncompressed inodes:  %s\n",
           (sb->disk.flags & SQUASHFS_FLAG_UNCOMP_INODES) ? "yes" : "no");
    printf("  - Uncompressed data:    %s\n",
           (sb->disk.flags & SQUASHFS_FLAG_UNCOMP_DATA) ? "yes" : "no");
    printf("  - No fragments:         %s\n",
           (sb->disk.flags & SQUASHFS_FLAG_NO_FRAGS) ? "yes" : "no");
    printf("  - Has xattrs:           %s\n",
           sb->has_xattrs ? "yes" : "no");
    printf("  - Has export table:     %s\n",
           sb->has_export ? "yes" : "no");
    printf("ID Count:        %u\n", sb->disk.id_count);
    printf("Bytes Used:      %lu\n", (unsigned long)sb->disk.bytes_used);
    printf("Root Inode:      0x%016lx\n", (unsigned long)sb->disk.root_inode);
    printf("\nTable Locations:\n");
    printf("  Inode Table:   0x%016lx\n", (unsigned long)sb->disk.inode_table);
    printf("  Dir Table:     0x%016lx\n", (unsigned long)sb->disk.dir_table);
    printf("  Frag Table:    0x%016lx\n", (unsigned long)sb->disk.frag_table);
    printf("  ID Table:      0x%016lx\n", (unsigned long)sb->disk.id_table);
    printf("  Xattr Table:   0x%016lx\n", (unsigned long)sb->disk.xattr_table);
    printf("  Export Table:  0x%016lx\n", (unsigned long)sb->disk.export_table);
}