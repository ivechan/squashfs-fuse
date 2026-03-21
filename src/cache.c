/*
 * SquashFS-FUSE Cache System
 *
 * Generic LRU cache implementation with thread-safe access.
 * Uses hash table + doubly linked list for O(1) operations.
 *
 * Copyright (C) 2024
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "cache.h"
#include "log.h"
#include "stats.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>

/* Hash table load factor threshold for resizing */
#define CACHE_LOAD_FACTOR_THRESHOLD  0.75
#define CACHE_INITIAL_BUCKETS        64

/* Minimum buckets for cache entries (must be power of 2) */
#define CACHE_MIN_BUCKETS            16

/* Internal cache entry structure */
struct cache_entry {
    cache_key_t key;            /* Cache key */
    void *value;                /* Cached value */
    size_t memory_size;         /* Memory size of value */

    /* LRU list links */
    struct cache_entry *prev;   /* Previous entry in LRU list */
    struct cache_entry *next;   /* Next entry in LRU list */

    /* Hash table links (for collision chaining) */
    struct cache_entry *hnext;  /* Next entry in hash bucket */
};

/* FNV-1a hash function */
static inline uint64_t cache_hash(cache_key_t key)
{
    uint64_t hash = 14695981039346656037ULL; /* FNV offset basis */
    hash ^= (key >> 56) & 0xFF;
    hash *= 1099511628211ULL;
    hash ^= (key >> 48) & 0xFF;
    hash *= 1099511628211ULL;
    hash ^= (key >> 40) & 0xFF;
    hash *= 1099511628211ULL;
    hash ^= (key >> 32) & 0xFF;
    hash *= 1099511628211ULL;
    hash ^= (key >> 24) & 0xFF;
    hash *= 1099511628211ULL;
    hash ^= (key >> 16) & 0xFF;
    hash *= 1099511628211ULL;
    hash ^= (key >> 8) & 0xFF;
    hash *= 1099511628211ULL;
    hash ^= key & 0xFF;
    hash *= 1099511628211ULL;
    return hash;
}

/* Create a new cache entry */
static struct cache_entry *cache_entry_new(cache_key_t key, void *value,
                                            size_t memory_size)
{
    struct cache_entry *entry = malloc(sizeof(*entry));
    if (!entry) {
        return NULL;
    }

    entry->key = key;
    entry->value = value;
    entry->memory_size = memory_size;
    entry->prev = NULL;
    entry->next = NULL;
    entry->hnext = NULL;

    return entry;
}

/* Free a cache entry */
static void cache_entry_free(struct cache_entry *entry, cache_free_fn free_fn)
{
    if (entry) {
        if (free_fn && entry->value) {
            free_fn(entry->value);
        }
        free(entry);
    }
}

/* Insert entry at head of LRU list (most recently used) */
static void lru_insert_head(sqfs_cache_t *cache, struct cache_entry *entry)
{
    entry->prev = NULL;
    entry->next = cache->head;

    if (cache->head) {
        cache->head->prev = entry;
    } else {
        /* Empty list, this becomes the tail too */
        cache->tail = entry;
    }
    cache->head = entry;
}

/* Remove entry from LRU list */
static void lru_remove(sqfs_cache_t *cache, struct cache_entry *entry)
{
    if (entry->prev) {
        entry->prev->next = entry->next;
    } else {
        cache->head = entry->next;
    }

    if (entry->next) {
        entry->next->prev = entry->prev;
    } else {
        cache->tail = entry->prev;
    }

    entry->prev = NULL;
    entry->next = NULL;
}

/* Move entry to head of LRU list */
static void lru_move_to_head(sqfs_cache_t *cache, struct cache_entry *entry)
{
    if (cache->head == entry) {
        return; /* Already at head */
    }

    lru_remove(cache, entry);
    lru_insert_head(cache, entry);
}

/* Find entry in hash table */
static struct cache_entry *hash_find(sqfs_cache_t *cache, cache_key_t key)
{
    uint64_t hash = cache_hash(key);
    size_t idx = hash & cache->bucket_mask;
    struct cache_entry *entry = cache->buckets[idx];

    while (entry) {
        if (entry->key == key) {
            return entry;
        }
        entry = entry->hnext;
    }

    return NULL;
}

/* Insert entry into hash table */
static void hash_insert(sqfs_cache_t *cache, struct cache_entry *entry)
{
    uint64_t hash = cache_hash(entry->key);
    size_t idx = hash & cache->bucket_mask;

    entry->hnext = cache->buckets[idx];
    cache->buckets[idx] = entry;
}

/* Remove entry from hash table */
static void hash_remove(sqfs_cache_t *cache, cache_key_t key)
{
    uint64_t hash = cache_hash(key);
    size_t idx = hash & cache->bucket_mask;
    struct cache_entry *entry = cache->buckets[idx];
    struct cache_entry *prev = NULL;

    while (entry) {
        if (entry->key == key) {
            if (prev) {
                prev->hnext = entry->hnext;
            } else {
                cache->buckets[idx] = entry->hnext;
            }
            entry->hnext = NULL;
            return;
        }
        prev = entry;
        entry = entry->hnext;
    }
}

/* Check if hash table needs resizing and resize if needed */
static int cache_resize_if_needed(sqfs_cache_t *cache)
{
    size_t load = cache->current_entries;

    /* Check if we need to resize based on load factor */
    if (load <= (cache->bucket_count * CACHE_LOAD_FACTOR_THRESHOLD)) {
        return 0; /* No resize needed */
    }

    /* Calculate new bucket count (double) */
    size_t new_count = cache->bucket_count * 2;
    if (new_count < cache->bucket_count) {
        /* Overflow protection */
        return 0;
    }

    /* Allocate new bucket array */
    struct cache_entry **new_buckets = calloc(new_count, sizeof(*new_buckets));
    if (!new_buckets) {
        return -1; /* Continue with old buckets */
    }

    /* Save old buckets */
    struct cache_entry **old_buckets = cache->buckets;

    /* Update cache with new buckets */
    cache->buckets = new_buckets;
    cache->bucket_count = new_count;
    cache->bucket_mask = new_count - 1;

    /* Rehash all entries */
    struct cache_entry *entry = cache->head;
    while (entry) {
        uint64_t hash = cache_hash(entry->key);
        size_t idx = hash & cache->bucket_mask;
        entry->hnext = cache->buckets[idx];
        cache->buckets[idx] = entry;
        entry = entry->next;
    }

    free(old_buckets);
    return 0;
}

/* Evict entries to satisfy memory and entry limits */
static void cache_evict(sqfs_cache_t *cache, size_t memory_needed)
{
    while (cache->tail) {
        /* Check if we have satisfied constraints */
        if (cache->current_entries < cache->max_entries &&
            cache->current_memory + memory_needed <= cache->max_memory) {
            break;
        }

        /* Evict the least recently used entry (tail) */
        struct cache_entry *victim = cache->tail;

        /* Remove from hash table */
        hash_remove(cache, victim->key);

        /* Remove from LRU list */
        lru_remove(cache, victim);

        /* Update statistics */
        cache->current_entries--;
        cache->current_memory -= victim->memory_size;

        /* Free the entry */
        cache_entry_free(victim, cache->free_fn);
    }
}

/*
 * Initialize a cache instance.
 */
int sqfs_cache_init(sqfs_cache_t *cache, size_t max_entries,
                    size_t max_memory, cache_free_fn free_fn)
{
    if (!cache || max_entries == 0) {
        errno = EINVAL;
        return -1;
    }

    memset(cache, 0, sizeof(*cache));

    /* Initialize read-write lock */
    if (pthread_rwlock_init(&cache->lock, NULL) != 0) {
        return -1;
    }

    /* Calculate initial bucket count */
    size_t bucket_count = CACHE_INITIAL_BUCKETS;
    while (bucket_count < max_entries && bucket_count < 65536) {
        bucket_count *= 2;
    }

    /* Allocate hash table */
    cache->buckets = calloc(bucket_count, sizeof(*cache->buckets));
    if (!cache->buckets) {
        pthread_rwlock_destroy(&cache->lock);
        return -1;
    }

    cache->max_entries = max_entries;
    cache->max_memory = max_memory;
    cache->free_fn = free_fn;
    cache->bucket_count = bucket_count;
    cache->bucket_mask = bucket_count - 1;
    cache->current_entries = 0;
    cache->current_memory = 0;
    cache->head = NULL;
    cache->tail = NULL;
    cache->hits = 0;
    cache->misses = 0;

    return 0;
}

/*
 * Destroy a cache instance and free all resources.
 */
void sqfs_cache_destroy(sqfs_cache_t *cache)
{
    if (!cache) {
        return;
    }

    /* Acquire write lock */
    pthread_rwlock_wrlock(&cache->lock);

    /* Free all entries */
    struct cache_entry *entry = cache->head;
    while (entry) {
        struct cache_entry *next = entry->next;
        cache_entry_free(entry, cache->free_fn);
        entry = next;
    }

    /* Free hash table */
    free(cache->buckets);
    cache->buckets = NULL;

    /* Release and destroy lock */
    pthread_rwlock_unlock(&cache->lock);
    pthread_rwlock_destroy(&cache->lock);

    memset(cache, 0, sizeof(*cache));
}

/*
 * Get a value from the cache.
 */
void *sqfs_cache_get(sqfs_cache_t *cache, cache_key_t key)
{
    if (!cache) {
        return NULL;
    }

    /* Acquire read lock first */
    pthread_rwlock_rdlock(&cache->lock);

    struct cache_entry *entry = hash_find(cache, key);
    void *value = NULL;

    if (entry) {
        /* Found - move to head and return value */
        value = entry->value;
        cache->hits++;

        /* Need write lock to modify LRU list */
        pthread_rwlock_unlock(&cache->lock);
        pthread_rwlock_wrlock(&cache->lock);

        /* Re-find in case it was modified */
        entry = hash_find(cache, key);
        if (entry) {
            lru_move_to_head(cache, entry);
        }
    } else {
        /* Not found */
        cache->misses++;
    }

    pthread_rwlock_unlock(&cache->lock);
    return value;
}

/*
 * Put a value into the cache.
 */
int sqfs_cache_put(sqfs_cache_t *cache, cache_key_t key, void *value,
                   size_t memory_size)
{
    if (!cache || !value) {
        errno = EINVAL;
        return -1;
    }

    /* Check if memory size exceeds limit */
    if (memory_size > cache->max_memory) {
        /* Value too large to cache */
        errno = E2BIG;
        return -1;
    }

    /* Acquire write lock */
    pthread_rwlock_wrlock(&cache->lock);

    /* Check if key already exists */
    struct cache_entry *existing = hash_find(cache, key);
    if (existing) {
        /* Update existing entry */
        cache->current_memory -= existing->memory_size;
        cache->current_memory += memory_size;

        /* Free old value */
        if (cache->free_fn && existing->value) {
            cache->free_fn(existing->value);
        }

        existing->value = value;
        existing->memory_size = memory_size;
        lru_move_to_head(cache, existing);

        pthread_rwlock_unlock(&cache->lock);
        return 0;
    }

    /* Evict entries if needed */
    cache_evict(cache, memory_size);

    /* Check if we can insert */
    if (cache->current_entries >= cache->max_entries) {
        /* Still can't fit - cache is at max entries with minimal memory */
        pthread_rwlock_unlock(&cache->lock);
        errno = ENOSPC;
        return -1;
    }

    /* Create new entry */
    struct cache_entry *entry = cache_entry_new(key, value, memory_size);
    if (!entry) {
        pthread_rwlock_unlock(&cache->lock);
        errno = ENOMEM;
        return -1;
    }

    /* Insert into hash table */
    hash_insert(cache, entry);

    /* Insert at head of LRU list */
    lru_insert_head(cache, entry);

    /* Update statistics */
    cache->current_entries++;
    cache->current_memory += memory_size;

    /* Resize hash table if needed */
    cache_resize_if_needed(cache);

    pthread_rwlock_unlock(&cache->lock);
    return 0;
}

/*
 * Remove an entry from the cache.
 */
void sqfs_cache_remove(sqfs_cache_t *cache, cache_key_t key)
{
    if (!cache) {
        return;
    }

    /* Acquire write lock */
    pthread_rwlock_wrlock(&cache->lock);

    /* Find the entry */
    struct cache_entry *entry = hash_find(cache, key);
    if (!entry) {
        pthread_rwlock_unlock(&cache->lock);
        return;
    }

    /* Remove from hash table */
    hash_remove(cache, key);

    /* Remove from LRU list */
    lru_remove(cache, entry);

    /* Update statistics */
    cache->current_entries--;
    cache->current_memory -= entry->memory_size;

    /* Free the entry */
    cache_entry_free(entry, cache->free_fn);

    pthread_rwlock_unlock(&cache->lock);
}

/*
 * Clear all entries from the cache.
 */
void sqfs_cache_clear(sqfs_cache_t *cache)
{
    if (!cache) {
        return;
    }

    /* Acquire write lock */
    pthread_rwlock_wrlock(&cache->lock);

    /* Free all entries */
    struct cache_entry *entry = cache->head;
    while (entry) {
        struct cache_entry *next = entry->next;
        cache_entry_free(entry, cache->free_fn);
        entry = next;
    }

    /* Clear hash table */
    memset(cache->buckets, 0, cache->bucket_count * sizeof(*cache->buckets));

    /* Reset state */
    cache->head = NULL;
    cache->tail = NULL;
    cache->current_entries = 0;
    cache->current_memory = 0;

    pthread_rwlock_unlock(&cache->lock);
}

/*
 * Get cache hit count.
 */
size_t sqfs_cache_hits(sqfs_cache_t *cache)
{
    if (!cache) {
        return 0;
    }

    pthread_rwlock_rdlock(&cache->lock);
    size_t hits = cache->hits;
    pthread_rwlock_unlock(&cache->lock);

    return hits;
}

/*
 * Get cache miss count.
 */
size_t sqfs_cache_misses(sqfs_cache_t *cache)
{
    if (!cache) {
        return 0;
    }

    pthread_rwlock_rdlock(&cache->lock);
    size_t misses = cache->misses;
    pthread_rwlock_unlock(&cache->lock);

    return misses;
}

/*
 * Get current number of entries in cache.
 */
size_t sqfs_cache_entries(sqfs_cache_t *cache)
{
    if (!cache) {
        return 0;
    }

    pthread_rwlock_rdlock(&cache->lock);
    size_t entries = cache->current_entries;
    pthread_rwlock_unlock(&cache->lock);

    return entries;
}

/*
 * Get current memory usage of cache.
 */
size_t sqfs_cache_memory(sqfs_cache_t *cache)
{
    if (!cache) {
        return 0;
    }

    pthread_rwlock_rdlock(&cache->lock);
    size_t memory = cache->current_memory;
    pthread_rwlock_unlock(&cache->lock);

    return memory;
}