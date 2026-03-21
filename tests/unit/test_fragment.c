/*
 * Unit tests for Fragment Table
 *
 * Tests fragment entry parsing, table structure, and helper functions.
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "fragment.h"
#include "utils.h"
#include <string.h>

/* ============================================================================
 * Fragment Entry Tests
 * ============================================================================ */

/* Test: Fragment entry size */
static void test_frag_entry_size(void **state) {
    (void)state;

    /* Fragment entry is 16 bytes on disk */
    assert_int_equal(sizeof(sqfs_frag_entry_t), 16);
}

/* Test: Fragment entry field offsets */
static void test_frag_entry_offsets(void **state) {
    (void)state;

    assert_int_equal(offsetof(sqfs_frag_entry_t, start),  0);
    assert_int_equal(offsetof(sqfs_frag_entry_t, size),   8);
    assert_int_equal(offsetof(sqfs_frag_entry_t, unused), 12);
}

/* Test: Fragment entry start field */
static void test_frag_entry_start(void **state) {
    (void)state;

    sqfs_frag_entry_t entry;
    memset(&entry, 0, sizeof(entry));

    /* Start is 64-bit on-disk position */
    entry.start = 0x123456789ABCDEF0ULL;
    assert_int_equal(entry.start, 0x123456789ABCDEF0ULL);
}

/* Test: Fragment entry size field */
static void test_frag_entry_size_field(void **state) {
    (void)state;

    sqfs_frag_entry_t entry;
    memset(&entry, 0, sizeof(entry));

    /* Size is 32-bit with bit 24 = uncompressed flag */
    entry.size = 0x01000100;  /* Uncompressed, size = 256 */
    assert_true(sqfs_fragment_is_uncompressed(entry.size));
    assert_int_equal(sqfs_fragment_size(entry.size), 256);

    entry.size = 0x00000100;  /* Compressed, size = 256 */
    assert_false(sqfs_fragment_is_uncompressed(entry.size));
    assert_int_equal(sqfs_fragment_size(entry.size), 256);
}

/* ============================================================================
 * Fragment Size Tests
 * ============================================================================ */

/* Test: Fragment uncompressed flag */
static void test_frag_uncompressed_flag(void **state) {
    (void)state;

    /* Bit 24 indicates uncompressed */
    assert_true(sqfs_fragment_is_uncompressed(0x01000000));
    assert_true(sqfs_fragment_is_uncompressed(0x01FFFFFF));
    assert_false(sqfs_fragment_is_uncompressed(0x00FFFFFF));
    assert_false(sqfs_fragment_is_uncompressed(0x00000100));
}

/* Test: Fragment size extraction */
static void test_frag_size_extraction(void **state) {
    (void)state;

    /* Lower 24 bits are the actual size */
    assert_int_equal(sqfs_fragment_size(0x00000000), 0);
    assert_int_equal(sqfs_fragment_size(0x00000100), 256);
    assert_int_equal(sqfs_fragment_size(0x00FFFFFF), 16777215);
    assert_int_equal(sqfs_fragment_size(0x01FFFFFF), 16777215);  /* Flag ignored */
}

/* Test: Fragment size maximum */
static void test_frag_size_max(void **state) {
    (void)state;

    /* Maximum fragment size is block_size (typically 128 KiB) */
    /* But encoded size can be up to 24 bits = 16 MiB */
    uint32_t max_encoded_size = 0x00FFFFFF;
    assert_int_equal(sqfs_fragment_size(max_encoded_size), 16777215);
}

/* ============================================================================
 * Fragment Index Tests
 * ============================================================================ */

/* Test: Fragment none value */
static void test_frag_none(void **state) {
    (void)state;

    /* 0xFFFFFFFF means no fragment */
    assert_int_equal(SQFS_FRAGMENT_NONE, 0xFFFFFFFF);

    assert_true(sqfs_fragment_is_none(SQFS_FRAGMENT_NONE));
    assert_true(sqfs_fragment_is_none(0xFFFFFFFF));

    assert_false(sqfs_fragment_is_none(0));
    assert_false(sqfs_fragment_is_none(100));
}

/* Test: Fragment index validity */
static void test_frag_index_valid(void **state) {
    (void)state;

    /* Valid indices are 0 to frag_count-1 */
    uint32_t frag_count = 100;

    /* Valid indices */
    assert_true(0 < frag_count);
    assert_true(99 < frag_count);

    /* Invalid indices */
    assert_false(100 < frag_count);
    assert_false(SQFS_FRAGMENT_NONE < frag_count);
}

/* ============================================================================
 * Fragment Table Structure Tests
 * ============================================================================ */

/* Test: Fragment table entries per block */
static void test_frag_entries_per_block(void **state) {
    (void)state;

    /* Each metadata block can hold 512 entries (8 KiB / 16 bytes) */
    assert_int_equal(8192 / sizeof(sqfs_frag_entry_t), 512);

    /* Verify helper function */
    assert_int_equal(sqfs_fragment_table_blocks(1), 1);
    assert_int_equal(sqfs_fragment_table_blocks(512), 1);
    assert_int_equal(sqfs_fragment_table_blocks(513), 2);
    assert_int_equal(sqfs_fragment_table_blocks(1024), 2);
    assert_int_equal(sqfs_fragment_table_blocks(1025), 3);
}

/* Test: Fragment table block count calculation */
static void test_frag_table_blocks(void **state) {
    (void)state;

    /* ceil(count / 512) */
    struct {
        uint32_t count;
        uint32_t expected_blocks;
    } test_cases[] = {
        { 0,    0 },
        { 1,    1 },
        { 511,  1 },
        { 512,  1 },
        { 513,  2 },
        { 1024, 2 },
        { 1025, 3 },
        { 10000, 20 },
    };

    for (size_t i = 0; i < sizeof(test_cases)/sizeof(test_cases[0]); i++) {
        uint32_t blocks = sqfs_fragment_table_blocks(test_cases[i].count);
        assert_int_equal(blocks, test_cases[i].expected_blocks);
    }
}

/* ============================================================================
 * Fragment Cache Tests
 * ============================================================================ */

/* Test: Fragment cache entry structure */
static void test_frag_cache_entry(void **state) {
    (void)state;

    sqfs_fragment_t cache_entry;
    memset(&cache_entry, 0, sizeof(cache_entry));

    cache_entry.block_start = 0x1000;
    cache_entry.data_size = 4096;
    cache_entry.is_cached = true;

    assert_int_equal(cache_entry.block_start, 0x1000);
    assert_int_equal(cache_entry.data_size, 4096);
    assert_true(cache_entry.is_cached);
}

/* Test: Fragment table structure */
static void test_frag_table_structure(void **state) {
    (void)state;

    sqfs_fragment_table_t table;
    sqfs_fragment_table_init(&table);

    assert_null(table.entries);
    assert_int_equal(table.count, 0);
    assert_false(table.loaded);

    sqfs_fragment_table_destroy(&table);
}

/* ============================================================================
 * Two-Level Lookup Tests
 * ============================================================================ */

/* Test: Fragment table lookup structure */
static void test_frag_two_level_lookup(void **state) {
    (void)state;

    /* Fragment table uses two-level lookup:
     * 1. Lookup table: array of 64-bit pointers to metadata blocks
     * 2. Metadata blocks: contain actual fragment entries
     */

    uint32_t frag_count = 1000;
    uint32_t blocks_needed = sqfs_fragment_table_blocks(frag_count);
    uint32_t entries_per_block = 512;

    /* Calculate block index for a given fragment index */
    uint32_t frag_idx = 750;
    uint32_t block_idx = frag_idx / entries_per_block;
    uint32_t entry_in_block = frag_idx % entries_per_block;

    assert_int_equal(block_idx, 1);      /* Second metadata block */
    assert_int_equal(entry_in_block, 238); /* 750 - 512 = 238 */
}

int main(void) {
    const struct CMUnitTest tests[] = {
        /* Entry tests */
        cmocka_unit_test(test_frag_entry_size),
        cmocka_unit_test(test_frag_entry_offsets),
        cmocka_unit_test(test_frag_entry_start),
        cmocka_unit_test(test_frag_entry_size_field),

        /* Size tests */
        cmocka_unit_test(test_frag_uncompressed_flag),
        cmocka_unit_test(test_frag_size_extraction),
        cmocka_unit_test(test_frag_size_max),

        /* Index tests */
        cmocka_unit_test(test_frag_none),
        cmocka_unit_test(test_frag_index_valid),

        /* Table structure tests */
        cmocka_unit_test(test_frag_entries_per_block),
        cmocka_unit_test(test_frag_table_blocks),

        /* Cache tests */
        cmocka_unit_test(test_frag_cache_entry),
        cmocka_unit_test(test_frag_table_structure),

        /* Lookup tests */
        cmocka_unit_test(test_frag_two_level_lookup),
    };

    return cmocka_run_group_tests_name("Fragment Table Tests", tests, NULL, NULL);
}