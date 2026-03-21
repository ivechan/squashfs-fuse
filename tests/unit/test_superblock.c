/*
 * Unit tests for Superblock
 *
 * Tests superblock parsing, validation, and helper functions.
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "superblock.h"
#include "utils.h"
#include <string.h>
#include <math.h>

/* Link with -lm for log2 function */

/* ============================================================================
 * Magic Number Tests
 * ============================================================================ */

/* Test: Valid magic number constant */
static void test_sb_magic_value(void **state) {
    (void)state;

    /* SquashFS magic is "hsqs" on disk = 0x73717368 in little-endian */
    assert_int_equal(SQUASHFS_MAGIC, 0x73717368);

    /* Verify the bytes spell "hsqs" */
    uint32_t magic = SQUASHFS_MAGIC;
    uint8_t bytes[4];
    memcpy(bytes, &magic, 4);
    assert_int_equal(bytes[0], 'h');
    assert_int_equal(bytes[1], 's');
    assert_int_equal(bytes[2], 'q');
    assert_int_equal(bytes[3], 's');
}

/* Test: Table valid check */
static void test_sb_table_valid(void **state) {
    (void)state;

    /* Valid table offsets */
    assert_true(sqfs_table_valid(0x1000));
    assert_true(sqfs_table_valid(0x10000));
    assert_true(sqfs_table_valid(0xABCDEF00));

    /* Invalid table offset (0xFFFFFFFFFFFFFFFF) */
    assert_false(sqfs_table_valid(0xFFFFFFFFFFFFFFFFULL));
    assert_false(sqfs_table_valid(SQFS_INVALID_OFFSET));
}

/* ============================================================================
 * Version Tests
 * ============================================================================ */

/* Test: Valid version constants */
static void test_sb_version_values(void **state) {
    (void)state;

    /* SquashFS 4.0 is the required version */
    assert_int_equal(SQUASHFS_VERSION_MAJOR, 4);
    assert_int_equal(SQUASHFS_VERSION_MINOR, 0);
}

/* ============================================================================
 * Compressor ID Tests
 * ============================================================================ */

/* Test: Compressor ID values */
static void test_sb_compressor_ids(void **state) {
    (void)state;

    /* Verify compressor IDs match spec */
    assert_int_equal(SQUASHFS_COMP_GZIP, 1);
    assert_int_equal(SQUASHFS_COMP_LZMA, 2);
    assert_int_equal(SQUASHFS_COMP_LZO,  3);
    assert_int_equal(SQUASHFS_COMP_XZ,   4);
    assert_int_equal(SQUASHFS_COMP_LZ4,  5);
    assert_int_equal(SQUASHFS_COMP_ZSTD, 6);
}

/* ============================================================================
 * Flag Tests
 * ============================================================================ */

/* Test: Superblock flag values */
static void test_sb_flag_values(void **state) {
    (void)state;

    /* Verify flag bit positions match spec */
    assert_int_equal(SQUASHFS_FLAG_UNCOMP_INODES,  0x0001);
    assert_int_equal(SQUASHFS_FLAG_UNCOMP_DATA,    0x0002);
    assert_int_equal(SQUASHFS_FLAG_UNUSED,         0x0004);
    assert_int_equal(SQUASHFS_FLAG_UNCOMP_FRAGS,   0x0008);
    assert_int_equal(SQUASHFS_FLAG_NO_FRAGS,       0x0010);
    assert_int_equal(SQUASHFS_FLAG_ALWAYS_FRAGS,   0x0020);
    assert_int_equal(SQUASHFS_FLAG_DEDUPE,         0x0040);
    assert_int_equal(SQUASHFS_FLAG_EXPORT,         0x0080);
    assert_int_equal(SQUASHFS_FLAG_UNCOMP_XATTRS,  0x0100);
    assert_int_equal(SQUASHFS_FLAG_NO_XATTRS,      0x0200);
    assert_int_equal(SQUASHFS_FLAG_COMP_OPTS,      0x0400);
    assert_int_equal(SQUASHFS_FLAG_UNCOMP_IDS,     0x0800);
}

/* Test: Flag combination */
static void test_sb_flag_combination(void **state) {
    (void)state;

    /* Multiple flags can be combined */
    uint16_t flags = SQUASHFS_FLAG_EXPORT | SQUASHFS_FLAG_NO_FRAGS;
    assert_true((flags & SQUASHFS_FLAG_EXPORT) != 0);
    assert_true((flags & SQUASHFS_FLAG_NO_FRAGS) != 0);
    assert_false((flags & SQUASHFS_FLAG_UNCOMP_INODES) != 0);
}

/* ============================================================================
 * Block Size Tests
 * ============================================================================ */

/* Test: Valid block sizes */
static void test_sb_block_size_valid(void **state) {
    (void)state;

    /* Valid block sizes: 4K to 1M, powers of 2 */
    uint32_t valid_sizes[] = {
        4096,    /* 4 KiB - minimum */
        8192,    /* 8 KiB */
        16384,   /* 16 KiB */
        32768,   /* 32 KiB */
        65536,   /* 64 KiB */
        131072,  /* 128 KiB - default */
        262144,  /* 256 KiB */
        524288,  /* 512 KiB */
        1048576  /* 1 MiB - maximum */
    };

    for (size_t i = 0; i < sizeof(valid_sizes)/sizeof(valid_sizes[0]); i++) {
        uint32_t size = valid_sizes[i];

        /* Must be power of 2 */
        assert_true((size & (size - 1)) == 0);

        /* Must be >= 4K and <= 1M */
        assert_true(size >= 4096);
        assert_true(size <= 1048576);

        /* block_log should match */
        uint16_t expected_log = (uint16_t)(log2(size));
        assert_int_equal((1 << expected_log), size);
    }
}

/* Test: Block log calculation */
static void test_sb_block_log_calculation(void **state) {
    (void)state;

    /* block_log = log2(block_size) */
    struct {
        uint32_t block_size;
        uint16_t block_log;
    } test_cases[] = {
        { 4096,    12 },
        { 8192,    13 },
        { 16384,   14 },
        { 32768,   15 },
        { 65536,   16 },
        { 131072,  17 },
        { 262144,  18 },
        { 524288,  19 },
        { 1048576, 20 },
    };

    for (size_t i = 0; i < sizeof(test_cases)/sizeof(test_cases[0]); i++) {
        uint16_t calculated = (uint16_t)(log2(test_cases[i].block_size));
        assert_int_equal(calculated, test_cases[i].block_log);

        /* Verify: 2^block_log = block_size */
        assert_int_equal((1U << test_cases[i].block_log), test_cases[i].block_size);
    }
}

/* ============================================================================
 * Disk Format Tests
 * ============================================================================ */

/* Test: Superblock size */
static void test_sb_size(void **state) {
    (void)state;

    /* Superblock is exactly 96 bytes on disk */
    assert_int_equal(sizeof(squashfs_superblock_t), 96);
}

/* Test: Superblock field offsets */
static void test_sb_field_offsets(void **state) {
    (void)state;

    /* Verify field offsets match spec */
    assert_int_equal(offsetof(squashfs_superblock_t, magic),        0);
    assert_int_equal(offsetof(squashfs_superblock_t, inode_count),  4);
    assert_int_equal(offsetof(squashfs_superblock_t, mod_time),     8);
    assert_int_equal(offsetof(squashfs_superblock_t, block_size),   12);
    assert_int_equal(offsetof(squashfs_superblock_t, frag_count),   16);
    assert_int_equal(offsetof(squashfs_superblock_t, compressor),   20);
    assert_int_equal(offsetof(squashfs_superblock_t, block_log),    22);
    assert_int_equal(offsetof(squashfs_superblock_t, flags),        24);
    assert_int_equal(offsetof(squashfs_superblock_t, id_count),     26);
    assert_int_equal(offsetof(squashfs_superblock_t, version_major),28);
    assert_int_equal(offsetof(squashfs_superblock_t, version_minor),30);
    assert_int_equal(offsetof(squashfs_superblock_t, root_inode),   32);
    assert_int_equal(offsetof(squashfs_superblock_t, bytes_used),   40);
    assert_int_equal(offsetof(squashfs_superblock_t, id_table),     48);
    assert_int_equal(offsetof(squashfs_superblock_t, xattr_table),  56);
    assert_int_equal(offsetof(squashfs_superblock_t, inode_table),  64);
    assert_int_equal(offsetof(squashfs_superblock_t, dir_table),    72);
    assert_int_equal(offsetof(squashfs_superblock_t, frag_table),   80);
    assert_int_equal(offsetof(squashfs_superblock_t, export_table), 88);
}

/* Test: Root inode reference encoding */
static void test_sb_root_inode_encoding(void **state) {
    (void)state;

    /* Root inode is a 64-bit reference: (block_pos << 16) | offset */
    uint64_t pos = 0x12345;
    uint16_t offset = 0x1234;
    uint64_t ref = sqfs_make_meta_ref(pos, offset);

    assert_int_equal(sqfs_meta_block_pos(ref), pos);
    assert_int_equal(sqfs_meta_block_offset(ref), offset);
}

/* ============================================================================
 * Runtime Superblock Tests
 * ============================================================================ */

/* Test: Runtime superblock size */
static void test_sb_runtime_size(void **state) {
    (void)state;

    /* Runtime superblock includes additional fields */
    assert_true(sizeof(sqfs_superblock_t) > sizeof(squashfs_superblock_t));
}

/* ============================================================================
 * Error Code Tests
 * ============================================================================ */

/* Test: Superblock error codes */
static void test_sb_error_codes(void **state) {
    (void)state;

    /* Verify error codes for superblock validation */
    assert_int_equal(SQFS_ERR_BAD_MAGIC,   -1002);
    assert_int_equal(SQFS_ERR_BAD_VERSION, -1003);
    assert_int_equal(SQFS_ERR_BAD_COMP,    -1004);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        /* Magic tests */
        cmocka_unit_test(test_sb_magic_value),
        cmocka_unit_test(test_sb_table_valid),

        /* Version tests */
        cmocka_unit_test(test_sb_version_values),

        /* Compressor tests */
        cmocka_unit_test(test_sb_compressor_ids),

        /* Flag tests */
        cmocka_unit_test(test_sb_flag_values),
        cmocka_unit_test(test_sb_flag_combination),

        /* Block size tests */
        cmocka_unit_test(test_sb_block_size_valid),
        cmocka_unit_test(test_sb_block_log_calculation),

        /* Disk format tests */
        cmocka_unit_test(test_sb_size),
        cmocka_unit_test(test_sb_field_offsets),
        cmocka_unit_test(test_sb_root_inode_encoding),

        /* Runtime tests */
        cmocka_unit_test(test_sb_runtime_size),

        /* Error code tests */
        cmocka_unit_test(test_sb_error_codes),
    };

    return cmocka_run_group_tests_name("Superblock Tests", tests, NULL, NULL);
}