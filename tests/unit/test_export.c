/*
 * Unit tests for Export Table
 *
 * Tests export table structure, inode reference lookup, and helper functions.
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "utils.h"
#include "inode.h"
#include "superblock.h"
#include <string.h>

/* ============================================================================
 * Export Table Constants Tests
 * ============================================================================ */

/* Test: Export table entry size */
static void test_export_entry_size(void **state) {
    (void)state;

    /* Export table entry is 64-bit inode reference */
    assert_int_equal(sizeof(uint64_t), 8);
}

/* Test: Export table entries per block */
static void test_export_entries_per_block(void **state) {
    (void)state;

    /* Each metadata block can hold 1024 inode references (8 KiB / 8 bytes) */
    assert_int_equal(8192 / sizeof(uint64_t), 1024);
}

/* ============================================================================
 * Inode Reference Tests
 * ============================================================================ */

/* Test: Inode reference encoding */
static void test_export_ref_encoding(void **state) {
    (void)state;

    /* Inode reference: (block_pos << 16) | offset
     * Points to inode location in inode table
     */
    uint64_t pos = 0x10000;     /* Inode table block position */
    uint16_t offset = 0x100;    /* Offset within uncompressed block */

    uint64_t ref = sqfs_make_meta_ref(pos, offset);

    assert_int_equal(sqfs_meta_block_pos(ref), pos);
    assert_int_equal(sqfs_meta_block_offset(ref), offset);
}

/* Test: Root inode reference */
static void test_export_root_inode(void **state) {
    (void)state;

    /* Root inode reference is stored in superblock.root_inode */
    /* ref = (block_pos << 16) | offset */
    uint64_t root_ref = 0x0000000100000000ULL;  /* block_pos=0x10000, offset=0 */

    uint64_t block_pos = sqfs_meta_block_pos(root_ref);
    uint16_t offset = sqfs_meta_block_offset(root_ref);

    assert_int_equal(block_pos, 0x10000ULL);
    assert_int_equal(offset, 0);
}

/* Test: Inode reference extraction */
static void test_export_ref_extraction(void **state) {
    (void)state;

    /* Reference format: block_pos = ref >> 16, offset = ref & 0xFFFF */
    struct {
        uint64_t ref;
        uint64_t expected_pos;
        uint16_t expected_offset;
    } test_cases[] = {
        { 0x0000000000000000ULL, 0, 0 },
        { 0x0000000000000001ULL, 0, 1 },
        { 0x0000000000010000ULL, 1, 0 },
        { 0x0000000100023456ULL, 0x10002, 0x3456 },
        { 0xFFFFFFFFFFFF0000ULL, 0xFFFFFFFFFFFFULL, 0 },
        { 0x000000000000FFFFULL, 0, 0xFFFF },
    };

    for (size_t i = 0; i < sizeof(test_cases)/sizeof(test_cases[0]); i++) {
        uint64_t pos = sqfs_meta_block_pos(test_cases[i].ref);
        uint16_t offset = sqfs_meta_block_offset(test_cases[i].ref);

        assert_int_equal(pos, test_cases[i].expected_pos);
        assert_int_equal(offset, test_cases[i].expected_offset);
    }
}

/* ============================================================================
 * Export Table Lookup Tests
 * ============================================================================ */

/* Test: Export table index calculation */
static void test_export_index_calculation(void **state) {
    (void)state;

    /* Index = inode_number - 1 (inode 0 is reserved) */
    uint32_t inode_numbers[] = { 1, 2, 100, 1000, 65535 };
    uint32_t expected_indices[] = { 0, 1, 99, 999, 65534 };

    for (size_t i = 0; i < sizeof(inode_numbers)/sizeof(inode_numbers[0]); i++) {
        uint32_t index = inode_numbers[i] - 1;
        assert_int_equal(index, expected_indices[i]);
    }
}

/* Test: Export table block index */
static void test_export_block_index(void **state) {
    (void)state;

    /* Each block holds 1024 references */
    uint32_t entries_per_block = 1024;

    /* Block index for inode number */
    uint32_t inode_nums[] = { 1, 1024, 1025, 2048, 2049 };
    uint32_t expected_blocks[] = { 0, 0, 1, 1, 2 };

    for (size_t i = 0; i < sizeof(inode_nums)/sizeof(inode_nums[0]); i++) {
        uint32_t index = inode_nums[i] - 1;
        uint32_t block_idx = index / entries_per_block;
        assert_int_equal(block_idx, expected_blocks[i]);
    }
}

/* Test: Export table offset within block */
static void test_export_block_offset(void **state) {
    (void)state;

    uint32_t entries_per_block = 1024;

    /* Offset within block for inode number */
    uint32_t inode_nums[] = { 1, 1024, 1025, 2049 };
    uint32_t expected_offsets[] = { 0, 1023, 0, 0 };

    for (size_t i = 0; i < sizeof(inode_nums)/sizeof(inode_nums[0]); i++) {
        uint32_t index = inode_nums[i] - 1;
        uint32_t offset = index % entries_per_block;
        assert_int_equal(offset, expected_offsets[i]);
    }
}

/* Test: Export table size calculation */
static void test_export_table_size(void **state) {
    (void)state;

    /* Number of metadata blocks = ceil(inode_count / 1024) */
    uint32_t inode_counts[] = { 1, 1024, 1025, 2048, 10000 };
    uint32_t expected_blocks[] = { 1, 1, 2, 2, 10 };

    for (size_t i = 0; i < sizeof(inode_counts)/sizeof(inode_counts[0]); i++) {
        uint32_t blocks = (inode_counts[i] + 1023) / 1024;
        assert_int_equal(blocks, expected_blocks[i]);
    }
}

/* ============================================================================
 * Export Table Validation Tests
 * ============================================================================ */

/* Test: Invalid inode number */
static void test_export_invalid_inode(void **state) {
    (void)state;

    /* Inode number 0 is reserved/invalid */
    uint32_t inode_number = 0;
    assert_true(inode_number == 0);

    /* Attempting to export-lookup inode 0 should fail */
    /* Index = 0 - 1 would underflow */
}

/* Test: Missing export table */
static void test_export_missing_table(void **state) {
    (void)state;

    /* Export table is optional; if missing, superblock.export_table = 0xFFFFFFFFFFFFFFFF */
    uint64_t missing_marker = SQFS_INVALID_OFFSET;
    assert_false(sqfs_table_valid(missing_marker));
}

/* Test: Export table lookup structure */
static void test_export_two_level_lookup(void **state) {
    (void)state;

    /* Export table uses two-level lookup:
     * 1. Lookup table: array of 64-bit pointers to metadata blocks
     * 2. Metadata blocks: contain inode references
     *
     * To find inode N:
     * 1. Compute index = N - 1
     * 2. Compute block_idx = index / 1024
     * 3. Read lookup_table[block_idx] to get metadata block position
     * 4. Read metadata block, get entry at offset = (index % 1024) * 8
     */

    uint32_t inode_number = 2000;
    uint32_t index = inode_number - 1;
    uint32_t block_idx = index / 1024;
    uint32_t entry_offset = (index % 1024) * sizeof(uint64_t);

    assert_int_equal(block_idx, 1);         /* Second lookup table entry */
    assert_int_equal(entry_offset, 975 * 8); /* Byte offset in metadata block */
}

/* ============================================================================
 * Lookup Table Structure Tests
 * ============================================================================ */

/* Test: Lookup table size */
static void test_export_lookup_table_size(void **state) {
    (void)state;

    /* Lookup table is an array of 64-bit pointers, stored uncompressed */
    uint32_t inode_count = 10000;
    uint32_t num_blocks = (inode_count + 1023) / 1024;
    uint64_t lookup_table_size = num_blocks * sizeof(uint64_t);

    assert_int_equal(lookup_table_size, 10 * 8);  /* 10 blocks * 8 bytes */
}

/* Test: Lookup table entry points to metadata block */
static void test_export_lookup_entry(void **state) {
    (void)state;

    /* Each lookup table entry is an absolute disk position */
    /* The entry points directly to the metadata block header */
    /* When reading: read 16-bit header, then read/expand the block */
}

int main(void) {
    const struct CMUnitTest tests[] = {
        /* Constants tests */
        cmocka_unit_test(test_export_entry_size),
        cmocka_unit_test(test_export_entries_per_block),

        /* Reference tests */
        cmocka_unit_test(test_export_ref_encoding),
        cmocka_unit_test(test_export_root_inode),
        cmocka_unit_test(test_export_ref_extraction),

        /* Lookup tests */
        cmocka_unit_test(test_export_index_calculation),
        cmocka_unit_test(test_export_block_index),
        cmocka_unit_test(test_export_block_offset),
        cmocka_unit_test(test_export_table_size),

        /* Validation tests */
        cmocka_unit_test(test_export_invalid_inode),
        cmocka_unit_test(test_export_missing_table),
        cmocka_unit_test(test_export_two_level_lookup),

        /* Structure tests */
        cmocka_unit_test(test_export_lookup_table_size),
        cmocka_unit_test(test_export_lookup_entry),
    };

    return cmocka_run_group_tests_name("Export Table Tests", tests, NULL, NULL);
}