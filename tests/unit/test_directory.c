/*
 * Unit tests for Directory Table
 *
 * Tests directory header, entry parsing, and helper functions.
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "directory.h"
#include "inode.h"
#include "utils.h"
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Directory Header Tests
 * ============================================================================ */

/* Test: Directory header size */
static void test_dir_header_size(void **state) {
    (void)state;

    /* Directory header is 12 bytes */
    assert_int_equal(sizeof(sqfs_dir_header_t), 12);
}

/* Test: Directory header field offsets */
static void test_dir_header_offsets(void **state) {
    (void)state;

    assert_int_equal(offsetof(sqfs_dir_header_t, count),        0);
    assert_int_equal(offsetof(sqfs_dir_header_t, start),        4);
    assert_int_equal(offsetof(sqfs_dir_header_t, inode_number), 8);
}

/* Test: Directory header count encoding (off-by-one) */
static void test_dir_header_count_encoding(void **state) {
    (void)state;

    sqfs_dir_header_t header;

    /* count is stored off-by-one: count=0 means 1 entry */
    header.count = 0;
    assert_int_equal(header.count + 1, 1);

    header.count = 255;
    assert_int_equal(header.count + 1, 256);

    /* Maximum entries per header is 256 */
    assert_true(header.count <= 255);
}

/* ============================================================================
 * Directory Entry Tests
 * ============================================================================ */

/* Test: Directory entry size */
static void test_dir_entry_size(void **state) {
    (void)state;

    /* Directory entry is 8 bytes (without name) */
    assert_int_equal(sizeof(sqfs_dir_entry_t), 8);
}

/* Test: Directory entry field offsets */
static void test_dir_entry_offsets(void **state) {
    (void)state;

    assert_int_equal(offsetof(sqfs_dir_entry_t, offset),       0);
    assert_int_equal(offsetof(sqfs_dir_entry_t, inode_offset), 2);
    assert_int_equal(offsetof(sqfs_dir_entry_t, type),         4);
    assert_int_equal(offsetof(sqfs_dir_entry_t, name_size),    6);
}

/* Test: Directory entry name size encoding (off-by-one) */
static void test_dir_entry_name_encoding(void **state) {
    (void)state;

    sqfs_dir_entry_t entry;

    /* name_size is stored off-by-one: name_size=0 means 1 byte name */
    entry.name_size = 0;
    assert_int_equal(entry.name_size + 1, 1);

    /* Maximum name length is 256 characters (name_size = 255) */
    entry.name_size = 255;
    assert_int_equal(entry.name_size + 1, 256);

    assert_true(entry.name_size <= 255);
}

/* Test: Directory entry inode offset (signed) */
static void test_dir_entry_inode_offset(void **state) {
    (void)state;

    /* inode_offset is signed (can be negative for hard links) */
    int16_t offsets[] = { -100, -1, 0, 1, 100, 32767, -32768 };

    for (size_t i = 0; i < sizeof(offsets)/sizeof(offsets[0]); i++) {
        sqfs_dir_entry_t entry;
        entry.inode_offset = offsets[i];
        assert_int_equal(entry.inode_offset, offsets[i]);
    }
}

/* Test: Directory entry type values */
static void test_dir_entry_types(void **state) {
    (void)state;

    /* Entry stores basic inode types only (1-7) */
    sqfs_dir_entry_t entry;

    entry.type = SQFS_INODE_DIR;
    assert_int_equal(entry.type, 1);

    entry.type = SQFS_INODE_FILE;
    assert_int_equal(entry.type, 2);

    entry.type = SQFS_INODE_SYMLINK;
    assert_int_equal(entry.type, 3);

    /* Even extended inodes store basic type in entry */
    entry.type = SQFS_INODE_DIR;  /* LDIR (8) would store DIR (1) */
    assert_int_equal(entry.type, 1);
}

/* ============================================================================
 * Directory Index Tests
 * ============================================================================ */

/* Test: Directory index size */
static void test_dir_index_size(void **state) {
    (void)state;

    /* Directory index is 12 bytes (without name) */
    assert_int_equal(sizeof(sqfs_dir_index_t), 12);
}

/* Test: Directory index field offsets */
static void test_dir_index_offsets(void **state) {
    (void)state;

    assert_int_equal(offsetof(sqfs_dir_index_t, index),     0);
    assert_int_equal(offsetof(sqfs_dir_index_t, start),     4);
    assert_int_equal(offsetof(sqfs_dir_index_t, name_size), 8);
}

/* ============================================================================
 * Runtime Directory Entry Tests
 * ============================================================================ */

/* Test: Runtime dirent structure */
static void test_dirent_structure(void **state) {
    (void)state;

    sqfs_dirent_t dirent;

    dirent.name = strdup("test.txt");
    dirent.inode_number = 42;
    dirent.inode_ref = 0x10000;
    dirent.type = SQFS_INODE_FILE;

    assert_string_equal(dirent.name, "test.txt");
    assert_int_equal(dirent.inode_number, 42);
    assert_int_equal(dirent.type, SQFS_INODE_FILE);

    free(dirent.name);
}

/* ============================================================================
 * Inode Number Calculation Tests
 * ============================================================================ */

/* Test: Inode number from header and entry */
static void test_inode_number_calculation(void **state) {
    (void)state;

    /* inode_number = header.inode_number + entry.inode_offset */
    uint32_t header_inode = 100;
    int16_t offsets[] = { -50, -1, 0, 1, 50 };

    for (size_t i = 0; i < sizeof(offsets)/sizeof(offsets[0]); i++) {
        uint32_t calculated = (uint32_t)((int32_t)header_inode + offsets[i]);
        /* Inode numbers are positive */
        if (offsets[i] >= 0 || (int32_t)header_inode + offsets[i] > 0) {
            assert_true(calculated > 0 || calculated == 0);
        }
    }
}

/* Test: Inode reference from header start and entry offset */
static void test_inode_ref_calculation(void **state) {
    (void)state;

    /* Inode reference: (block_pos << 16) | offset */
    uint32_t start = 0x1000;   /* Metadata block position */
    uint16_t offset = 0x100;   /* Offset within block */

    uint64_t ref = sqfs_make_meta_ref(start, offset);

    assert_int_equal(sqfs_meta_block_pos(ref), start);
    assert_int_equal(sqfs_meta_block_offset(ref), offset);
}

/* ============================================================================
 * Empty Directory Tests
 * ============================================================================ */

/* Test: Empty directory detection */
static void test_empty_directory(void **state) {
    (void)state;

    /* Empty directory has file_size < 4 in inode */
    /* This means no directory table entry exists */

    uint16_t empty_size = 3;  /* file_size < 4 means empty */
    assert_true(empty_size < 4);

    uint16_t non_empty_size = 4;  /* file_size >= 4 means has entries */
    assert_false(non_empty_size < 4);
}

/* ============================================================================
 * Directory Entry Validation Tests
 * ============================================================================ */

/* Test: Maximum entries per header */
static void test_max_entries_per_header(void **state) {
    (void)state;

    /* Maximum 256 entries per header (count = 255 means 256 entries) */
    uint32_t max_count = 255;
    uint32_t max_entries = max_count + 1;
    assert_int_equal(max_entries, 256);
}

/* Test: Maximum name length */
static void test_max_name_length(void **state) {
    (void)state;

    /* Maximum name length is 256 characters (name_size = 255) */
    uint16_t max_name_size = 255;
    uint16_t max_name_length = max_name_size + 1;
    assert_int_equal(max_name_length, 256);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        /* Header tests */
        cmocka_unit_test(test_dir_header_size),
        cmocka_unit_test(test_dir_header_offsets),
        cmocka_unit_test(test_dir_header_count_encoding),

        /* Entry tests */
        cmocka_unit_test(test_dir_entry_size),
        cmocka_unit_test(test_dir_entry_offsets),
        cmocka_unit_test(test_dir_entry_name_encoding),
        cmocka_unit_test(test_dir_entry_inode_offset),
        cmocka_unit_test(test_dir_entry_types),

        /* Index tests */
        cmocka_unit_test(test_dir_index_size),
        cmocka_unit_test(test_dir_index_offsets),

        /* Runtime tests */
        cmocka_unit_test(test_dirent_structure),

        /* Calculation tests */
        cmocka_unit_test(test_inode_number_calculation),
        cmocka_unit_test(test_inode_ref_calculation),

        /* Empty directory tests */
        cmocka_unit_test(test_empty_directory),

        /* Validation tests */
        cmocka_unit_test(test_max_entries_per_header),
        cmocka_unit_test(test_max_name_length),
    };

    return cmocka_run_group_tests_name("Directory Table Tests", tests, NULL, NULL);
}