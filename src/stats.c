/*
 * SquashFS-FUSE - Statistics System Implementation
 *
 * Tracks performance metrics and outputs periodic reports.
 *
 * Copyright (c) 2024 SquashFS-FUSE Authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define _POSIX_C_SOURCE 200809L

#include "stats.h"
#include "log.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* ============================================================================
 * Global Statistics Instance
 * ============================================================================ */

sqfs_stats_t g_stats;

/* ============================================================================
 * Initialization
 * ============================================================================ */

void sqfs_stats_init(void) {
    memset(&g_stats, 0, sizeof(g_stats));
    g_stats.start_time = time(NULL);
    g_stats.last_report = g_stats.start_time;
}

void sqfs_stats_reset(void) {
    time_t start = g_stats.start_time;
    memset(&g_stats, 0, sizeof(g_stats));
    g_stats.start_time = start;
    g_stats.last_report = time(NULL);
}

/* ============================================================================
 * Statistics Retrieval
 * ============================================================================ */

void sqfs_stats_get(sqfs_stats_t *out) {
    if (out != NULL) {
        memcpy(out, &g_stats, sizeof(sqfs_stats_t));
    }
}

/* ============================================================================
 * Cache Stats Update Functions
 * ============================================================================ */

void sqfs_stats_cache_update_inode(uint64_t entries, uint64_t memory) {
    g_stats.cache_inode.current_entries = entries;
    g_stats.cache_inode.current_memory = memory;
}

void sqfs_stats_cache_update_dir(uint64_t entries, uint64_t memory) {
    g_stats.cache_dir.current_entries = entries;
    g_stats.cache_dir.current_memory = memory;
}

void sqfs_stats_cache_update_meta(uint64_t entries, uint64_t memory) {
    g_stats.cache_meta.current_entries = entries;
    g_stats.cache_meta.current_memory = memory;
}

void sqfs_stats_cache_update_data(uint64_t entries, uint64_t memory) {
    g_stats.cache_data.current_entries = entries;
    g_stats.cache_data.current_memory = memory;
}

/* ============================================================================
 * Report Timing
 * ============================================================================ */

bool sqfs_stats_should_report(void) {
    time_t now = time(NULL);
    if (now - g_stats.last_report >= SQFS_STATS_REPORT_INTERVAL) {
        g_stats.last_report = now;
        return true;
    }
    return false;
}

/* ============================================================================
 * Statistics Report Generation
 * ============================================================================ */

void sqfs_stats_report(void) {
    time_t now = time(NULL);
    time_t uptime = now - g_stats.start_time;

    /* Build JSON stats report */
    char report[4096];
    int len;

    /* Calculate cache hit rates */
    uint64_t inode_total = g_stats.cache_inode.hits + g_stats.cache_inode.misses;
    uint64_t dir_total = g_stats.cache_dir.hits + g_stats.cache_dir.misses;
    uint64_t meta_total = g_stats.cache_meta.hits + g_stats.cache_meta.misses;
    uint64_t data_total = g_stats.cache_data.hits + g_stats.cache_data.misses;

    /* Calculate average decompression time */
    uint64_t avg_decomp_time = 0;
    if (g_stats.compressor.decompress_count > 0) {
        avg_decomp_time = g_stats.compressor.total_time_us /
                          g_stats.compressor.decompress_count;
    }

    len = snprintf(report, sizeof(report),
        "{"
        "\"uptime_sec\":%ld,"
        "\"operations\":{"
            "\"open\":%lu,"
            "\"read\":%lu,"
            "\"read_bytes\":%lu,"
            "\"readdir\":%lu,"
            "\"getattr\":%lu,"
            "\"lookup\":%lu"
        "},"
        "\"cache\":{"
            "\"inode\":{\"hits\":%lu,\"misses\":%lu,\"evictions\":%lu,"
                      "\"entries\":%lu,\"memory\":%lu},"
            "\"dir\":{\"hits\":%lu,\"misses\":%lu,\"evictions\":%lu,"
                    "\"entries\":%lu,\"memory\":%lu},"
            "\"meta\":{\"hits\":%lu,\"misses\":%lu,\"evictions\":%lu,"
                     "\"entries\":%lu,\"memory\":%lu},"
            "\"data\":{\"hits\":%lu,\"misses\":%lu,\"evictions\":%lu,"
                     "\"entries\":%lu,\"memory\":%lu}"
        "},"
        "\"compressor\":{"
            "\"calls\":%lu,"
            "\"bytes_in\":%lu,"
            "\"bytes_out\":%lu,"
            "\"errors\":%lu,"
            "\"avg_time_us\":%lu"
        "},"
        "\"errors\":{"
            "\"total\":%lu,"
            "\"io\":%lu,"
            "\"corrupt\":%lu,"
            "\"no_memory\":%lu"
        "}"
        "}",
        (long)uptime,
        (unsigned long)g_stats.open_count,
        (unsigned long)g_stats.read_count,
        (unsigned long)g_stats.read_bytes,
        (unsigned long)g_stats.readdir_count,
        (unsigned long)g_stats.getattr_count,
        (unsigned long)g_stats.lookup_count,
        (unsigned long)g_stats.cache_inode.hits,
        (unsigned long)g_stats.cache_inode.misses,
        (unsigned long)g_stats.cache_inode.evictions,
        (unsigned long)g_stats.cache_inode.current_entries,
        (unsigned long)g_stats.cache_inode.current_memory,
        (unsigned long)g_stats.cache_dir.hits,
        (unsigned long)g_stats.cache_dir.misses,
        (unsigned long)g_stats.cache_dir.evictions,
        (unsigned long)g_stats.cache_dir.current_entries,
        (unsigned long)g_stats.cache_dir.current_memory,
        (unsigned long)g_stats.cache_meta.hits,
        (unsigned long)g_stats.cache_meta.misses,
        (unsigned long)g_stats.cache_meta.evictions,
        (unsigned long)g_stats.cache_meta.current_entries,
        (unsigned long)g_stats.cache_meta.current_memory,
        (unsigned long)g_stats.cache_data.hits,
        (unsigned long)g_stats.cache_data.misses,
        (unsigned long)g_stats.cache_data.evictions,
        (unsigned long)g_stats.cache_data.current_entries,
        (unsigned long)g_stats.cache_data.current_memory,
        (unsigned long)g_stats.compressor.decompress_count,
        (unsigned long)g_stats.compressor.decompress_bytes_in,
        (unsigned long)g_stats.compressor.decompress_bytes_out,
        (unsigned long)g_stats.compressor.decompress_errors,
        (unsigned long)avg_decomp_time,
        (unsigned long)g_stats.errors.total,
        (unsigned long)g_stats.errors.io,
        (unsigned long)g_stats.errors.corrupt,
        (unsigned long)g_stats.errors.no_memory
    );

    if (len > 0 && (size_t)len < sizeof(report)) {
        /* Log as INFO with stats type */
        SQFS_LOG_INFO_DATA(SQFS_MOD_FUSE, "statistics report", report);
    }

    /* Also log summary to console if debug is enabled */
    SQFS_LOG_DEBUG(SQFS_MOD_FUSE,
        "Stats: uptime=%lds, opens=%lu, reads=%lu (%lu bytes), "
        "cache hits: inode=%lu/%lu dir=%lu/%lu meta=%lu/%lu data=%lu/%lu",
        (long)uptime,
        (unsigned long)g_stats.open_count,
        (unsigned long)g_stats.read_count,
        (unsigned long)g_stats.read_bytes,
        (unsigned long)g_stats.cache_inode.hits,
        (unsigned long)(inode_total > 0 ? inode_total : 1),
        (unsigned long)g_stats.cache_dir.hits,
        (unsigned long)(dir_total > 0 ? dir_total : 1),
        (unsigned long)g_stats.cache_meta.hits,
        (unsigned long)(meta_total > 0 ? meta_total : 1),
        (unsigned long)g_stats.cache_data.hits,
        (unsigned long)(data_total > 0 ? data_total : 1)
    );
}