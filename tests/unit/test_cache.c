/*
 * Unit tests for LRU Cache
 */

#include <stdarg.h>
#include <stddef.h>
#include <setjmp.h>
#include <cmocka.h>

#include "cache.h"
#include <stdlib.h>
#include <string.h>

/* Test data */
static int free_count = 0;

static void test_free_fn(void *value) {
    (void)value;
    free_count++;
}

/* Test: Initialize and destroy cache */
static void test_cache_init_destroy(void **state) {
    (void)state;

    sqfs_cache_t cache;
    int ret = sqfs_cache_init(&cache, 100, 1024 * 1024, test_free_fn);
    assert_int_equal(ret, 0);
    assert_int_equal(cache.max_entries, 100);
    assert_int_equal(cache.max_memory, 1024 * 1024);
    assert_int_equal(cache.current_entries, 0);

    sqfs_cache_destroy(&cache);
}

/* Test: Put and get */
static void test_cache_put_get(void **state) {
    (void)state;

    sqfs_cache_t cache;
    sqfs_cache_init(&cache, 10, 1024, test_free_fn);

    char *value = strdup("test_value");
    int ret = sqfs_cache_put(&cache, 1, value, strlen(value) + 1);
    assert_int_equal(ret, 0);

    char *retrieved = (char *)sqfs_cache_get(&cache, 1);
    assert_non_null(retrieved);
    assert_string_equal(retrieved, "test_value");

    sqfs_cache_destroy(&cache);
}

/* Test: Cache miss */
static void test_cache_miss(void **state) {
    (void)state;

    sqfs_cache_t cache;
    sqfs_cache_init(&cache, 10, 1024, NULL);

    void *value = sqfs_cache_get(&cache, 999);
    assert_null(value);

    sqfs_cache_destroy(&cache);
}

/* Test: Remove entry */
static void test_cache_remove(void **state) {
    (void)state;

    free_count = 0;
    sqfs_cache_t cache;
    sqfs_cache_init(&cache, 10, 1024, test_free_fn);

    char *value = strdup("test");
    sqfs_cache_put(&cache, 1, value, 5);

    sqfs_cache_remove(&cache, 1);

    void *retrieved = sqfs_cache_get(&cache, 1);
    assert_null(retrieved);
    assert_int_equal(free_count, 1);

    sqfs_cache_destroy(&cache);
}

/* Test: LRU eviction */
static void test_cache_lru_eviction(void **state) {
    (void)state;

    free_count = 0;
    sqfs_cache_t cache;
    /* Only allow 2 entries */
    sqfs_cache_init(&cache, 2, 1024, test_free_fn);

    char *v1 = strdup("value1");
    char *v2 = strdup("value2");
    char *v3 = strdup("value3");

    sqfs_cache_put(&cache, 1, v1, 10);
    sqfs_cache_put(&cache, 2, v2, 10);
    /* This should evict key 1 (LRU) */
    sqfs_cache_put(&cache, 3, v3, 10);

    /* Key 1 should be evicted */
    void *retrieved = sqfs_cache_get(&cache, 1);
    assert_null(retrieved);

    /* Key 2 and 3 should still exist */
    retrieved = sqfs_cache_get(&cache, 2);
    assert_non_null(retrieved);
    retrieved = sqfs_cache_get(&cache, 3);
    assert_non_null(retrieved);

    sqfs_cache_destroy(&cache);
}

/* Test: Update existing key */
static void test_cache_update(void **state) {
    (void)state;

    free_count = 0;
    sqfs_cache_t cache;
    sqfs_cache_init(&cache, 10, 1024, test_free_fn);

    char *v1 = strdup("value1");
    char *v2 = strdup("value2");

    sqfs_cache_put(&cache, 1, v1, 10);
    sqfs_cache_put(&cache, 1, v2, 10);  /* Update same key */

    char *retrieved = (char *)sqfs_cache_get(&cache, 1);
    assert_string_equal(retrieved, "value2");

    /* Old value should have been freed */
    assert_int_equal(free_count, 1);

    sqfs_cache_destroy(&cache);
}

/* Test: Clear cache */
static void test_cache_clear(void **state) {
    (void)state;

    free_count = 0;
    sqfs_cache_t cache;
    sqfs_cache_init(&cache, 10, 1024, test_free_fn);

    for (int i = 0; i < 5; i++) {
        char *val = strdup("x");
        sqfs_cache_put(&cache, i, val, 2);
    }

    assert_int_equal(cache.current_entries, 5);

    sqfs_cache_clear(&cache);

    assert_int_equal(cache.current_entries, 0);
    assert_int_equal(free_count, 5);

    sqfs_cache_destroy(&cache);
}

int main(void) {
    const struct CMUnitTest tests[] = {
        cmocka_unit_test(test_cache_init_destroy),
        cmocka_unit_test(test_cache_put_get),
        cmocka_unit_test(test_cache_miss),
        cmocka_unit_test(test_cache_remove),
        cmocka_unit_test(test_cache_lru_eviction),
        cmocka_unit_test(test_cache_update),
        cmocka_unit_test(test_cache_clear),
    };

    return cmocka_run_group_tests(tests, NULL, NULL);
}