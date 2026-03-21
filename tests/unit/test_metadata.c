/*
 * Unit tests for Metadata Block handling
 *
 * Tests metadata block header parsing, size limits, and reference encoding.
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "utils.h"
#include <string.h>

/* ============================================================================
 * Metadata Block Constants Tests
 * ============================================================================ */

/* Test: Metadata block constants */
static void test_meta_constants(void **state) {
    (void)state;

    /* Maximum metadata block size is 8 KiB */
    assert_int_equal(SQFS_META_MAX_SIZE, 8192);

    /* Uncompressed flag is bit 15 of header */
    assert_int_equal(SQFS_META_UNCOMPRESSED_FLAG, 0x8000);
}

/* ============================================================================
 * Metadata Reference Tests
 * ============================================================================ */

/* Test: Metadata reference encoding */
static void test_meta_ref_encoding(void **state) {
    (void)state;

    /* 64-bit reference: (block_pos << 16) | offset */
    uint64_t pos = 0x0000ABCD1234;
    uint16_t offset = 0x5678;

    uint64_t ref = sqfs_make_meta_ref(pos, offset);

    assert_int_equal(sqfs_meta_block_pos(ref), pos);
    assert_int_equal(sqfs_meta_block_offset(ref), offset);
}

/* Test: Metadata reference position extraction */
static void test_meta_ref_position(void **state) {
    (void)state;

    /* High 48 bits are block position */
    uint64_t ref = 0xABCD123400000000ULL;
    uint64_t pos = sqfs_meta_block_pos(ref);
    assert_int_equal(pos, 0xABCD1234ULL);

    /* Position is relative to table start, not file start */
    ref = 0x0000000100000000ULL;
    pos = sqfs_meta_block_pos(ref);
    assert_int_equal(pos, 1);
}

/* Test: Metadata reference offset extraction */
static void test_meta_ref_offset(void **state) {
    (void)state;

    /* Low 16 bits are offset within uncompressed block */
    uint64_t ref = 0x0000000000001234ULL;
    uint16_t offset = sqfs_meta_block_offset(ref);
    assert_int_equal(offset, 0x1234);

    /* Offset must be < 8192 (within block) */
    ref = 0x0000000000001FFFULL;  /* Max valid offset */
    offset = sqfs_meta_block_offset(ref);
    assert_int_equal(offset, 0x1FFF);

    /* Invalid offset (>= 8192) */
    ref = 0x0000000000002000ULL;
    offset = sqfs_meta_block_offset(ref);
    assert_int_equal(offset, 0x2000);  /* Value is extracted, but invalid */
}

/* Test: Metadata reference edge cases */
static void test_meta_ref_edge_cases(void **state) {
    (void)state;

    /* Zero reference */
    uint64_t ref = 0;
    assert_int_equal(sqfs_meta_block_pos(ref), 0);
    assert_int_equal(sqfs_meta_block_offset(ref), 0);

    /* Maximum position (48 bits) */
    ref = 0xFFFFFFFF00000000ULL;
    assert_int_equal(sqfs_meta_block_pos(ref), 0xFFFFFFFFULL);

    /* Maximum offset (16 bits) */
    ref = 0x000000000000FFFFULL;
    assert_int_equal(sqfs_meta_block_offset(ref), 0xFFFF);
}

/* ============================================================================
 * Metadata Block Header Tests
 * ============================================================================ */

/* Test: Metadata block header format */
static void test_meta_header_format(void **state) {
    (void)state;

    /* Header is 16 bits:
     * - Bit 15: uncompressed flag
     * - Bits 0-14: compressed size
     */

    /* Compressed block, size = 4096 */
    uint16_t header = 0x1000;
    assert_false((header & SQFS_META_UNCOMPRESSED_FLAG) != 0);
    assert_int_equal(header & 0x7FFF, 4096);

    /* Uncompressed block, size = 4096 */
    header = 0x9000;
    assert_true((header & SQFS_META_UNCOMPRESSED_FLAG) != 0);
    assert_int_equal(header & 0x7FFF, 4096);

    /* Maximum compressed size = 8191 (0x1FFF) */
    header = 0x1FFF;
    assert_int_equal(header & 0x7FFF, 8191);
}

/* Test: Metadata block size validation */
static void test_meta_size_validation(void **state) {
    (void)state;

    /* Valid sizes: 0 to 8191 (0x1FFF) */
    uint16_t valid_sizes[] = { 0, 1, 100, 4096, 8191 };

    for (size_t i = 0; i < sizeof(valid_sizes)/sizeof(valid_sizes[0]); i++) {
        uint16_t size = valid_sizes[i];
        assert_true(size < SQFS_META_MAX_SIZE);
    }

    /* Size of 8192 (0x2000) would be invalid in lower 15 bits */
    /* But 0x2000 = 8192 in 15 bits is impossible (max is 0x1FFF = 8191) */
}

/* Test: Uncompressed block detection */
static void test_meta_uncompressed_detection(void **state) {
    (void)state;

    uint16_t compressed_header = 0x1000;
    uint16_t uncompressed_header = 0x9000;

    assert_false((compressed_header & SQFS_META_UNCOMPRESSED_FLAG) != 0);
    assert_true((uncompressed_header & SQFS_META_UNCOMPRESSED_FLAG) != 0);
}

/* ============================================================================
 * Cross-Block Entry Tests
 * ============================================================================ */

/* Test: Entry crossing block boundary */
static void test_meta_cross_block_entry(void **state) {
    (void)state;

    /* Entries can cross metadata block boundaries
     * If an entry starts at offset 8180 in a block and is 40 bytes,
     * it continues at offset 0 of the next block.
     */

    uint16_t block_size = 8192;
    uint16_t entry_offset = 8180;
    uint16_t entry_size = 40;
    uint16_t remaining_in_block = block_size - entry_offset;

    assert_int_equal(remaining_in_block, 12);

    /* Entry spans: 12 bytes at end of block, 28 bytes in next block */
    uint16_t bytes_in_next = entry_size - remaining_in_block;
    assert_int_equal(bytes_in_next, 28);
}

/* Test: Maximum entry size */
static void test_meta_max_entry_size(void **state) {
    (void)state;

    /* Largest inode is extended file inode: 56 bytes + block_sizes[] */
    /* Extended directory inode: 40 bytes + directory index entries */

    /* Even the largest entries fit within two blocks */
    assert_true(56 < 8192 * 2);
}

/* ============================================================================
 * Block Size Utilities Tests
 * ============================================================================ */

/* Test: Block uncompressed detection */
static void test_block_uncompressed(void **state) {
    (void)state;

    /* Data blocks also use bit 24 for uncompressed flag */
    uint32_t compressed_size = 0x00100000;  /* 1 MiB compressed */
    uint32_t uncompressed_size = 0x01100000;  /* Uncompressed, 1 MiB */

    assert_false(sqfs_block_is_uncompressed(compressed_size));
    assert_true(sqfs_block_is_uncompressed(uncompressed_size));

    assert_int_equal(sqfs_block_size(compressed_size), 1048576);
    assert_int_equal(sqfs_block_size(uncompressed_size), 1048576);
}

/* Test: Block size extraction */
static void test_block_size_extraction(void **state) {
    (void)state;

    /* Lower 24 bits are the actual size */
    assert_int_equal(sqfs_block_size(0x00000000), 0);
    assert_int_equal(sqfs_block_size(0x00000100), 256);
    assert_int_equal(sqfs_block_size(0x00FFFFFF), 16777215);
    assert_int_equal(sqfs_block_size(0x01FFFFFF), 16777215);
}

/* Test: Zero size block (sparse file hole) */
static void test_block_zero_size(void **state) {
    (void)state;

    /* Size of 0 indicates a hole in sparse file */
    uint32_t hole_size = 0;

    assert_int_equal(sqfs_block_size(hole_size), 0);
    assert_false(sqfs_block_is_uncompressed(hole_size));

    /* Reading a hole returns all zeros without disk I/O */
}

/* Test: Block size exceeds input (stored uncompressed) */
static void test_block_size_exceeds_input(void **state) {
    (void)state;

    /* If compression would make data larger, store uncompressed */
    /* Uncompressed block size equals input block size */

    uint32_t block_size = 131072;  /* 128 KiB */
    uint32_t stored_size = block_size | (1U << 24);  /* With uncompressed flag */

    assert_true(sqfs_block_is_uncompressed(stored_size));
    assert_int_equal(sqfs_block_size(stored_size), block_size);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        /* Constants tests */
        cmocka_unit_test(test_meta_constants),

        /* Reference tests */
        cmocka_unit_test(test_meta_ref_encoding),
        cmocka_unit_test(test_meta_ref_position),
        cmocka_unit_test(test_meta_ref_offset),
        cmocka_unit_test(test_meta_ref_edge_cases),

        /* Header tests */
        cmocka_unit_test(test_meta_header_format),
        cmocka_unit_test(test_meta_size_validation),
        cmocka_unit_test(test_meta_uncompressed_detection),

        /* Cross-block tests */
        cmocka_unit_test(test_meta_cross_block_entry),
        cmocka_unit_test(test_meta_max_entry_size),

        /* Block size tests */
        cmocka_unit_test(test_block_uncompressed),
        cmocka_unit_test(test_block_size_extraction),
        cmocka_unit_test(test_block_zero_size),
        cmocka_unit_test(test_block_size_exceeds_input),
    };

    return cmocka_run_group_tests_name("Metadata Block Tests", tests, NULL, NULL);
}