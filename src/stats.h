/*
 * SquashFS-FUSE - Statistics System Header
 *
 * Tracks performance metrics and operation counts for debugging and analysis.
 *
 * Copyright (c) 2024 SquashFS-FUSE Authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef SQFS_STATS_H
#define SQFS_STATS_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Cache Statistics
 * ============================================================================ */

typedef struct {
    uint64_t hits;            /* Cache hits */
    uint64_t misses;          /* Cache misses */
    uint64_t evictions;       /* Cache evictions */
    uint64_t current_entries; /* Current entry count */
    uint64_t current_memory;  /* Current memory usage (bytes) */
} sqfs_cache_stats_t;

/* ============================================================================
 * Compressor Statistics
 * ============================================================================ */

typedef struct {
    uint64_t decompress_count;    /* Decompression operations */
    uint64_t decompress_bytes_in;  /* Input bytes (compressed) */
    uint64_t decompress_bytes_out; /* Output bytes (uncompressed) */
    uint64_t decompress_errors;    /* Decompression errors */
    uint64_t total_time_us;        /* Total decompression time (microseconds) */
} sqfs_compressor_stats_t;

/* ============================================================================
 * Error Statistics
 * ============================================================================ */

typedef struct {
    uint64_t total;      /* Total errors */
    uint64_t io;         /* I/O errors */
    uint64_t corrupt;    /* Corruption errors */
    uint64_t no_memory;  /* Memory allocation failures */
} sqfs_error_stats_t;

/* ============================================================================
 * Global Statistics
 * ============================================================================ */

typedef struct {
    /* Timestamps */
    time_t start_time;    /* Start time (seconds since epoch) */
    time_t last_report;   /* Last report time */

    /* File operations */
    uint64_t open_count;      /* File open operations */
    uint64_t read_count;      /* Read operations */
    uint64_t read_bytes;      /* Total bytes read */
    uint64_t readdir_count;   /* Directory read operations */
    uint64_t getattr_count;   /* getattr operations */
    uint64_t lookup_count;    /* Path lookups */

    /* Cache statistics (per cache type) */
    sqfs_cache_stats_t cache_inode;
    sqfs_cache_stats_t cache_dir;
    sqfs_cache_stats_t cache_meta;
    sqfs_cache_stats_t cache_data;

    /* Compressor statistics */
    sqfs_compressor_stats_t compressor;

    /* Error statistics */
    sqfs_error_stats_t errors;
} sqfs_stats_t;

/* Global statistics instance */
extern sqfs_stats_t g_stats;

/* ============================================================================
 * Statistics API
 * ============================================================================ */

/*
 * Initialize statistics tracking.
 */
void sqfs_stats_init(void);

/*
 * Reset all statistics to zero.
 */
void sqfs_stats_reset(void);

/*
 * Output statistics report to log.
 * Generates a JSON-formatted stats report.
 */
void sqfs_stats_report(void);

/*
 * Get current statistics (copy).
 *
 * @param out  Output buffer for statistics
 */
void sqfs_stats_get(sqfs_stats_t *out);

/* ============================================================================
 * Statistics Update Macros
 * ============================================================================ */

/* Increment operation counters */
#define SQFS_STATS_INC_OPEN()       (g_stats.open_count++)
#define SQFS_STATS_INC_READ(n)      do { g_stats.read_count++; g_stats.read_bytes += (n); } while(0)
#define SQFS_STATS_INC_READDIR()    (g_stats.readdir_count++)
#define SQFS_STATS_INC_GETATTR()    (g_stats.getattr_count++)
#define SQFS_STATS_INC_LOOKUP()     (g_stats.lookup_count++)

/* Cache statistics */
#define SQFS_STATS_CACHE_HIT(cache)       (g_stats.cache_##cache.hits++)
#define SQFS_STATS_CACHE_MISS(cache)      (g_stats.cache_##cache.misses++)
#define SQFS_STATS_CACHE_EVICT(cache)     (g_stats.cache_##cache.evictions++)

/* Compressor statistics */
#define SQFS_STATS_COMP_DECOMPRESS(in_sz, out_sz, time_us) \
    do { \
        g_stats.compressor.decompress_count++; \
        g_stats.compressor.decompress_bytes_in += (in_sz); \
        g_stats.compressor.decompress_bytes_out += (out_sz); \
        g_stats.compressor.total_time_us += (time_us); \
    } while(0)

#define SQFS_STATS_COMP_ERROR() (g_stats.compressor.decompress_errors++)

/* Error statistics */
#define SQFS_STATS_ERROR_IO()      do { g_stats.errors.io++; g_stats.errors.total++; } while(0)
#define SQFS_STATS_ERROR_CORRUPT() do { g_stats.errors.corrupt++; g_stats.errors.total++; } while(0)
#define SQFS_STATS_ERROR_MEMORY()  do { g_stats.errors.no_memory++; g_stats.errors.total++; } while(0)

/* ============================================================================
 * Cache Stats Update Functions
 * ============================================================================ */

/*
 * Update cache entry/memory counts.
 * Call these when adding or removing cache entries.
 */
void sqfs_stats_cache_update_inode(uint64_t entries, uint64_t memory);
void sqfs_stats_cache_update_dir(uint64_t entries, uint64_t memory);
void sqfs_stats_cache_update_meta(uint64_t entries, uint64_t memory);
void sqfs_stats_cache_update_data(uint64_t entries, uint64_t memory);

/* ============================================================================
 * Reporting Interval
 * ============================================================================ */

/* Default report interval: 60 seconds */
#define SQFS_STATS_REPORT_INTERVAL 60

/*
 * Check if it's time for a stats report.
 * Call this periodically from the main loop.
 *
 * @return true if a report should be generated
 */
bool sqfs_stats_should_report(void);

#ifdef __cplusplus
}
#endif

#endif /* SQFS_STATS_H */