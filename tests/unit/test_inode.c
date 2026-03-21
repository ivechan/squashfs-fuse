/*
 * Unit tests for Inode
 *
 * Tests inode types, parsing, and helper functions for all 14 inode types.
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "inode.h"
#include "fragment.h"
#include "utils.h"
#include <string.h>

/* ============================================================================
 * Inode Type Tests
 * ============================================================================ */

/* Test: Basic inode type values */
static void test_inode_basic_types(void **state) {
    (void)state;

    /* Verify basic types match spec (1-7) */
    assert_int_equal(SQFS_INODE_DIR,     1);
    assert_int_equal(SQFS_INODE_FILE,    2);
    assert_int_equal(SQFS_INODE_SYMLINK, 3);
    assert_int_equal(SQFS_INODE_BLKDEV,  4);
    assert_int_equal(SQFS_INODE_CHRDEV,  5);
    assert_int_equal(SQFS_INODE_FIFO,    6);
    assert_int_equal(SQFS_INODE_SOCKET,  7);
}

/* Test: Extended inode type values */
static void test_inode_extended_types(void **state) {
    (void)state;

    /* Verify extended types match spec (8-14) */
    assert_int_equal(SQFS_INODE_LDIR,     8);
    assert_int_equal(SQFS_INODE_LFILE,    9);
    assert_int_equal(SQFS_INODE_LSYMLINK, 10);
    assert_int_equal(SQFS_INODE_LBLKDEV,  11);
    assert_int_equal(SQFS_INODE_LCHRDEV,  12);
    assert_int_equal(SQFS_INODE_LFIFO,    13);
    assert_int_equal(SQFS_INODE_LSOCKET,  14);
}

/* Test: Is extended check */
static void test_inode_is_extended(void **state) {
    (void)state;

    /* Basic types are not extended */
    assert_false(sqfs_inode_is_extended(SQFS_INODE_DIR));
    assert_false(sqfs_inode_is_extended(SQFS_INODE_FILE));
    assert_false(sqfs_inode_is_extended(SQFS_INODE_SYMLINK));
    assert_false(sqfs_inode_is_extended(SQFS_INODE_BLKDEV));
    assert_false(sqfs_inode_is_extended(SQFS_INODE_CHRDEV));
    assert_false(sqfs_inode_is_extended(SQFS_INODE_FIFO));
    assert_false(sqfs_inode_is_extended(SQFS_INODE_SOCKET));

    /* Extended types are extended */
    assert_true(sqfs_inode_is_extended(SQFS_INODE_LDIR));
    assert_true(sqfs_inode_is_extended(SQFS_INODE_LFILE));
    assert_true(sqfs_inode_is_extended(SQFS_INODE_LSYMLINK));
    assert_true(sqfs_inode_is_extended(SQFS_INODE_LBLKDEV));
    assert_true(sqfs_inode_is_extended(SQFS_INODE_LCHRDEV));
    assert_true(sqfs_inode_is_extended(SQFS_INODE_LFIFO));
    assert_true(sqfs_inode_is_extended(SQFS_INODE_LSOCKET));
}

/* Test: Basic type from extended */
static void test_inode_basic_from_extended(void **state) {
    (void)state;

    /* Extended types map to basic types */
    assert_int_equal(sqfs_inode_basic_type(SQFS_INODE_LDIR),     SQFS_INODE_DIR);
    assert_int_equal(sqfs_inode_basic_type(SQFS_INODE_LFILE),    SQFS_INODE_FILE);
    assert_int_equal(sqfs_inode_basic_type(SQFS_INODE_LSYMLINK), SQFS_INODE_SYMLINK);
    assert_int_equal(sqfs_inode_basic_type(SQFS_INODE_LBLKDEV),  SQFS_INODE_BLKDEV);
    assert_int_equal(sqfs_inode_basic_type(SQFS_INODE_LCHRDEV),  SQFS_INODE_CHRDEV);
    assert_int_equal(sqfs_inode_basic_type(SQFS_INODE_LFIFO),    SQFS_INODE_FIFO);
    assert_int_equal(sqfs_inode_basic_type(SQFS_INODE_LSOCKET),  SQFS_INODE_SOCKET);

    /* Basic types return unchanged */
    assert_int_equal(sqfs_inode_basic_type(SQFS_INODE_DIR), SQFS_INODE_DIR);
    assert_int_equal(sqfs_inode_basic_type(SQFS_INODE_FILE), SQFS_INODE_FILE);
}

/* Test: Is directory check */
static void test_inode_is_dir(void **state) {
    (void)state;

    assert_true(sqfs_inode_is_dir(SQFS_INODE_DIR));
    assert_true(sqfs_inode_is_dir(SQFS_INODE_LDIR));

    assert_false(sqfs_inode_is_dir(SQFS_INODE_FILE));
    assert_false(sqfs_inode_is_dir(SQFS_INODE_LFILE));
    assert_false(sqfs_inode_is_dir(SQFS_INODE_SYMLINK));
}

/* Test: Is file check */
static void test_inode_is_file(void **state) {
    (void)state;

    assert_true(sqfs_inode_is_file(SQFS_INODE_FILE));
    assert_true(sqfs_inode_is_file(SQFS_INODE_LFILE));

    assert_false(sqfs_inode_is_file(SQFS_INODE_DIR));
    assert_false(sqfs_inode_is_file(SQFS_INODE_LDIR));
    assert_false(sqfs_inode_is_file(SQFS_INODE_SYMLINK));
}

/* Test: Is symlink check */
static void test_inode_is_symlink(void **state) {
    (void)state;

    assert_true(sqfs_inode_is_symlink(SQFS_INODE_SYMLINK));
    assert_true(sqfs_inode_is_symlink(SQFS_INODE_LSYMLINK));

    assert_false(sqfs_inode_is_symlink(SQFS_INODE_FILE));
    assert_false(sqfs_inode_is_symlink(SQFS_INODE_DIR));
}

/* Test: Is device check */
static void test_inode_is_dev(void **state) {
    (void)state;

    assert_true(sqfs_inode_is_dev(SQFS_INODE_BLKDEV));
    assert_true(sqfs_inode_is_dev(SQFS_INODE_CHRDEV));
    assert_true(sqfs_inode_is_dev(SQFS_INODE_LBLKDEV));
    assert_true(sqfs_inode_is_dev(SQFS_INODE_LCHRDEV));

    assert_false(sqfs_inode_is_dev(SQFS_INODE_FILE));
    assert_false(sqfs_inode_is_dev(SQFS_INODE_FIFO));
}

/* Test: Is IPC check */
static void test_inode_is_ipc(void **state) {
    (void)state;

    assert_true(sqfs_inode_is_ipc(SQFS_INODE_FIFO));
    assert_true(sqfs_inode_is_ipc(SQFS_INODE_SOCKET));
    assert_true(sqfs_inode_is_ipc(SQFS_INODE_LFIFO));
    assert_true(sqfs_inode_is_ipc(SQFS_INODE_LSOCKET));

    assert_false(sqfs_inode_is_ipc(SQFS_INODE_FILE));
    assert_false(sqfs_inode_is_ipc(SQFS_INODE_BLKDEV));
}

/* ============================================================================
 * Inode Header Tests
 * ============================================================================ */

/* Test: Inode header size */
static void test_inode_header_size(void **state) {
    (void)state;

    /* Common header is 16 bytes */
    assert_int_equal(sizeof(sqfs_inode_header_t), 16);
}

/* Test: Inode header field offsets */
static void test_inode_header_offsets(void **state) {
    (void)state;

    assert_int_equal(offsetof(sqfs_inode_header_t, type),         0);
    assert_int_equal(offsetof(sqfs_inode_header_t, permissions),  2);
    assert_int_equal(offsetof(sqfs_inode_header_t, uid_idx),      4);
    assert_int_equal(offsetof(sqfs_inode_header_t, gid_idx),      6);
    assert_int_equal(offsetof(sqfs_inode_header_t, mtime),        8);
    assert_int_equal(offsetof(sqfs_inode_header_t, inode_number), 12);
}

/* ============================================================================
 * Directory Inode Tests
 * ============================================================================ */

/* Test: Basic directory inode size */
static void test_inode_dir_size(void **state) {
    (void)state;

    /* Basic directory inode: header(16) + 4*3 + 2*2 = 32 bytes */
    assert_int_equal(sizeof(sqfs_inode_dir_t), 32);
}

/* Test: Extended directory inode size */
static void test_inode_ldir_size(void **state) {
    (void)state;

    /* Extended directory inode: header(16) + 4*4 + 2*2 + 4 = 40 bytes */
    assert_int_equal(sizeof(sqfs_inode_ldir_t), 40);
}

/* Test: Directory inode fields */
static void test_inode_dir_fields(void **state) {
    (void)state;

    sqfs_inode_dir_t dir;
    memset(&dir, 0, sizeof(dir));

    /* Set header fields */
    dir.header.type = SQFS_INODE_DIR;
    dir.header.permissions = 0755;
    dir.header.uid_idx = 0;
    dir.header.gid_idx = 0;
    dir.header.mtime = 1234567890;
    dir.header.inode_number = 1;

    /* Set directory-specific fields */
    dir.block_idx = 0x1000;
    dir.link_count = 2;
    dir.file_size = 100;
    dir.block_offset = 0;
    dir.parent_inode = 1;

    assert_int_equal(dir.header.type, SQFS_INODE_DIR);
    assert_int_equal(dir.block_idx, 0x1000);
    assert_int_equal(dir.link_count, 2);
}

/* ============================================================================
 * File Inode Tests
 * ============================================================================ */

/* Test: Basic file inode size */
static void test_inode_file_size(void **state) {
    (void)state;

    /* Basic file inode: header(16) + 4*4 = 32 bytes (without block_sizes) */
    assert_int_equal(sizeof(sqfs_inode_file_t), 32);
}

/* Test: Extended file inode size */
static void test_inode_lfile_size(void **state) {
    (void)state;

    /* Extended file inode: header(16) + 8*3 + 4*4 = 40 bytes */
    assert_int_equal(sizeof(sqfs_inode_lfile_t), 56);
}

/* Test: File inode fragment index */
static void test_inode_file_frag_idx(void **state) {
    (void)state;

    sqfs_inode_file_t file;
    memset(&file, 0, sizeof(file));

    /* No fragment */
    file.frag_idx = 0xFFFFFFFF;
    assert_true(file.frag_idx == SQFS_FRAGMENT_NONE);

    /* Has fragment */
    file.frag_idx = 0;
    assert_false(file.frag_idx == SQFS_FRAGMENT_NONE);
}

/* ============================================================================
 * Symlink Inode Tests
 * ============================================================================ */

/* Test: Basic symlink inode size */
static void test_inode_symlink_size(void **state) {
    (void)state;

    /* Basic symlink inode: header(16) + 4*2 = 24 bytes (without target) */
    assert_int_equal(sizeof(sqfs_inode_symlink_t), 24);
}

/* Test: Extended symlink inode size */
static void test_inode_lsymlink_size(void **state) {
    (void)state;

    /* Extended symlink inode: header(16) + 4*2 = 24 bytes (without target and xattr) */
    assert_int_equal(sizeof(sqfs_inode_lsymlink_t), 24);
}

/* ============================================================================
 * Device Inode Tests
 * ============================================================================ */

/* Test: Device inode size */
static void test_inode_dev_size(void **state) {
    (void)state;

    /* Device inode: header(16) + 4*2 = 24 bytes */
    assert_int_equal(sizeof(sqfs_inode_dev_t), 24);
}

/* Test: Extended device inode size */
static void test_inode_ldev_size(void **state) {
    (void)state;

    /* Extended device inode: header(16) + 4*3 = 28 bytes */
    assert_int_equal(sizeof(sqfs_inode_ldev_t), 28);
}

/* Test: Device number encoding */
static void test_inode_dev_number(void **state) {
    (void)state;

    /* Device number encoding: major/minor */
    /* major = (dev & 0xFFF00) >> 8 */
    /* minor = (dev & 0x000FF) | ((dev >> 12) & 0xFFF00) */

    uint32_t dev = 0x00012345;  /* major=0x123, minor=0x45 */

    uint32_t major = sqfs_dev_major(dev);
    uint32_t minor = sqfs_dev_minor(dev);

    /* Verify extraction */
    assert_int_equal(major, 0x123);
    assert_int_equal(minor, 0x45);
}

/* Test: Device number encoding complex */
static void test_inode_dev_number_complex(void **state) {
    (void)state;

    /* Test with large minor number (> 255) */
    /* For dev = 0x10000100:
     * major = (0x10000100 & 0xFFF00) >> 8 = 0x100 >> 8 = 0x01
     * minor = (0x10000100 & 0x000FF) | ((0x10000100 >> 12) & 0xFFF00)
     *       = 0x00 | (0x10000 & 0xFFF00) = 0x00 | 0x10000 = 0x10000
     */
    uint32_t dev = 0x10000100;

    uint32_t major = sqfs_dev_major(dev);
    uint32_t minor = sqfs_dev_minor(dev);

    assert_int_equal(major, 0x01);
    assert_int_equal(minor, 0x10000);
}

/* ============================================================================
 * IPC Inode Tests
 * ============================================================================ */

/* Test: IPC inode size */
static void test_inode_ipc_size(void **state) {
    (void)state;

    /* IPC inode: header(16) + 4 = 20 bytes */
    assert_int_equal(sizeof(sqfs_inode_ipc_t), 20);
}

/* Test: Extended IPC inode size */
static void test_inode_lipc_size(void **state) {
    (void)state;

    /* Extended IPC inode: header(16) + 4*2 = 24 bytes */
    assert_int_equal(sizeof(sqfs_inode_lipc_t), 24);
}

/* ============================================================================
 * Runtime Inode Tests
 * ============================================================================ */

/* Test: Runtime inode type name */
static void test_inode_type_name(void **state) {
    (void)state;

    /* All types should have valid names */
    assert_non_null(sqfs_inode_type_name(SQFS_INODE_DIR));
    assert_non_null(sqfs_inode_type_name(SQFS_INODE_FILE));
    assert_non_null(sqfs_inode_type_name(SQFS_INODE_SYMLINK));
    assert_non_null(sqfs_inode_type_name(SQFS_INODE_BLKDEV));
    assert_non_null(sqfs_inode_type_name(SQFS_INODE_CHRDEV));
    assert_non_null(sqfs_inode_type_name(SQFS_INODE_FIFO));
    assert_non_null(sqfs_inode_type_name(SQFS_INODE_SOCKET));
    assert_non_null(sqfs_inode_type_name(SQFS_INODE_LDIR));
    assert_non_null(sqfs_inode_type_name(SQFS_INODE_LFILE));
}

/* Test: Block count calculation */
static void test_inode_block_count(void **state) {
    (void)state;

    uint32_t block_size = 131072;  /* 128 KiB */

    /* File without fragment: ceil(file_size / block_size) */
    assert_int_equal(sqfs_calc_block_count(131072, block_size, false), 1);
    assert_int_equal(sqfs_calc_block_count(131073, block_size, false), 2);
    assert_int_equal(sqfs_calc_block_count(262144, block_size, false), 2);

    /* File with fragment: floor(file_size / block_size) */
    assert_int_equal(sqfs_calc_block_count(131072, block_size, true), 1);
    assert_int_equal(sqfs_calc_block_count(131073, block_size, true), 1);
    assert_int_equal(sqfs_calc_block_count(262144, block_size, true), 2);
    assert_int_equal(sqfs_calc_block_count(262145, block_size, true), 2);

    /* Small file with fragment: 0 full blocks */
    assert_int_equal(sqfs_calc_block_count(1000, block_size, true), 0);

    /* Empty file: 0 blocks */
    assert_int_equal(sqfs_calc_block_count(0, block_size, false), 0);
    assert_int_equal(sqfs_calc_block_count(0, block_size, true), 0);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        /* Type tests */
        cmocka_unit_test(test_inode_basic_types),
        cmocka_unit_test(test_inode_extended_types),
        cmocka_unit_test(test_inode_is_extended),
        cmocka_unit_test(test_inode_basic_from_extended),
        cmocka_unit_test(test_inode_is_dir),
        cmocka_unit_test(test_inode_is_file),
        cmocka_unit_test(test_inode_is_symlink),
        cmocka_unit_test(test_inode_is_dev),
        cmocka_unit_test(test_inode_is_ipc),

        /* Header tests */
        cmocka_unit_test(test_inode_header_size),
        cmocka_unit_test(test_inode_header_offsets),

        /* Directory inode tests */
        cmocka_unit_test(test_inode_dir_size),
        cmocka_unit_test(test_inode_ldir_size),
        cmocka_unit_test(test_inode_dir_fields),

        /* File inode tests */
        cmocka_unit_test(test_inode_file_size),
        cmocka_unit_test(test_inode_lfile_size),
        cmocka_unit_test(test_inode_file_frag_idx),

        /* Symlink inode tests */
        cmocka_unit_test(test_inode_symlink_size),
        cmocka_unit_test(test_inode_lsymlink_size),

        /* Device inode tests */
        cmocka_unit_test(test_inode_dev_size),
        cmocka_unit_test(test_inode_ldev_size),
        cmocka_unit_test(test_inode_dev_number),
        cmocka_unit_test(test_inode_dev_number_complex),

        /* IPC inode tests */
        cmocka_unit_test(test_inode_ipc_size),
        cmocka_unit_test(test_inode_lipc_size),

        /* Runtime tests */
        cmocka_unit_test(test_inode_type_name),
        cmocka_unit_test(test_inode_block_count),
    };

    return cmocka_run_group_tests_name("Inode Tests", tests, NULL, NULL);
}