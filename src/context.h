/*
 * SquashFS-FUSE - Context Header
 *
 * Defines the FUSE context structure shared across all modules.
 */

#ifndef SQFS_CONTEXT_H
#define SQFS_CONTEXT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "superblock.h"
#include "compressor.h"
#include "cache.h"
#include "fragment.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Forward Declarations
 * ============================================================================ */

struct sqfs_xattr_table;

/* ============================================================================
 * FUSE Private Context
 * ============================================================================ */

/*
 * sqfs_fuse_ctx_t holds all runtime state for a mounted SquashFS image.
 * This structure is passed to all FUSE operations as private data.
 */
typedef struct sqfs_fuse_ctx {
    sqfs_superblock_t          *sb;           /* Superblock */
    sqfs_compressor_t          *comp;         /* Compressor instance */
    sqfs_cache_t                inode_cache;  /* Inode cache */
    sqfs_cache_t                dir_cache;    /* Directory cache */
    sqfs_cache_t                meta_cache;   /* Metadata block cache */
    sqfs_cache_t                data_cache;   /* Data block cache */
    uint32_t                   *id_table;     /* UID/GID table */
    size_t                      id_count;     /* ID table entry count */

    /* Fragment table (loaded on demand) */
    sqfs_fragment_table_t       *fragment_table;
    bool                        fragment_table_loaded;

    /* Xattr table (loaded on demand) */
    struct sqfs_xattr_table    *xattr_table;

    /* Configuration */
    int                         no_cache;     /* Disable caching flag */
    int                         debug_level;  /* Debug verbosity level */
} sqfs_fuse_ctx_t;

#ifdef __cplusplus
}
#endif

#endif /* SQFS_CONTEXT_H */