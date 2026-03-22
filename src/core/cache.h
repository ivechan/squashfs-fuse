/*
 * SquashFS-FUSE Cache System
 *
 * Generic LRU cache implementation with thread-safe access.
 * Uses hash table + doubly linked list for O(1) operations.
 *
 * Copyright (C) 2024
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef SQFS_CACHE_H
#define SQFS_CACHE_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>

/* Default cache configurations */
#define CACHE_INODE_MAX_ENTRIES    1024
#define CACHE_INODE_MAX_MEMORY     (4 * 1024 * 1024)   /* 4 MiB */

#define CACHE_DIR_MAX_ENTRIES      512
#define CACHE_DIR_MAX_MEMORY       (2 * 1024 * 1024)   /* 2 MiB */

#define CACHE_META_MAX_ENTRIES     256
#define CACHE_META_MAX_MEMORY      (8 * 1024 * 1024)   /* 8 MiB (each block is 8 KiB) */

#define CACHE_DATA_MAX_ENTRIES     128
#define CACHE_DATA_MAX_MEMORY      (16 * 1024 * 1024)  /* 16 MiB */

/* Cache key type */
typedef uint64_t cache_key_t;

/* Cache value free function callback */
typedef void (*cache_free_fn)(void *value);

/* Forward declaration */
struct cache_entry;

/* LRU Cache structure */
typedef struct {
    size_t max_entries;        /* Maximum number of entries */
    size_t current_entries;    /* Current number of entries */
    size_t max_memory;         /* Maximum memory usage in bytes */
    size_t current_memory;     /* Current memory usage in bytes */

    pthread_rwlock_t lock;     /* Read-write lock for thread safety */

    struct cache_entry *head;  /* LRU list head (most recently used) */
    struct cache_entry *tail;  /* LRU list tail (least recently used) */

    cache_free_fn free_fn;     /* Value free function */

    /* Hash table (separate chaining) */
    struct cache_entry **buckets;
    size_t bucket_count;        /* Number of hash buckets */
    size_t bucket_mask;         /* Mask for modulo operation (count - 1) */

    /* Statistics */
    size_t hits;                /* Cache hit count */
    size_t misses;              /* Cache miss count */
} sqfs_cache_t;

/*
 * Initialize a cache instance.
 *
 * @param cache      Pointer to cache structure to initialize
 * @param max_entries Maximum number of entries allowed
 * @param max_memory  Maximum memory usage in bytes
 * @param free_fn     Function to free cached values (can be NULL)
 * @return 0 on success, -1 on error (errno set)
 */
int sqfs_cache_init(sqfs_cache_t *cache, size_t max_entries,
                    size_t max_memory, cache_free_fn free_fn);

/*
 * Destroy a cache instance and free all resources.
 *
 * @param cache Pointer to cache structure to destroy
 */
void sqfs_cache_destroy(sqfs_cache_t *cache);

/*
 * Get a value from the cache.
 * Moves the entry to the head of the LRU list if found.
 *
 * @param cache Pointer to cache structure
 * @param key   Key to look up
 * @return Pointer to cached value, or NULL if not found
 */
void *sqfs_cache_get(sqfs_cache_t *cache, cache_key_t key);

/*
 * Put a value into the cache.
 * If the key already exists, the old value is replaced.
 * May evict entries to satisfy memory/entry limits.
 *
 * @param cache       Pointer to cache structure
 * @param key         Key for the entry
 * @param value       Pointer to value to cache
 * @param memory_size Memory size of the value in bytes
 * @return 0 on success, -1 on error (errno set)
 */
int sqfs_cache_put(sqfs_cache_t *cache, cache_key_t key, void *value,
                   size_t memory_size);

/*
 * Remove an entry from the cache.
 *
 * @param cache Pointer to cache structure
 * @param key   Key of entry to remove
 */
void sqfs_cache_remove(sqfs_cache_t *cache, cache_key_t key);

/*
 * Clear all entries from the cache.
 *
 * @param cache Pointer to cache structure
 */
void sqfs_cache_clear(sqfs_cache_t *cache);

/*
 * Get cache hit count.
 *
 * @param cache Pointer to cache structure
 * @return Number of cache hits
 */
size_t sqfs_cache_hits(sqfs_cache_t *cache);

/*
 * Get cache miss count.
 *
 * @param cache Pointer to cache structure
 * @return Number of cache misses
 */
size_t sqfs_cache_misses(sqfs_cache_t *cache);

/*
 * Get current number of entries in cache.
 *
 * @param cache Pointer to cache structure
 * @return Number of entries
 */
size_t sqfs_cache_entries(sqfs_cache_t *cache);

/*
 * Get current memory usage of cache.
 *
 * @param cache Pointer to cache structure
 * @return Memory usage in bytes
 */
size_t sqfs_cache_memory(sqfs_cache_t *cache);

#endif /* SQFS_CACHE_H */