/*
 * SquashFS-FUSE - Logging System Implementation
 *
 * JSON-formatted logging with file rotation and thread safety.
 *
 * Copyright (c) 2024 SquashFS-FUSE Authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define _POSIX_C_SOURCE 200809L

#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <time.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <errno.h>

/* ============================================================================
 * Module Names
 * ============================================================================ */

static const char *module_names[] = {
    [SQFS_MOD_SUPERBLOCK]  = "superblock",
    [SQFS_MOD_INODE]       = "inode",
    [SQFS_MOD_DIRECTORY]   = "directory",
    [SQFS_MOD_DATA]        = "data",
    [SQFS_MOD_FRAGMENT]    = "fragment",
    [SQFS_MOD_COMPRESSOR]  = "compressor",
    [SQFS_MOD_CACHE]       = "cache",
    [SQFS_MOD_XATTR]       = "xattr",
    [SQFS_MOD_FUSE]        = "fuse",
};

static const char *level_names[] = {
    [SQFS_LOG_ERROR] = "ERROR",
    [SQFS_LOG_WARN]  = "WARN",
    [SQFS_LOG_INFO]  = "INFO",
    [SQFS_LOG_DEBUG] = "DEBUG",
};

const char *sqfs_module_name(sqfs_module_t module) {
    if (module >= 0 && module < SQFS_MOD_COUNT) {
        return module_names[module];
    }
    return "unknown";
}

/* ============================================================================
 * Log State
 * ============================================================================ */

typedef struct {
    FILE             *file;           /* Log file handle */
    char             *path;           /* Log file path */
    size_t            max_size;       /* Max file size */
    size_t            current_size;   /* Current file size (approximate) */
    sqfs_log_level_t  level;          /* Runtime log level */
    pthread_mutex_t   mutex;          /* Thread safety */
    bool              initialized;    /* Initialization flag */
} sqfs_log_state_t;

static sqfs_log_state_t g_log = {
    .file        = NULL,
    .path        = NULL,
    .max_size    = SQFS_LOG_DEFAULT_MAX_SIZE,
    .current_size = 0,
    .level       = SQFS_LOG_LEVEL,
    .mutex       = PTHREAD_MUTEX_INITIALIZER,
    .initialized = false,
};

/* ============================================================================
 * JSON String Escaping
 * ============================================================================ */

/*
 * Escape a string for JSON output.
 * Returns a newly allocated string that must be freed.
 */
static char *json_escape_string(const char *str) {
    if (str == NULL) {
        return NULL;
    }

    size_t len = strlen(str);
    size_t escaped_len = 0;

    /* Calculate escaped length */
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)str[i];
        switch (c) {
            case '"':
            case '\\':
            case '\b':
            case '\f':
            case '\n':
            case '\r':
            case '\t':
                escaped_len += 2;
                break;
            default:
                if (c < 0x20) {
                    escaped_len += 6;  /* \uXXXX */
                } else {
                    escaped_len += 1;
                }
                break;
        }
    }

    /* Allocate and build escaped string */
    char *escaped = malloc(escaped_len + 1);
    if (escaped == NULL) {
        return NULL;
    }

    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)str[i];
        switch (c) {
            case '"':  escaped[j++] = '\\'; escaped[j++] = '"'; break;
            case '\\': escaped[j++] = '\\'; escaped[j++] = '\\'; break;
            case '\b': escaped[j++] = '\\'; escaped[j++] = 'b'; break;
            case '\f': escaped[j++] = '\\'; escaped[j++] = 'f'; break;
            case '\n': escaped[j++] = '\\'; escaped[j++] = 'n'; break;
            case '\r': escaped[j++] = '\\'; escaped[j++] = 'r'; break;
            case '\t': escaped[j++] = '\\'; escaped[j++] = 't'; break;
            default:
                if (c < 0x20) {
                    j += sprintf(escaped + j, "\\u%04x", c);
                } else {
                    escaped[j++] = c;
                }
                break;
        }
    }
    escaped[j] = '\0';

    return escaped;
}

/* ============================================================================
 * Time Formatting
 * ============================================================================ */

/*
 * Format current time as ISO 8601 with milliseconds.
 * Returns number of characters written (excluding null terminator).
 */
static int format_timestamp(char *buf, size_t size) {
    struct timespec ts;
    struct tm tm;

    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return snprintf(buf, size, "1970-01-01T00:00:00.000Z");
    }

    if (gmtime_r(&ts.tv_sec, &tm) == NULL) {
        return snprintf(buf, size, "1970-01-01T00:00:00.000Z");
    }

    return snprintf(buf, size, "%04d-%02d-%02dT%02d:%02d:%02d.%03ldZ",
                    tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                    tm.tm_hour, tm.tm_min, tm.tm_sec,
                    ts.tv_nsec / 1000000);
}

/* ============================================================================
 * File Rotation
 * ============================================================================ */

/*
 * Check if log file needs rotation and rotate if necessary.
 * Simple strategy: truncate and start over when max size exceeded.
 */
static void check_and_rotate(void) {
    if (g_log.file == NULL || g_log.max_size == 0) {
        return;
    }

    if (g_log.current_size >= g_log.max_size) {
        /* Truncate the file */
        fflush(g_log.file);
        if (ftruncate(fileno(g_log.file), 0) == 0) {
            rewind(g_log.file);
            g_log.current_size = 0;
        }
    }
}

/* ============================================================================
 * Public API Implementation
 * ============================================================================ */

int sqfs_log_init(const sqfs_log_config_t *config) {
    pthread_mutex_lock(&g_log.mutex);

    if (g_log.initialized) {
        pthread_mutex_unlock(&g_log.mutex);
        return 0;  /* Already initialized */
    }

    /* Set configuration */
    if (config != NULL) {
        if (config->path != NULL) {
            g_log.path = strdup(config->path);
            if (g_log.path == NULL) {
                pthread_mutex_unlock(&g_log.mutex);
                return -1;
            }
        }
        if (config->max_size > 0) {
            g_log.max_size = config->max_size;
        }
        /* Cap runtime level to compile-time level */
        if (config->level <= SQFS_LOG_LEVEL) {
            g_log.level = config->level;
        } else {
            g_log.level = SQFS_LOG_LEVEL;
        }
    }

    /* Use default path if none specified */
    if (g_log.path == NULL) {
        g_log.path = strdup(SQFS_LOG_DEFAULT_PATH);
        if (g_log.path == NULL) {
            pthread_mutex_unlock(&g_log.mutex);
            return -1;
        }
    }

    /* Open log file */
    g_log.file = fopen(g_log.path, "a");
    if (g_log.file == NULL) {
        free(g_log.path);
        g_log.path = NULL;
        pthread_mutex_unlock(&g_log.mutex);
        return -1;
    }

    /* Get current file size */
    struct stat st;
    if (fstat(fileno(g_log.file), &st) == 0) {
        g_log.current_size = (size_t)st.st_size;
    } else {
        g_log.current_size = 0;
    }

    /* Set line buffering for immediate output */
    setvbuf(g_log.file, NULL, _IOLBF, 0);

    g_log.initialized = true;
    pthread_mutex_unlock(&g_log.mutex);

    return 0;
}

void sqfs_log_destroy(void) {
    pthread_mutex_lock(&g_log.mutex);

    if (!g_log.initialized) {
        pthread_mutex_unlock(&g_log.mutex);
        return;
    }

    if (g_log.file != NULL) {
        fflush(g_log.file);
        fclose(g_log.file);
        g_log.file = NULL;
    }

    if (g_log.path != NULL) {
        free(g_log.path);
        g_log.path = NULL;
    }

    g_log.current_size = 0;
    g_log.initialized = false;

    pthread_mutex_unlock(&g_log.mutex);
}

void sqfs_log_flush(void) {
    pthread_mutex_lock(&g_log.mutex);
    if (g_log.file != NULL) {
        fflush(g_log.file);
    }
    pthread_mutex_unlock(&g_log.mutex);
}

void sqfs_log(sqfs_log_level_t level, sqfs_module_t module,
              const char *file, int line, const char *fmt, ...) {
    /* Check compile-time level */
    if (!SQFS_LOG_ENABLED(level)) {
        return;
    }

    pthread_mutex_lock(&g_log.mutex);

    /* Check runtime level */
    if (level > g_log.level) {
        pthread_mutex_unlock(&g_log.mutex);
        return;
    }

    /* Auto-initialize with defaults if needed */
    if (!g_log.initialized) {
        g_log.file = fopen(SQFS_LOG_DEFAULT_PATH, "a");
        if (g_log.file != NULL) {
            setvbuf(g_log.file, NULL, _IOLBF, 0);
            g_log.path = strdup(SQFS_LOG_DEFAULT_PATH);
            g_log.initialized = true;
        }
    }

    if (g_log.file == NULL) {
        pthread_mutex_unlock(&g_log.mutex);
        return;
    }

    /* Check rotation */
    check_and_rotate();

    /* Format timestamp */
    char timestamp[32];
    format_timestamp(timestamp, sizeof(timestamp));

    /* Format user message */
    char message[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    /* Escape strings for JSON */
    char *escaped_msg = json_escape_string(message);
    char *escaped_file = json_escape_string(file);

    /* Build JSON log entry */
    char log_line[8192];
    int len;

    if (level == SQFS_LOG_DEBUG && escaped_file != NULL) {
        /* Include file and line for DEBUG level */
        len = snprintf(log_line, sizeof(log_line),
            "{\"ts\":\"%s\",\"level\":\"%s\",\"module\":\"%s\",\"msg\":\"%s\","
            "\"file\":\"%s\",\"line\":%d}\n",
            timestamp,
            level_names[level],
            module_names[module],
            escaped_msg ? escaped_msg : "",
            escaped_file,
            line);
    } else {
        len = snprintf(log_line, sizeof(log_line),
            "{\"ts\":\"%s\",\"level\":\"%s\",\"module\":\"%s\",\"msg\":\"%s\"}\n",
            timestamp,
            level_names[level],
            module_names[module],
            escaped_msg ? escaped_msg : "");
    }

    /* Free escaped strings */
    free(escaped_msg);
    free(escaped_file);

    /* Write to file */
    if (len > 0 && (size_t)len < sizeof(log_line)) {
        size_t written = fwrite(log_line, 1, (size_t)len, g_log.file);
        g_log.current_size += written;
    }

    pthread_mutex_unlock(&g_log.mutex);
}

void sqfs_log_data(sqfs_log_level_t level, sqfs_module_t module,
                   const char *file, int line,
                   const char *msg, const char *json_data) {
    /* Check compile-time level */
    if (!SQFS_LOG_ENABLED(level)) {
        return;
    }

    pthread_mutex_lock(&g_log.mutex);

    /* Check runtime level */
    if (level > g_log.level) {
        pthread_mutex_unlock(&g_log.mutex);
        return;
    }

    /* Auto-initialize with defaults if needed */
    if (!g_log.initialized) {
        g_log.file = fopen(SQFS_LOG_DEFAULT_PATH, "a");
        if (g_log.file != NULL) {
            setvbuf(g_log.file, NULL, _IOLBF, 0);
            g_log.path = strdup(SQFS_LOG_DEFAULT_PATH);
            g_log.initialized = true;
        }
    }

    if (g_log.file == NULL) {
        pthread_mutex_unlock(&g_log.mutex);
        return;
    }

    /* Check rotation */
    check_and_rotate();

    /* Format timestamp */
    char timestamp[32];
    format_timestamp(timestamp, sizeof(timestamp));

    /* Escape message */
    char *escaped_msg = json_escape_string(msg);

    /* Build JSON log entry */
    char log_line[16384];
    int len;

    if (json_data != NULL && json_data[0] != '\0') {
        if (level == SQFS_LOG_DEBUG) {
            char *escaped_file = json_escape_string(file);
            len = snprintf(log_line, sizeof(log_line),
                "{\"ts\":\"%s\",\"level\":\"%s\",\"module\":\"%s\",\"msg\":\"%s\","
                "\"data\":%s,\"file\":\"%s\",\"line\":%d}\n",
                timestamp,
                level_names[level],
                module_names[module],
                escaped_msg ? escaped_msg : "",
                json_data,
                escaped_file ? escaped_file : "",
                line);
            free(escaped_file);
        } else {
            len = snprintf(log_line, sizeof(log_line),
                "{\"ts\":\"%s\",\"level\":\"%s\",\"module\":\"%s\",\"msg\":\"%s\","
                "\"data\":%s}\n",
                timestamp,
                level_names[level],
                module_names[module],
                escaped_msg ? escaped_msg : "",
                json_data);
        }
    } else {
        len = snprintf(log_line, sizeof(log_line),
            "{\"ts\":\"%s\",\"level\":\"%s\",\"module\":\"%s\",\"msg\":\"%s\"}\n",
            timestamp,
            level_names[level],
            module_names[module],
            escaped_msg ? escaped_msg : "");
    }

    free(escaped_msg);

    /* Write to file */
    if (len > 0 && (size_t)len < sizeof(log_line)) {
        size_t written = fwrite(log_line, 1, (size_t)len, g_log.file);
        g_log.current_size += written;
    }

    pthread_mutex_unlock(&g_log.mutex);
}