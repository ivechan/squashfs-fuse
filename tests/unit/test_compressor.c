/*
 * Unit tests for Compressor
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "compressor.h"
#include <stdlib.h>
#include <string.h>

/* Test: Create zlib compressor */
static void test_compressor_zlib_create(void **state) {
    (void)state;

    sqfs_compressor_t *comp = sqfs_compressor_create(SQFS_COMP_GZIP);
    assert_non_null(comp);
    assert_string_equal(comp->name, "gzip");

    sqfs_compressor_destroy(comp);
}

/* Test: Create zstd compressor */
static void test_compressor_zstd_create(void **state) {
    (void)state;

    sqfs_compressor_t *comp = sqfs_compressor_create(SQFS_COMP_ZSTD);
    assert_non_null(comp);
    assert_string_equal(comp->name, "zstd");

    sqfs_compressor_destroy(comp);
}

/* Test: Invalid compressor type */
static void test_compressor_invalid(void **state) {
    (void)state;

    sqfs_compressor_t *comp = sqfs_compressor_create((sqfs_compressor_id_t)99);
    assert_null(comp);
}

/* Test: Check supported compressors */
static void test_compressor_is_supported(void **state) {
    (void)state;

    assert_true(sqfs_compressor_is_supported(SQFS_COMP_GZIP));
    assert_true(sqfs_compressor_is_supported(SQFS_COMP_ZSTD));
    assert_false(sqfs_compressor_is_supported((sqfs_compressor_id_t)99));
    assert_false(sqfs_compressor_is_supported(SQFS_COMP_LZMA));
}

/* Test: Zlib decompress - raw deflate format */
static void test_compressor_zlib_decompress(void **state) {
    (void)state;

    sqfs_compressor_t *comp = sqfs_compressor_create(SQFS_COMP_GZIP);
    assert_non_null(comp);

    /*
     * Compressed data for "Hello, SquashFS!" using raw deflate
     * Generated with Python: zlib.compress(b'Hello, SquashFS!', 9)[2:-4]
     */
    const unsigned char compressed[] = {
        0xf3, 0x48, 0xcd, 0xc9, 0xc9, 0xd7, 0x51, 0x08,
        0x2e, 0x2c, 0x4d, 0x2c, 0xce, 0x70, 0x0b, 0x56,
        0x04, 0x00
    };
    const char *expected = "Hello, SquashFS!";

    char decompressed[64];
    size_t decomp_size = 0;
    int ret = comp->decompress(compressed, sizeof(compressed),
                               decompressed, sizeof(decompressed),
                               &decomp_size);
    assert_int_equal(ret, SQFS_COMP_OK);
    assert_int_equal(decomp_size, strlen(expected));

    decompressed[decomp_size] = '\0';
    assert_string_equal(decompressed, expected);

    sqfs_compressor_destroy(comp);
}

/* Test: Decompress with insufficient buffer */
static void test_compressor_buffer_too_small(void **state) {
    (void)state;

    sqfs_compressor_t *comp = sqfs_compressor_create(SQFS_COMP_GZIP);
    assert_non_null(comp);

    /* Compressed "Hello" */
    const unsigned char compressed[] = {0xf3, 0x48, 0xcd, 0xc9, 0xc9, 0x04, 0x00};

    char small_buf[2];
    size_t decomp_size = 0;
    int ret = comp->decompress(compressed, sizeof(compressed),
                               small_buf, sizeof(small_buf),
                               &decomp_size);
    assert_int_equal(ret, SQFS_COMP_OVERFLOW);

    sqfs_compressor_destroy(comp);
}

/* Test: Decompress corrupt data */
static void test_compressor_corrupt_data(void **state) {
    (void)state;

    sqfs_compressor_t *comp = sqfs_compressor_create(SQFS_COMP_GZIP);
    assert_non_null(comp);

    /* Invalid/corrupt compressed data */
    const unsigned char corrupt[] = {0xff, 0xfe, 0xfd, 0xfc};

    char decompressed[64];
    size_t decomp_size = 0;
    int ret = comp->decompress(corrupt, sizeof(corrupt),
                               decompressed, sizeof(decompressed),
                               &decomp_size);
    assert_int_equal(ret, SQFS_COMP_CORRUPT);

    sqfs_compressor_destroy(comp);
}

/* Test: Error message strings */
static void test_compressor_strerror(void **state) {
    (void)state;

    assert_non_null(sqfs_compressor_strerror(SQFS_COMP_OK));
    assert_non_null(sqfs_compressor_strerror(SQFS_COMP_ERROR));
    assert_non_null(sqfs_compressor_strerror(SQFS_COMP_OVERFLOW));
    assert_non_null(sqfs_compressor_strerror(SQFS_COMP_CORRUPT));
    assert_non_null(sqfs_compressor_strerror(SQFS_COMP_UNSUPPORTED));

    /* Unknown error should also return a string */
    assert_non_null(sqfs_compressor_strerror(-999));
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_compressor_zlib_create),
        cmocka_unit_test(test_compressor_zstd_create),
        cmocka_unit_test(test_compressor_invalid),
        cmocka_unit_test(test_compressor_is_supported),
        cmocka_unit_test(test_compressor_zlib_decompress),
        cmocka_unit_test(test_compressor_buffer_too_small),
        cmocka_unit_test(test_compressor_corrupt_data),
        cmocka_unit_test(test_compressor_strerror),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}