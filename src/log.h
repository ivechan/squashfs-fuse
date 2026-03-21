/*
 * SquashFS-FUSE - Logging System Header
 *
 * Provides structured JSON logging with compile-time level filtering.
 *
 * Copyright (c) 2024 SquashFS-FUSE Authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef SQFS_LOG_H
#define SQFS_LOG_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Log Levels
 * ============================================================================ */

typedef enum {
    SQFS_LOG_ERROR = 1,   /* Errors: serious problems affecting functionality */
    SQFS_LOG_WARN  = 2,   /* Warnings: potential issues or anomalies */
    SQFS_LOG_INFO  = 3,   /* Info: key operations and state changes */
    SQFS_LOG_DEBUG = 4,   /* Debug: detailed operation information */
} sqfs_log_level_t;

/* Compile-time log level (default: WARN) */
#ifndef SQFS_LOG_LEVEL
#define SQFS_LOG_LEVEL SQFS_LOG_WARN
#endif

/* Check if a log level is enabled at compile time */
#define SQFS_LOG_ENABLED(level) ((level) <= SQFS_LOG_LEVEL)

/* ============================================================================
 * Modules
 * ============================================================================ */

typedef enum {
    SQFS_MOD_SUPERBLOCK  = 0,   /* Superblock parsing */
    SQFS_MOD_INODE       = 1,   /* Inode management */
    SQFS_MOD_DIRECTORY   = 2,   /* Directory table parsing */
    SQFS_MOD_DATA        = 3,   /* Data block reading */
    SQFS_MOD_FRAGMENT    = 4,   /* Fragment handling */
    SQFS_MOD_COMPRESSOR  = 5,   /* Compression/decompression */
    SQFS_MOD_CACHE       = 6,   /* Cache system */
    SQFS_MOD_XATTR       = 7,   /* Extended attributes */
    SQFS_MOD_FUSE        = 8,   /* FUSE operations */
    SQFS_MOD_COUNT       = 9,   /* Total module count */
} sqfs_module_t;

/* Get module name string */
const char *sqfs_module_name(sqfs_module_t module);

/* ============================================================================
 * Log Configuration
 * ============================================================================ */

typedef struct {
    const char       *path;         /* Log file path (NULL = default) */
    size_t            max_size;     /* Max file size in bytes (0 = default 10MB) */
    sqfs_log_level_t  level;        /* Runtime log level (capped by SQFS_LOG_LEVEL) */
} sqfs_log_config_t;

/* Default log path */
#define SQFS_LOG_DEFAULT_PATH "/tmp/squashfs-fuse.log"

/* Default max file size: 10 MB */
#define SQFS_LOG_DEFAULT_MAX_SIZE (10 * 1024 * 1024)

/* ============================================================================
 * Log System API
 * ============================================================================ */

/*
 * Initialize the logging system.
 * Must be called before any logging operations.
 *
 * @param config  Configuration options (NULL for defaults)
 * @return 0 on success, negative error code on failure
 */
int sqfs_log_init(const sqfs_log_config_t *config);

/*
 * Destroy the logging system.
 * Flushes any buffered data and closes the log file.
 */
void sqfs_log_destroy(void);

/*
 * Flush buffered log data to disk.
 */
void sqfs_log_flush(void);

/*
 * Core logging function (use macros instead).
 *
 * @param level    Log level
 * @param module   Source module
 * @param file     Source file name
 * @param line     Source line number
 * @param fmt      Printf-style format string
 * @param ...      Format arguments
 */
void sqfs_log(sqfs_log_level_t level, sqfs_module_t module,
              const char *file, int line, const char *fmt, ...);

/*
 * Logging with additional JSON data.
 *
 * @param level      Log level
 * @param module     Source module
 * @param file       Source file name
 * @param line       Source line number
 * @param msg        Log message
 * @param json_data  Additional JSON data string (must be valid JSON object or null)
 */
void sqfs_log_data(sqfs_log_level_t level, sqfs_module_t module,
                   const char *file, int line,
                   const char *msg, const char *json_data);

/* ============================================================================
 * Convenience Macros
 * ============================================================================ */

#define _SQFS_LOG_IMPL(level, module, fmt, ...) \
    sqfs_log(level, module, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define _SQFS_LOG_DATA_IMPL(level, module, msg, json_data) \
    sqfs_log_data(level, module, __FILE__, __LINE__, msg, json_data)

/* ERROR level logging */
#define SQFS_LOG_ERROR(mod, fmt, ...) \
    do { if (SQFS_LOG_ENABLED(SQFS_LOG_ERROR)) \
        _SQFS_LOG_IMPL(SQFS_LOG_ERROR, mod, fmt, ##__VA_ARGS__); } while(0)

#define SQFS_LOG_ERROR_DATA(mod, msg, json_data) \
    do { if (SQFS_LOG_ENABLED(SQFS_LOG_ERROR)) \
        _SQFS_LOG_DATA_IMPL(SQFS_LOG_ERROR, mod, msg, json_data); } while(0)

/* WARN level logging */
#define SQFS_LOG_WARN(mod, fmt, ...) \
    do { if (SQFS_LOG_ENABLED(SQFS_LOG_WARN)) \
        _SQFS_LOG_IMPL(SQFS_LOG_WARN, mod, fmt, ##__VA_ARGS__); } while(0)

#define SQFS_LOG_WARN_DATA(mod, msg, json_data) \
    do { if (SQFS_LOG_ENABLED(SQFS_LOG_WARN)) \
        _SQFS_LOG_DATA_IMPL(SQFS_LOG_WARN, mod, msg, json_data); } while(0)

/* INFO level logging */
#define SQFS_LOG_INFO(mod, fmt, ...) \
    do { if (SQFS_LOG_ENABLED(SQFS_LOG_INFO)) \
        _SQFS_LOG_IMPL(SQFS_LOG_INFO, mod, fmt, ##__VA_ARGS__); } while(0)

#define SQFS_LOG_INFO_DATA(mod, msg, json_data) \
    do { if (SQFS_LOG_ENABLED(SQFS_LOG_INFO)) \
        _SQFS_LOG_DATA_IMPL(SQFS_LOG_INFO, mod, msg, json_data); } while(0)

/* DEBUG level logging */
#define SQFS_LOG_DEBUG(mod, fmt, ...) \
    do { if (SQFS_LOG_ENABLED(SQFS_LOG_DEBUG)) \
        _SQFS_LOG_IMPL(SQFS_LOG_DEBUG, mod, fmt, ##__VA_ARGS__); } while(0)

#define SQFS_LOG_DEBUG_DATA(mod, msg, json_data) \
    do { if (SQFS_LOG_ENABLED(SQFS_LOG_DEBUG)) \
        _SQFS_LOG_DATA_IMPL(SQFS_LOG_DEBUG, mod, msg, json_data); } while(0)

/* ============================================================================
 * Module-Specific Macros (for convenience)
 * ============================================================================ */

/* Superblock logging */
#define SQFS_LOG_SB_ERROR(fmt, ...) SQFS_LOG_ERROR(SQFS_MOD_SUPERBLOCK, fmt, ##__VA_ARGS__)
#define SQFS_LOG_SB_WARN(fmt, ...)  SQFS_LOG_WARN(SQFS_MOD_SUPERBLOCK, fmt, ##__VA_ARGS__)
#define SQFS_LOG_SB_INFO(fmt, ...)  SQFS_LOG_INFO(SQFS_MOD_SUPERBLOCK, fmt, ##__VA_ARGS__)
#define SQFS_LOG_SB_DEBUG(fmt, ...) SQFS_LOG_DEBUG(SQFS_MOD_SUPERBLOCK, fmt, ##__VA_ARGS__)
#define SQFS_LOG_SB_ERROR_DATA(msg, json_data) SQFS_LOG_ERROR_DATA(SQFS_MOD_SUPERBLOCK, msg, json_data)
#define SQFS_LOG_SB_WARN_DATA(msg, json_data)  SQFS_LOG_WARN_DATA(SQFS_MOD_SUPERBLOCK, msg, json_data)
#define SQFS_LOG_SB_INFO_DATA(msg, json_data)  SQFS_LOG_INFO_DATA(SQFS_MOD_SUPERBLOCK, msg, json_data)
#define SQFS_LOG_SB_DEBUG_DATA(msg, json_data) SQFS_LOG_DEBUG_DATA(SQFS_MOD_SUPERBLOCK, msg, json_data)

/* Inode logging */
#define SQFS_LOG_INODE_ERROR(fmt, ...) SQFS_LOG_ERROR(SQFS_MOD_INODE, fmt, ##__VA_ARGS__)
#define SQFS_LOG_INODE_WARN(fmt, ...)  SQFS_LOG_WARN(SQFS_MOD_INODE, fmt, ##__VA_ARGS__)
#define SQFS_LOG_INODE_INFO(fmt, ...)  SQFS_LOG_INFO(SQFS_MOD_INODE, fmt, ##__VA_ARGS__)
#define SQFS_LOG_INODE_DEBUG(fmt, ...) SQFS_LOG_DEBUG(SQFS_MOD_INODE, fmt, ##__VA_ARGS__)
#define SQFS_LOG_INODE_ERROR_DATA(msg, json_data) SQFS_LOG_ERROR_DATA(SQFS_MOD_INODE, msg, json_data)
#define SQFS_LOG_INODE_WARN_DATA(msg, json_data)  SQFS_LOG_WARN_DATA(SQFS_MOD_INODE, msg, json_data)
#define SQFS_LOG_INODE_INFO_DATA(msg, json_data)  SQFS_LOG_INFO_DATA(SQFS_MOD_INODE, msg, json_data)
#define SQFS_LOG_INODE_DEBUG_DATA(msg, json_data) SQFS_LOG_DEBUG_DATA(SQFS_MOD_INODE, msg, json_data)

/* Directory logging */
#define SQFS_LOG_DIR_ERROR(fmt, ...) SQFS_LOG_ERROR(SQFS_MOD_DIRECTORY, fmt, ##__VA_ARGS__)
#define SQFS_LOG_DIR_WARN(fmt, ...)  SQFS_LOG_WARN(SQFS_MOD_DIRECTORY, fmt, ##__VA_ARGS__)
#define SQFS_LOG_DIR_INFO(fmt, ...)  SQFS_LOG_INFO(SQFS_MOD_DIRECTORY, fmt, ##__VA_ARGS__)
#define SQFS_LOG_DIR_DEBUG(fmt, ...) SQFS_LOG_DEBUG(SQFS_MOD_DIRECTORY, fmt, ##__VA_ARGS__)
#define SQFS_LOG_DIR_ERROR_DATA(msg, json_data) SQFS_LOG_ERROR_DATA(SQFS_MOD_DIRECTORY, msg, json_data)
#define SQFS_LOG_DIR_WARN_DATA(msg, json_data)  SQFS_LOG_WARN_DATA(SQFS_MOD_DIRECTORY, msg, json_data)
#define SQFS_LOG_DIR_INFO_DATA(msg, json_data)  SQFS_LOG_INFO_DATA(SQFS_MOD_DIRECTORY, msg, json_data)
#define SQFS_LOG_DIR_DEBUG_DATA(msg, json_data) SQFS_LOG_DEBUG_DATA(SQFS_MOD_DIRECTORY, msg, json_data)

/* Data logging */
#define SQFS_LOG_DATA_ERROR(fmt, ...) SQFS_LOG_ERROR(SQFS_MOD_DATA, fmt, ##__VA_ARGS__)
#define SQFS_LOG_DATA_WARN(fmt, ...)  SQFS_LOG_WARN(SQFS_MOD_DATA, fmt, ##__VA_ARGS__)
#define SQFS_LOG_DATA_INFO(fmt, ...)  SQFS_LOG_INFO(SQFS_MOD_DATA, fmt, ##__VA_ARGS__)
#define SQFS_LOG_DATA_DEBUG(fmt, ...) SQFS_LOG_DEBUG(SQFS_MOD_DATA, fmt, ##__VA_ARGS__)
#define SQFS_LOG_DATA_ERROR_DATA(msg, json_data) SQFS_LOG_ERROR_DATA(SQFS_MOD_DATA, msg, json_data)
#define SQFS_LOG_DATA_WARN_DATA(msg, json_data)  SQFS_LOG_WARN_DATA(SQFS_MOD_DATA, msg, json_data)
#define SQFS_LOG_DATA_INFO_DATA(msg, json_data)  SQFS_LOG_INFO_DATA(SQFS_MOD_DATA, msg, json_data)
#define SQFS_LOG_DATA_DEBUG_DATA(msg, json_data) SQFS_LOG_DEBUG_DATA(SQFS_MOD_DATA, msg, json_data)

/* Fragment logging */
#define SQFS_LOG_FRAG_ERROR(fmt, ...) SQFS_LOG_ERROR(SQFS_MOD_FRAGMENT, fmt, ##__VA_ARGS__)
#define SQFS_LOG_FRAG_WARN(fmt, ...)  SQFS_LOG_WARN(SQFS_MOD_FRAGMENT, fmt, ##__VA_ARGS__)
#define SQFS_LOG_FRAG_INFO(fmt, ...)  SQFS_LOG_INFO(SQFS_MOD_FRAGMENT, fmt, ##__VA_ARGS__)
#define SQFS_LOG_FRAG_DEBUG(fmt, ...) SQFS_LOG_DEBUG(SQFS_MOD_FRAGMENT, fmt, ##__VA_ARGS__)
#define SQFS_LOG_FRAG_ERROR_DATA(msg, json_data) SQFS_LOG_ERROR_DATA(SQFS_MOD_FRAGMENT, msg, json_data)
#define SQFS_LOG_FRAG_WARN_DATA(msg, json_data)  SQFS_LOG_WARN_DATA(SQFS_MOD_FRAGMENT, msg, json_data)
#define SQFS_LOG_FRAG_INFO_DATA(msg, json_data)  SQFS_LOG_INFO_DATA(SQFS_MOD_FRAGMENT, msg, json_data)
#define SQFS_LOG_FRAG_DEBUG_DATA(msg, json_data) SQFS_LOG_DEBUG_DATA(SQFS_MOD_FRAGMENT, msg, json_data)

/* Compressor logging */
#define SQFS_LOG_COMP_ERROR(fmt, ...) SQFS_LOG_ERROR(SQFS_MOD_COMPRESSOR, fmt, ##__VA_ARGS__)
#define SQFS_LOG_COMP_WARN(fmt, ...)  SQFS_LOG_WARN(SQFS_MOD_COMPRESSOR, fmt, ##__VA_ARGS__)
#define SQFS_LOG_COMP_INFO(fmt, ...)  SQFS_LOG_INFO(SQFS_MOD_COMPRESSOR, fmt, ##__VA_ARGS__)
#define SQFS_LOG_COMP_DEBUG(fmt, ...) SQFS_LOG_DEBUG(SQFS_MOD_COMPRESSOR, fmt, ##__VA_ARGS__)
#define SQFS_LOG_COMP_ERROR_DATA(msg, json_data) SQFS_LOG_ERROR_DATA(SQFS_MOD_COMPRESSOR, msg, json_data)
#define SQFS_LOG_COMP_WARN_DATA(msg, json_data)  SQFS_LOG_WARN_DATA(SQFS_MOD_COMPRESSOR, msg, json_data)
#define SQFS_LOG_COMP_INFO_DATA(msg, json_data)  SQFS_LOG_INFO_DATA(SQFS_MOD_COMPRESSOR, msg, json_data)
#define SQFS_LOG_COMP_DEBUG_DATA(msg, json_data) SQFS_LOG_DEBUG_DATA(SQFS_MOD_COMPRESSOR, msg, json_data)

/* Cache logging */
#define SQFS_LOG_CACHE_ERROR(fmt, ...) SQFS_LOG_ERROR(SQFS_MOD_CACHE, fmt, ##__VA_ARGS__)
#define SQFS_LOG_CACHE_WARN(fmt, ...)  SQFS_LOG_WARN(SQFS_MOD_CACHE, fmt, ##__VA_ARGS__)
#define SQFS_LOG_CACHE_INFO(fmt, ...)  SQFS_LOG_INFO(SQFS_MOD_CACHE, fmt, ##__VA_ARGS__)
#define SQFS_LOG_CACHE_DEBUG(fmt, ...) SQFS_LOG_DEBUG(SQFS_MOD_CACHE, fmt, ##__VA_ARGS__)
#define SQFS_LOG_CACHE_ERROR_DATA(msg, json_data) SQFS_LOG_ERROR_DATA(SQFS_MOD_CACHE, msg, json_data)
#define SQFS_LOG_CACHE_WARN_DATA(msg, json_data)  SQFS_LOG_WARN_DATA(SQFS_MOD_CACHE, msg, json_data)
#define SQFS_LOG_CACHE_INFO_DATA(msg, json_data)  SQFS_LOG_INFO_DATA(SQFS_MOD_CACHE, msg, json_data)
#define SQFS_LOG_CACHE_DEBUG_DATA(msg, json_data) SQFS_LOG_DEBUG_DATA(SQFS_MOD_CACHE, msg, json_data)

/* Xattr logging */
#define SQFS_LOG_XATTR_ERROR(fmt, ...) SQFS_LOG_ERROR(SQFS_MOD_XATTR, fmt, ##__VA_ARGS__)
#define SQFS_LOG_XATTR_WARN(fmt, ...)  SQFS_LOG_WARN(SQFS_MOD_XATTR, fmt, ##__VA_ARGS__)
#define SQFS_LOG_XATTR_INFO(fmt, ...)  SQFS_LOG_INFO(SQFS_MOD_XATTR, fmt, ##__VA_ARGS__)
#define SQFS_LOG_XATTR_DEBUG(fmt, ...) SQFS_LOG_DEBUG(SQFS_MOD_XATTR, fmt, ##__VA_ARGS__)
#define SQFS_LOG_XATTR_ERROR_DATA(msg, json_data) SQFS_LOG_ERROR_DATA(SQFS_MOD_XATTR, msg, json_data)
#define SQFS_LOG_XATTR_WARN_DATA(msg, json_data)  SQFS_LOG_WARN_DATA(SQFS_MOD_XATTR, msg, json_data)
#define SQFS_LOG_XATTR_INFO_DATA(msg, json_data)  SQFS_LOG_INFO_DATA(SQFS_MOD_XATTR, msg, json_data)
#define SQFS_LOG_XATTR_DEBUG_DATA(msg, json_data) SQFS_LOG_DEBUG_DATA(SQFS_MOD_XATTR, msg, json_data)

/* FUSE logging */
#define SQFS_LOG_FUSE_ERROR(fmt, ...) SQFS_LOG_ERROR(SQFS_MOD_FUSE, fmt, ##__VA_ARGS__)
#define SQFS_LOG_FUSE_WARN(fmt, ...)  SQFS_LOG_WARN(SQFS_MOD_FUSE, fmt, ##__VA_ARGS__)
#define SQFS_LOG_FUSE_INFO(fmt, ...)  SQFS_LOG_INFO(SQFS_MOD_FUSE, fmt, ##__VA_ARGS__)
#define SQFS_LOG_FUSE_DEBUG(fmt, ...) SQFS_LOG_DEBUG(SQFS_MOD_FUSE, fmt, ##__VA_ARGS__)
#define SQFS_LOG_FUSE_ERROR_DATA(msg, json_data) SQFS_LOG_ERROR_DATA(SQFS_MOD_FUSE, msg, json_data)
#define SQFS_LOG_FUSE_WARN_DATA(msg, json_data)  SQFS_LOG_WARN_DATA(SQFS_MOD_FUSE, msg, json_data)
#define SQFS_LOG_FUSE_INFO_DATA(msg, json_data)  SQFS_LOG_INFO_DATA(SQFS_MOD_FUSE, msg, json_data)
#define SQFS_LOG_FUSE_DEBUG_DATA(msg, json_data) SQFS_LOG_DEBUG_DATA(SQFS_MOD_FUSE, msg, json_data)

#ifdef __cplusplus
}
#endif

#endif /* SQFS_LOG_H */