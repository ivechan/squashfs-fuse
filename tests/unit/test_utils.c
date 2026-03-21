/*
 * Unit tests for Utility Functions
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "utils.h"
#include "superblock.h"
#include <stdlib.h>
#include <string.h>

/* Test: Memory allocation functions */
static void test_utils_malloc_free(void **state) {
    (void)state;

    void *ptr = sqfs_malloc(100);
    assert_non_null(ptr);

    memset(ptr, 'A', 100);
    assert_int_equal(((char *)ptr)[0], 'A');
    assert_int_equal(((char *)ptr)[99], 'A');

    sqfs_free(ptr);
}

/* Test: Calloc */
static void test_utils_calloc(void **state) {
    (void)state;

    int *arr = sqfs_calloc(10, sizeof(int));
    assert_non_null(arr);

    /* Should be zero-initialized */
    for (int i = 0; i < 10; i++) {
        assert_int_equal(arr[i], 0);
    }

    sqfs_free(arr);
}

/* Test: Realloc */
static void test_utils_realloc(void **state) {
    (void)state;

    char *ptr = sqfs_malloc(10);
    assert_non_null(ptr);
    memset(ptr, 'A', 10);

    ptr = sqfs_realloc(ptr, 100);
    assert_non_null(ptr);

    /* Original data should be preserved */
    assert_int_equal(ptr[0], 'A');

    sqfs_free(ptr);
}

/* Test: Metadata reference encoding */
static void test_utils_meta_ref(void **state) {
    (void)state;

    uint64_t pos = 0x12345678ABCD;
    uint16_t offset = 0x1234;

    uint64_t ref = sqfs_make_meta_ref(pos, offset);

    uint64_t decoded_pos = sqfs_meta_block_pos(ref);
    uint16_t decoded_offset = sqfs_meta_block_offset(ref);

    assert_int_equal(decoded_pos, pos);
    assert_int_equal(decoded_offset, offset);
}

/* Test: Metadata block position extraction */
static void test_utils_meta_block_pos(void **state) {
    (void)state;

    /* ref = (pos << 16) | offset */
    uint64_t ref = 0x0000ABCD12340000ULL;  /* pos = 0xABCD1234, offset = 0 */
    uint64_t pos = sqfs_meta_block_pos(ref);
    assert_int_equal(pos, 0xABCD1234);
}

/* Test: Metadata block offset extraction */
static void test_utils_meta_block_offset(void **state) {
    (void)state;

    uint64_t ref = 0x0000000000005678ULL;
    uint16_t offset = sqfs_meta_block_offset(ref);
    assert_int_equal(offset, 0x5678);
}

/* Test: Table valid check */
static void test_utils_table_valid(void **state) {
    (void)state;

    /* Valid table positions */
    assert_true(sqfs_table_valid(0x1000));
    assert_true(sqfs_table_valid(0xABCDEF00));

    /* Invalid table positions */
    assert_false(sqfs_table_valid(0xFFFFFFFFFFFFFFFFULL));
}

/* Test: Little-endian conversion - 16 bit */
static void test_utils_le16(void **state) {
    (void)state;

    uint8_t bytes[2] = {0x34, 0x12};  /* Little-endian 0x1234 */
    uint16_t val = sqfs_le16_to_cpu(bytes);
    assert_int_equal(val, 0x1234);
}

/* Test: Little-endian conversion - 32 bit */
static void test_utils_le32(void **state) {
    (void)state;

    uint8_t bytes[4] = {0x78, 0x56, 0x34, 0x12};  /* Little-endian 0x12345678 */
    uint32_t val = sqfs_le32_to_cpu(bytes);
    assert_int_equal(val, 0x12345678);
}

/* Test: Little-endian conversion - 64 bit */
static void test_utils_le64(void **state) {
    (void)state;

    uint8_t bytes[8] = {0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01};
    uint64_t val = sqfs_le64_to_cpu(bytes);
    assert_int_equal(val, 0x0123456789ABCDEFULL);
}

/* Test: Error code to errno conversion */
static void test_utils_errno(void **state) {
    (void)state;

    assert_int_equal(sqfs_errno(SQFS_OK), 0);
    assert_int_equal(sqfs_errno(SQFS_ERR_NOT_FOUND), ENOENT);
    assert_int_equal(sqfs_errno(SQFS_ERR_NO_MEMORY), ENOMEM);
    assert_int_equal(sqfs_errno(SQFS_ERR_CORRUPT), EIO);
    assert_int_equal(sqfs_errno(SQFS_ERR_OVERFLOW), EOVERFLOW);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_utils_malloc_free),
        cmocka_unit_test(test_utils_calloc),
        cmocka_unit_test(test_utils_realloc),
        cmocka_unit_test(test_utils_meta_ref),
        cmocka_unit_test(test_utils_meta_block_pos),
        cmocka_unit_test(test_utils_meta_block_offset),
        cmocka_unit_test(test_utils_table_valid),
        cmocka_unit_test(test_utils_le16),
        cmocka_unit_test(test_utils_le32),
        cmocka_unit_test(test_utils_le64),
        cmocka_unit_test(test_utils_errno),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}