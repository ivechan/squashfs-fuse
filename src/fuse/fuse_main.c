/*
 * SquashFS - FUSE Main Entry Point
 *
 * FUSE 3.x main program entry point.
 *
 * Copyright (c) 2024 SquashFS Authors
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#define _POSIX_C_SOURCE 200809L
#define FUSE_USE_VERSION 31

#include <fuse3/fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/vfs.h>
#include <unistd.h>
#include <fcntl.h>

#include "context.h"
#include "superblock.h"
#include "compressor.h"
#include "cache.h"
#include "log.h"
#include "stats.h"
#include "fragment.h"
#include "utils.h"
#include "inode.h"
#include "data.h"

/* FUSE operations (defined in vfs_fuse.c) */
extern struct fuse_operations sqfs_fuse_operations;

/* ============================================================================
 * Version and Build Information
 * ============================================================================ */

#define SQFS_FUSE_VERSION_MAJOR 1
#define SQFS_FUSE_VERSION_MINOR 0
#define SQFS_FUSE_VERSION_STRING "1.0.0"

/* ============================================================================
 * Cache Configuration Defaults
 * ============================================================================ */

#define DEFAULT_CACHE_SIZE_MB    32

/* ============================================================================
 * Command Line Options
 * ============================================================================ */

static const char *usage_string =
    "Usage: squashfs-fuse [OPTIONS] <image_file> <mount_point>\n"
    "\n"
    "Options:\n"
    "  -h, --help          Show this help message\n"
    "  -V, --version       Show version information\n"
    "  -d, --debug LEVEL   Set debug level (0-3)\n"
    "  -o, --options OPTS  FUSE mount options\n"
    "  -f, --foreground    Run in foreground\n"
    "  -s, --single        Single-threaded mode\n"
    "  --nocache           Disable all caching\n"
    "  --cache-size SIZE   Set cache size in MiB (default: %d)\n"
    "  -l, --log PATH      Log file path (default: /tmp/squashfs-fuse.log)\n"
    "\n";

/* ============================================================================
 * ID Table Loading
 * ============================================================================ */

static int load_id_table(sqfs_ctx_t *ctx) {
    int ret;
    uint64_t id_table_pos = ctx->sb->disk.id_table;
    size_t id_count = ctx->sb->disk.id_count;

    if (id_count == 0) {
        ctx->id_table = NULL;
        ctx->id_count = 0;
        return SQFS_OK;
    }

    /* Read ID table lookup table (array of 64-bit positions) */
    uint64_t *id_pos_table = malloc(id_count * sizeof(uint64_t));
    if (id_pos_table == NULL) {
        return SQFS_ERR_NO_MEMORY;
    }

    /* Read the lookup table that points to metadata blocks containing IDs */
    ret = sqfs_pread_all(ctx->sb->fd, id_pos_table,
                         id_count * sizeof(uint64_t), id_table_pos);
    if (ret != SQFS_OK) {
        free(id_pos_table);
        return ret;
    }

    /* Allocate ID table */
    ctx->id_table = malloc(id_count * sizeof(uint32_t));
    if (ctx->id_table == NULL) {
        free(id_pos_table);
        return SQFS_ERR_NO_MEMORY;
    }
    ctx->id_count = id_count;

    /* Read IDs from metadata block */
    uint8_t *meta_block = malloc(SQFS_META_MAX_SIZE);
    if (meta_block == NULL) {
        free(id_pos_table);
        free(ctx->id_table);
        ctx->id_table = NULL;
        return SQFS_ERR_NO_MEMORY;
    }

    size_t out_size;
    uint64_t first_block_pos = sqfs_le64_to_cpu(id_pos_table);

    ret = sqfs_meta_read_block(ctx->sb->fd, first_block_pos,
                               meta_block, &out_size, ctx->comp);
    if (ret != SQFS_OK) {
        free(meta_block);
        free(id_pos_table);
        free(ctx->id_table);
        ctx->id_table = NULL;
        return ret;
    }

    /* Copy IDs from metadata block */
    size_t ids_to_copy = (id_count * sizeof(uint32_t) < out_size) ?
                         id_count : out_size / sizeof(uint32_t);
    memcpy(ctx->id_table, meta_block, ids_to_copy * sizeof(uint32_t));

    free(meta_block);
    free(id_pos_table);

    SQFS_LOG("Loaded %zu IDs from table", ids_to_copy);

    return SQFS_OK;
}

/* ============================================================================
 * Context Initialization and Cleanup
 * ============================================================================ */

static int init_context(sqfs_ctx_t *ctx, int fd,
                        size_t cache_size_mb, int no_cache) {
    int ret;

    memset(ctx, 0, sizeof(*ctx));
    ctx->no_cache = no_cache;
    ctx->debug_level = 0;

    /* Load superblock */
    ctx->sb = malloc(sizeof(sqfs_superblock_t));
    if (ctx->sb == NULL) {
        return SQFS_ERR_NO_MEMORY;
    }

    ret = sqfs_superblock_load(fd, ctx->sb);
    if (ret != SQFS_OK) {
        free(ctx->sb);
        ctx->sb = NULL;
        return ret;
    }

    /* Initialize compressor */
    ctx->comp = sqfs_compressor_create((sqfs_compressor_id_t)ctx->sb->disk.compressor);
    if (ctx->comp == NULL) {
        sqfs_superblock_destroy(ctx->sb);
        free(ctx->sb);
        ctx->sb = NULL;
        return SQFS_ERR_BAD_COMP;
    }

    /* Calculate cache sizes */
    size_t total_cache = cache_size_mb * 1024 * 1024;
    size_t inode_mem = total_cache / 8;
    size_t dir_mem = total_cache / 16;
    size_t meta_mem = total_cache / 4;
    size_t data_mem = total_cache / 2;

    /* Initialize caches if not disabled */
    if (!no_cache) {
        ret = sqfs_cache_init(&ctx->inode_cache,
                              CACHE_INODE_MAX_ENTRIES, inode_mem,
                              (cache_free_fn)sqfs_inode_free);
        if (ret != 0) {
            goto error;
        }

        ret = sqfs_cache_init(&ctx->dir_cache,
                              CACHE_DIR_MAX_ENTRIES, dir_mem, NULL);
        if (ret != 0) {
            goto error;
        }

        ret = sqfs_cache_init(&ctx->meta_cache,
                              CACHE_META_MAX_ENTRIES, meta_mem, NULL);
        if (ret != 0) {
            goto error;
        }

        ret = sqfs_cache_init(&ctx->data_cache,
                              CACHE_DATA_MAX_ENTRIES, data_mem,
                              sqfs_data_block_cache_free);
        if (ret != 0) {
            goto error;
        }
    }

    /* Load ID table */
    ret = load_id_table(ctx);
    if (ret != SQFS_OK) {
        goto error;
    }

    /* Initialize and load fragment table if filesystem has fragments */
    if (ctx->sb->has_fragments) {
        ctx->fragment_table = malloc(sizeof(sqfs_fragment_table_t));
        if (ctx->fragment_table == NULL) {
            ret = SQFS_ERR_NO_MEMORY;
            goto error;
        }
        sqfs_fragment_table_init(ctx->fragment_table);

        ret = sqfs_fragment_table_load(ctx);
        if (ret != SQFS_OK) {
            SQFS_LOG("Warning: Failed to load fragment table: %d", ret);
            ctx->fragment_table_loaded = false;
        } else {
            ctx->fragment_table_loaded = true;
        }
    }

    SQFS_LOG("Context initialized: block_size=%u, compressor=%d",
             ctx->sb->disk.block_size, ctx->sb->disk.compressor);

    return SQFS_OK;

error:
    if (!no_cache) {
        sqfs_cache_destroy(&ctx->inode_cache);
        sqfs_cache_destroy(&ctx->dir_cache);
        sqfs_cache_destroy(&ctx->meta_cache);
        sqfs_cache_destroy(&ctx->data_cache);
    }
    sqfs_compressor_destroy(ctx->comp);
    sqfs_superblock_destroy(ctx->sb);
    free(ctx->sb);
    ctx->sb = NULL;
    return ret;
}

static void destroy_context(sqfs_ctx_t *ctx) {
    if (ctx == NULL) {
        return;
    }

    if (ctx->id_table != NULL) {
        free(ctx->id_table);
        ctx->id_table = NULL;
    }

    if (!ctx->no_cache) {
        sqfs_cache_destroy(&ctx->inode_cache);
        sqfs_cache_destroy(&ctx->dir_cache);
        sqfs_cache_destroy(&ctx->meta_cache);
        sqfs_cache_destroy(&ctx->data_cache);
    }

    if (ctx->comp != NULL) {
        sqfs_compressor_destroy(ctx->comp);
        ctx->comp = NULL;
    }

    if (ctx->sb != NULL) {
        sqfs_superblock_destroy(ctx->sb);
        free(ctx->sb);
        ctx->sb = NULL;
    }
}

/* ============================================================================
 * Main Function
 * ============================================================================ */

static void print_usage(void) {
    fprintf(stderr, usage_string, DEFAULT_CACHE_SIZE_MB);
}

static void print_version(void) {
    printf("squashfs-fuse version %s\n", SQFS_FUSE_VERSION_STRING);
    printf("Using FUSE %d.%d\n", FUSE_MAJOR_VERSION, FUSE_MINOR_VERSION);
    printf("Supported compressors: gzip, zstd\n");
}

/* Global context for FUSE operations */
static sqfs_ctx_t g_ctx;
static int g_fd = -1;
static char *g_image_file = NULL;
static size_t g_cache_size_mb = DEFAULT_CACHE_SIZE_MB;
static int g_no_cache = 0;

/* Initialization function called before FUSE main */
static void *sqfs_fuse_init(struct fuse_conn_info *conn,
                            struct fuse_config *cfg) {
    (void)conn;
    cfg->kernel_cache = 1;
    cfg->use_ino = 1;
    return &g_ctx;
}

/* Cleanup on exit */
static void sqfs_fuse_destroy(void *private_data) {
    (void)private_data;
    SQFS_LOG_INFO(SQFS_MOD_FUSE, "unmounting filesystem");
    sqfs_stats_report();
    destroy_context(&g_ctx);
    if (g_fd >= 0) {
        close(g_fd);
        g_fd = -1;
    }
    sqfs_log_destroy();
}

int main(int argc, char *argv[]) {
    struct fuse_args args = FUSE_ARGS_INIT(0, NULL);
    int ret;
    int i;
    char *mount_point = NULL;

    /* Initialize logging early */
    sqfs_log_config_t log_config = {
        .path = NULL,
        .max_size = 0,
        .level = SQFS_LOG_LEVEL,
    };
    sqfs_log_init(&log_config);
    sqfs_stats_init();

    /* First pass: identify image file and mount point, handle custom options */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--nocache") == 0) {
            g_no_cache = 1;
        } else if (strncmp(argv[i], "--cache-size=", 13) == 0) {
            g_cache_size_mb = (size_t)atoi(argv[i] + 13);
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage();
            sqfs_log_destroy();
            return 0;
        } else if (strcmp(argv[i], "-V") == 0 || strcmp(argv[i], "--version") == 0) {
            print_version();
            sqfs_log_destroy();
            return 0;
        } else if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--log") == 0) {
            i++;
        } else if (strncmp(argv[i], "-l", 2) == 0 && strlen(argv[i]) > 2) {
            /* -l/path format */
        } else if (argv[i][0] != '-') {
            if (g_image_file == NULL) {
                g_image_file = strdup(argv[i]);
            } else if (mount_point == NULL) {
                mount_point = strdup(argv[i]);
            }
        }
    }

    if (g_image_file == NULL) {
        fprintf(stderr, "Error: missing image file\n");
        print_usage();
        sqfs_log_destroy();
        return 1;
    }
    if (mount_point == NULL) {
        fprintf(stderr, "Error: missing mount point\n");
        print_usage();
        sqfs_log_destroy();
        return 1;
    }

    /* Second pass: build FUSE args */
    fuse_opt_add_arg(&args, argv[0]);

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--nocache") == 0 ||
            strncmp(argv[i], "--cache-size=", 13) == 0) {
            continue;
        }
        if (strcmp(argv[i], "-l") == 0 || strcmp(argv[i], "--log") == 0) {
            i++;
            continue;
        }
        if (strncmp(argv[i], "-l", 2) == 0 && strlen(argv[i]) > 2) {
            continue;
        }
        if (strcmp(argv[i], g_image_file) == 0) {
            continue;
        }
        fuse_opt_add_arg(&args, argv[i]);
    }

    SQFS_LOG_INFO(SQFS_MOD_FUSE, "opening image: %s", g_image_file);

    /* Open the SquashFS image */
    g_fd = open(g_image_file, O_RDONLY);
    if (g_fd < 0) {
        fprintf(stderr, "Error: cannot open '%s': %s\n", g_image_file, strerror(errno));
        SQFS_LOG_ERROR(SQFS_MOD_FUSE, "cannot open image: %s", strerror(errno));
        fuse_opt_free_args(&args);
        sqfs_log_destroy();
        return 1;
    }

    /* Initialize context */
    ret = init_context(&g_ctx, g_fd, g_cache_size_mb, g_no_cache);
    if (ret != SQFS_OK) {
        fprintf(stderr, "Error: failed to initialize context: %s\n",
                strerror(sqfs_errno(ret)));
        close(g_fd);
        fuse_opt_free_args(&args);
        sqfs_log_destroy();
        return 1;
    }

    SQFS_LOG_INFO(SQFS_MOD_FUSE, "filesystem initialized");

    /* Add init and destroy operations */
    struct fuse_operations sqfs_oper = sqfs_fuse_operations;
    sqfs_oper.init = sqfs_fuse_init;
    sqfs_oper.destroy = sqfs_fuse_destroy;

    /* Add default mount options */
    fuse_opt_add_arg(&args, "-oro");
    fuse_opt_add_arg(&args, "-onosuid");
    fuse_opt_add_arg(&args, "-onodev");

    /* Print info */
    printf("SquashFS-FUSE %s\n", SQFS_FUSE_VERSION_STRING);
    printf("Image: %s\n", g_image_file);
    printf("Block size: %u bytes\n", g_ctx.sb->disk.block_size);
    printf("Inodes: %u\n", g_ctx.sb->disk.inode_count);
    printf("Press Ctrl+C to unmount\n\n");
    fflush(stdout);

    /* Run FUSE */
    ret = fuse_main(args.argc, args.argv, &sqfs_oper, NULL);

    fuse_opt_free_args(&args);
    free(g_image_file);
    free(mount_point);

    return ret;
}