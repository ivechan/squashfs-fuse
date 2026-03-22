# SquashFS-FUSE 架构设计文档

## 1. 概述

本文档定义 SquashFS-FUSE 文件系统的架构设计，所有开发 Agent 必须遵循此规范。

### 1.1 设计原则

- **第一性原理**: 基于 SquashFS 二进制格式规范 (doc/squashfs.adoc) 进行设计
- **严格模式**: 遇到任何格式不一致立即返回错误
- **线程安全**: 所有共享数据结构必须支持多线程并发访问
- **流式处理**: 大文件读取不一次性加载到内存
- **缓存友好**: 多层缓存设计，LRU 淘汰策略
- **VFS 抽象**: 核心逻辑与 VFS 实现解耦，支持多后端

### 1.2 技术约束

| 约束项 | 值 |
|--------|-----|
| 编程语言 | C11 |
| FUSE 版本 | FUSE 3.x |
| 支持压缩算法 | zlib, zstd |
| 元数据块大小 | 8 KiB (固定) |
| 数据块大小 | 4 KiB - 1 MiB (可配置) |

---

## 2. 模块架构

### 2.1 整体架构图

```
┌─────────────────────────────────────────────────────────────────────────┐
│                           Application Layer                              │
│  ┌─────────────────┐  ┌─────────────────┐  ┌─────────────────┐         │
│  │   FUSE Backend  │  │ Kernel Backend  │  │  Other Backend  │         │
│  │  (src/fuse/)    │  │ (src/kernel/)   │  │    (Future)     │         │
│  └────────┬────────┘  └────────┬────────┘  └────────┬────────┘         │
└───────────┼────────────────────┼────────────────────┼───────────────────┘
            │                    │                    │
            └────────────────────┼────────────────────┘
                                 │
                                 ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                        VFS Abstraction Layer                             │
│                            (src/vfs/)                                    │
│  ┌─────────────────────────────────────────────────────────────────┐   │
│  │  sqfs_vfs_attr_t  │  sqfs_vfs_dirent_t  │  sqfs_vfs_fh_t        │   │
│  │  sqfs_vfs_ops_t   │  sqfs_vfs_statfs_t  │                       │   │
│  └─────────────────────────────────────────────────────────────────┘   │
│  Operations: getattr | open | read | readdir | readlink | statfs       │
│              getxattr | listxattr | release                            │
└─────────────────────────────────────────────────────────────────────────┘
                                 │
                                 ▼
┌─────────────────────────────────────────────────────────────────────────┐
│                         Core Library (src/core/)                         │
│  ┌─────────────────────────────────────────────────────────────────┐   │
│  │                      Inode Manager                               │   │
│  │  解析 Inode | 缓存 Inode | 路径解析                              │   │
│  └─────────────────────────────────────────────────────────────────┘   │
│                                 │                                        │
│           ┌─────────────────────┼─────────────────────┐                 │
│           ▼                     ▼                     ▼                 │
│  ┌─────────────────┐  ┌─────────────────┐  ┌─────────────────┐         │
│  │ Directory Table │  │   Data Layer    │  │  Xattr Table    │         │
│  │   解析目录项     │  │ 数据块+Fragment │  │   扩展属性      │         │
│  └─────────────────┘  └─────────────────┘  └─────────────────┘         │
│           │                     │                     │                 │
│           └─────────────────────┼─────────────────────┘                 │
│                                 ▼                                        │
│  ┌─────────────────────────────────────────────────────────────────┐   │
│  │                    Metadata Block Reader                          │   │
│  │  读取 + 解压元数据块 (8 KiB chunks)                              │   │
│  └─────────────────────────────────────────────────────────────────┘   │
│                                 │                                        │
│                                 ▼                                        │
│  ┌─────────────────────────────────────────────────────────────────┐   │
│  │                    Compressor Layer                               │   │
│  │          zlib (deflate)  |  zstd                                  │   │
│  └─────────────────────────────────────────────────────────────────┘   │
│                                 │                                        │
│                                 ▼                                        │
│  ┌─────────────────────────────────────────────────────────────────┐   │
│  │                    Superblock & Archive                           │   │
│  │  Superblock 解析 | 文件句柄管理 | 基础信息                        │   │
│  └─────────────────────────────────────────────────────────────────┘   │
│                                                                           │
│  Caches: inode_cache | dir_cache | meta_cache | data_cache              │
└─────────────────────────────────────────────────────────────────────────┘
```

### 2.2 目录结构

```
src/
├── core/                    # 核心库 (VFS 无关)
│   ├── context.h            # sqfs_ctx_t 上下文结构
│   ├── superblock.c/h       # 超级块解析
│   ├── inode.c/h            # Inode 解析与管理
│   ├── directory.c/h        # 目录表解析
│   ├── data.c/h             # 数据块读取
│   ├── fragment.c/h         # 碎片表处理
│   ├── xattr.c/h            # 扩展属性
│   ├── compressor.c/h       # 压缩/解压
│   ├── cache.c/h            # LRU 缓存
│   ├── utils.c/h            # 工具函数
│   ├── log.c/h              # 日志系统
│   └── stats.c/h            # 统计系统
│
├── vfs/                     # VFS 抽象层
│   ├── vfs.h                # VFS 接口定义
│   └── vfs_ops.c            # VFS 操作实现
│
├── fuse/                    # FUSE 后端
│   ├── fuse_main.c          # FUSE 主程序入口
│   └── vfs_fuse.c           # FUSE VFS 适配器
│
└── kernel/                  # 内核模块框架 (未来)
    └── vfs_kernel.c         # Linux VFS 适配器骨架
```

---

## 3. VFS 抽象层

### 3.1 设计目标

VFS 抽象层将 SquashFS 核心逻辑与具体的 VFS 实现分离，支持:

- **FUSE 用户态文件系统**: 当前主要实现
- **Linux 内核模块**: 未来扩展
- **其他后端**: 如库形式嵌入其他程序

### 3.2 VFS 数据结构

```c
// src/vfs/vfs.h

// VFS 操作结果码
typedef enum {
    SQFS_VFS_OK              = 0,
    SQFS_VFS_ERR_NOT_FOUND   = -ENOENT,
    SQFS_VFS_ERR_NOT_DIR     = -ENOTDIR,
    SQFS_VFS_ERR_IS_DIR      = -EISDIR,
    SQFS_VFS_ERR_ROFS        = -EROFS,
    SQFS_VFS_ERR_NOMEM       = -ENOMEM,
    SQFS_VFS_ERR_INVAL       = -EINVAL,
    SQFS_VFS_ERR_IO          = -EIO,
    SQFS_VFS_ERR_NOSUP       = -EOPNOTSUPP,
    SQFS_VFS_ERR_NODATA      = -ENODATA,
} sqfs_vfs_result_t;

// VFS 文件属性 (VFS 无关)
typedef struct {
    uint64_t    ino;            // Inode 号
    uint32_t    mode;           // 文件类型 + 权限
    uint32_t    nlink;          // 硬链接数
    uint32_t    uid;            // 用户 ID
    uint32_t    gid;            // 组 ID
    uint64_t    size;           // 文件大小
    uint64_t    blocks;         // 512 字节块数
    uint32_t    blksize;        // I/O 块大小
    uint64_t    atime;          // 访问时间
    uint64_t    mtime;          // 修改时间
    uint64_t    ctime;          // 变更时间
    uint32_t    rdev_major;     // 设备主号
    uint32_t    rdev_minor;     // 设备次号
} sqfs_vfs_attr_t;

// VFS 目录项
typedef struct {
    char           *name;       // 名称 (需释放)
    uint64_t        ino;        // Inode 号
    uint32_t        type;       // 文件类型 (S_IFDIR, S_IFREG 等)
} sqfs_vfs_dirent_t;

// VFS 文件句柄
typedef struct sqfs_vfs_fh {
    void           *inode;      // 内部 inode 指针
    uint64_t        file_size;  // 文件大小
} sqfs_vfs_fh_t;

// VFS 文件系统统计
typedef struct {
    uint64_t    bsize;          // 块大小
    uint64_t    frsize;         // 片段大小
    uint64_t    blocks;         // 总块数
    uint64_t    bfree;          // 空闲块数
    uint64_t    bavail;         // 可用块数
    uint64_t    files;          // 总 inode 数
    uint64_t    ffree;          // 空闲 inode 数
    uint32_t    flags;          // 挂载标志
    uint32_t    namemax;        // 最大文件名长度
} sqfs_vfs_statfs_t;
```

### 3.3 VFS 操作接口

```c
// src/vfs/vfs.h

// VFS 操作函数指针表
typedef struct sqfs_vfs_ops {
    // 生命周期
    int  (*init)(sqfs_ctx_t *ctx);
    void (*destroy)(sqfs_ctx_t *ctx);

    // 文件操作
    int  (*getattr)(sqfs_ctx_t *ctx, const char *path, sqfs_vfs_attr_t *attr);
    int  (*open)(sqfs_ctx_t *ctx, const char *path, sqfs_vfs_fh_t **fh);
    int  (*read)(sqfs_ctx_t *ctx, sqfs_vfs_fh_t *fh, void *buf,
                 size_t size, uint64_t offset);
    void (*release)(sqfs_ctx_t *ctx, sqfs_vfs_fh_t *fh);

    // 目录操作
    int  (*readdir)(sqfs_ctx_t *ctx, const char *path,
                    sqfs_vfs_dirent_t **entries, size_t *count);

    // 符号链接
    int  (*readlink)(sqfs_ctx_t *ctx, const char *path, char *buf, size_t size);

    // 文件系统信息
    int  (*statfs)(sqfs_ctx_t *ctx, sqfs_vfs_statfs_t *stat);

    // 扩展属性
    int  (*getxattr)(sqfs_ctx_t *ctx, const char *path, const char *name,
                     void *value, size_t size);
    int  (*listxattr)(sqfs_ctx_t *ctx, const char *path,
                      char *list, size_t size);
} sqfs_vfs_ops_t;
```

### 3.4 VFS 操作实现

```c
// src/vfs/vfs_ops.c

// 路径解析 (核心函数)
int sqfs_vfs_resolve_path(sqfs_ctx_t *ctx, const char *path,
                          sqfs_inode_t **out_inode);

// 填充文件属性
int sqfs_vfs_fill_attr(sqfs_ctx_t *ctx, sqfs_inode_t *inode,
                       sqfs_vfs_attr_t *attr);

// 获取文件属性
int sqfs_vfs_getattr(sqfs_ctx_t *ctx, const char *path,
                     sqfs_vfs_attr_t *attr);

// 打开文件
int sqfs_vfs_open(sqfs_ctx_t *ctx, const char *path, sqfs_vfs_fh_t **fh);

// 读取文件
int sqfs_vfs_read(sqfs_ctx_t *ctx, sqfs_vfs_fh_t *fh,
                  void *buf, size_t size, uint64_t offset);

// 关闭文件
void sqfs_vfs_release(sqfs_ctx_t *ctx, sqfs_vfs_fh_t *fh);

// 读取目录
int sqfs_vfs_readdir(sqfs_ctx_t *ctx, const char *path,
                     sqfs_vfs_dirent_t **entries, size_t *count);

// 读取符号链接
int sqfs_vfs_readlink(sqfs_ctx_t *ctx, const char *path,
                      char *buf, size_t size);

// 文件系统统计
int sqfs_vfs_statfs(sqfs_ctx_t *ctx, sqfs_vfs_statfs_t *stat);

// 扩展属性
int sqfs_vfs_getxattr(sqfs_ctx_t *ctx, const char *path,
                      const char *name, void *value, size_t size);
int sqfs_vfs_listxattr(sqfs_ctx_t *ctx, const char *path,
                       char *list, size_t size);
```

---

## 4. 核心数据结构

### 4.1 上下文结构

```c
// src/core/context.h

// 主上下文结构 (VFS 无关)
typedef struct sqfs_ctx {
    sqfs_superblock_t          *sb;           // 超级块
    sqfs_compressor_t          *comp;         // 压缩器实例
    sqfs_cache_t                inode_cache;  // Inode 缓存
    sqfs_cache_t                dir_cache;    // 目录缓存
    sqfs_cache_t                meta_cache;   // 元数据块缓存
    sqfs_cache_t                data_cache;   // 数据块缓存
    uint32_t                   *id_table;     // UID/GID 表
    size_t                      id_count;     // ID 表条目数

    // 碎片表 (延迟加载)
    sqfs_fragment_table_t      *fragment_table;
    bool                        fragment_table_loaded;

    // 扩展属性表 (延迟加载)
    struct sqfs_xattr_table    *xattr_table;

    // 配置
    int                         no_cache;     // 禁用缓存标志
    int                         debug_level;  // 调试级别
} sqfs_ctx_t;

// 向后兼容别名
typedef sqfs_ctx_t sqfs_fuse_ctx_t;
```

### 4.2 Superblock (96 字节)

```c
// src/core/superblock.h

#define SQUASHFS_MAGIC        0x73717368  // "hsqs" on disk
#define SQUASHFS_VERSION_MAJOR 4
#define SQUASHFS_VERSION_MINOR 0

// 压缩算法 ID
typedef enum {
    SQUASHFS_COMP_GZIP = 1,
    SQUASHFS_COMP_LZMA = 2,
    SQUASHFS_COMP_LZO  = 3,
    SQUASHFS_COMP_XZ   = 4,
    SQUASHFS_COMP_LZ4  = 5,
    SQUASHFS_COMP_ZSTD = 6,
} squashfs_compressor_t;

// Superblock 标志位
typedef enum {
    SQUASHFS_FLAG_UNCOMP_INODES   = 0x0001,
    SQUASHFS_FLAG_UNCOMP_DATA     = 0x0002,
    SQUASHFS_FLAG_UNUSED          = 0x0004,
    SQUASHFS_FLAG_UNCOMP_FRAGS    = 0x0008,
    SQUASHFS_FLAG_NO_FRAGS        = 0x0010,
    SQUASHFS_FLAG_ALWAYS_FRAGS    = 0x0020,
    SQUASHFS_FLAG_DEDUPE          = 0x0040,
    SQUASHFS_FLAG_EXPORT          = 0x0080,
    SQUASHFS_FLAG_UNCOMP_XATTRS   = 0x0100,
    SQUASHFS_FLAG_NO_XATTRS       = 0x0200,
    SQUASHFS_FLAG_COMP_OPTS       = 0x0400,
    SQUASHFS_FLAG_UNCOMP_IDS      = 0x0800,
} squashfs_flag_t;

// Superblock 结构 (磁盘格式, 小端序)
typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t inode_count;
    uint32_t mod_time;
    uint32_t block_size;
    uint32_t frag_count;
    uint16_t compressor;
    uint16_t block_log;
    uint16_t flags;
    uint16_t id_count;
    uint16_t version_major;
    uint16_t version_minor;
    uint64_t root_inode;
    uint64_t bytes_used;
    uint64_t id_table;
    uint64_t xattr_table;
    uint64_t inode_table;
    uint64_t dir_table;
    uint64_t frag_table;
    uint64_t export_table;
} squashfs_superblock_t;

// 解析后的运行时 Superblock
typedef struct {
    squashfs_superblock_t disk;
    int fd;                    // 文件描述符
    size_t file_size;          // 镜像文件大小
    bool has_xattrs;
    bool has_fragments;
    bool has_export;
} sqfs_superblock_t;
```

### 4.3 Inode 类型与结构

```c
// src/core/inode.h

// Inode 类型枚举
typedef enum {
    SQFS_INODE_DIR      = 1,   // Basic Directory
    SQFS_INODE_FILE     = 2,   // Basic File
    SQFS_INODE_SYMLINK  = 3,   // Basic Symlink
    SQFS_INODE_BLKDEV   = 4,   // Basic Block Device
    SQFS_INODE_CHRDEV   = 5,   // Basic Character Device
    SQFS_INODE_FIFO     = 6,   // Basic FIFO
    SQFS_INODE_SOCKET   = 7,   // Basic Socket
    SQFS_INODE_LDIR     = 8,   // Extended Directory
    SQFS_INODE_LFILE    = 9,   // Extended File
    SQFS_INODE_LSYMLINK = 10,  // Extended Symlink
    SQFS_INODE_LBLKDEV  = 11,  // Extended Block Device
    SQFS_INODE_LCHRDEV  = 12,  // Extended Character Device
    SQFS_INODE_LFIFO    = 13,  // Extended FIFO
    SQFS_INODE_LSOCKET  = 14,  // Extended Socket
} sqfs_inode_type_t;

// Inode 公共头 (所有 Inode 共享)
typedef struct __attribute__((packed)) {
    uint16_t type;
    uint16_t permissions;
    uint16_t uid_idx;          // ID Table 索引
    uint16_t gid_idx;          // ID Table 索引
    uint32_t mtime;
    uint32_t inode_number;
} sqfs_inode_header_t;

// Basic Directory Inode
typedef struct __attribute__((packed)) {
    sqfs_inode_header_t header;
    uint32_t block_idx;        // 目录表块索引
    uint32_t link_count;
    uint16_t file_size;        // 目录项大小 + 3
    uint16_t block_offset;     // 块内偏移
    uint32_t parent_inode;
} sqfs_inode_dir_t;

// Extended Directory Inode
typedef struct __attribute__((packed)) {
    sqfs_inode_header_t header;
    uint32_t link_count;
    uint32_t file_size;
    uint32_t block_idx;
    uint32_t parent_inode;
    uint16_t index_count;
    uint16_t block_offset;
    uint32_t xattr_idx;        // Xattr Table 索引, 0xFFFFFFFF = 无
} sqfs_inode_ldir_t;

// Basic File Inode
typedef struct __attribute__((packed)) {
    sqfs_inode_header_t header;
    uint32_t blocks_start;     // 首个数据块位置
    uint32_t frag_idx;         // Fragment 索引, 0xFFFFFFFF = 无
    uint32_t block_offset;     // Fragment 内偏移
    uint32_t file_size;
    // 后跟 block_sizes[] 数组
} sqfs_inode_file_t;

// Extended File Inode
typedef struct __attribute__((packed)) {
    sqfs_inode_header_t header;
    uint64_t blocks_start;
    uint64_t file_size;
    uint64_t sparse;           // 稀疏文件空洞大小
    uint32_t link_count;
    uint32_t frag_idx;
    uint32_t block_offset;
    uint32_t xattr_idx;
    // 后跟 block_sizes[] 数组
} sqfs_inode_lfile_t;

// 运行时 Inode 结构 (解析后的统一表示)
struct sqfs_inode {
    uint64_t inode_number;
    sqfs_inode_type_t type;
    uint16_t permissions;
    uint16_t uid;
    uint16_t gid;
    uint32_t mtime;
    uint32_t link_count;

    union {
        struct {
            uint64_t block_idx;
            uint32_t size;
            uint16_t block_offset;
            uint64_t parent_inode;
            uint16_t index_count;
        } dir;

        struct {
            uint64_t blocks_start;
            uint64_t file_size;
            uint64_t sparse;
            uint32_t frag_idx;
            uint32_t block_offset;
            uint32_t block_count;
            uint32_t xattr_idx;
        } file;

        struct {
            char *target;
            uint32_t target_size;
        } symlink;

        struct {
            uint32_t major;
            uint32_t minor;
        } dev;

        uint32_t xattr_idx;
    };

    uint32_t *block_sizes;     // 数据块大小数组 (动态分配)
};

typedef struct sqfs_inode sqfs_inode_t;
```

### 4.4 目录表结构

```c
// src/core/directory.h

// 目录表头
typedef struct __attribute__((packed)) {
    uint32_t count;            // 条目数 - 1
    uint32_t start;            // Inode 元数据块位置
    uint32_t inode_number;     // 参考 Inode 号
} sqfs_dir_header_t;

// 目录条目
typedef struct __attribute__((packed)) {
    uint16_t offset;           // Inode 块内偏移
    int16_t inode_offset;      // 相对 Inode 号偏移
    uint16_t type;             // Inode 类型 (basic)
    uint16_t name_size;        // 名称长度 - 1
    // 后跟 name[] 字节数组
} sqfs_dir_entry_t;

// 运行时目录条目
typedef struct {
    char *name;
    uint64_t inode_number;
    uint64_t inode_ref;        // Inode 引用
    sqfs_inode_type_t type;
} sqfs_dirent_t;
```

### 4.5 Fragment 表结构

```c
// src/core/fragment.h

// Fragment 表条目 (16 字节)
typedef struct __attribute__((packed)) {
    uint64_t start;            // Fragment 块位置
    uint32_t size;             // 压缩后大小, bit24 = 未压缩
    uint32_t unused;
} sqfs_frag_entry_t;
```

### 4.6 Xattr 表结构

```c
// src/core/xattr.h

// Xattr ID 表条目 (16 字节)
typedef struct __attribute__((packed)) {
    uint64_t xattr_ref;        // 元数据引用
    uint32_t count;            // 键值对数量
    uint32_t size;             // 总大小
} sqfs_xattr_id_entry_t;

// 运行时 Xattr
typedef struct {
    char *key;                 // 完整键名 (含前缀)
    void *value;
    size_t value_size;
} sqfs_xattr_t;
```

---

## 5. 缓存系统设计

### 5.1 缓存层次结构

```
┌─────────────────────────────────────────────────────────┐
│                    LRU Cache Core                        │
│  通用 LRU 实现, 支持多线程, 读写锁                         │
└─────────────────────────────────────────────────────────┘
        │           │           │           │
        ▼           ▼           ▼           ▼
   ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌─────────┐
   │  Inode  │ │Directory│ │Metadata │ │  Data   │
   │  Cache  │ │  Cache  │ │  Block  │ │  Block  │
   └─────────┘ └─────────┘ └─────────┘ └─────────┘
```

### 5.2 通用 LRU 缓存接口

```c
// src/core/cache.h

typedef uint64_t cache_key_t;
typedef void (*cache_free_fn)(void *value);

typedef struct {
    size_t max_entries;
    size_t current_entries;
    size_t max_memory;
    size_t current_memory;

    pthread_rwlock_t lock;
    struct cache_entry *head;
    struct cache_entry *tail;

    cache_free_fn free_fn;

    struct cache_entry **buckets;
    size_t bucket_count;
} sqfs_cache_t;

int sqfs_cache_init(sqfs_cache_t *cache, size_t max_entries,
                    size_t max_memory, cache_free_fn free_fn);
void sqfs_cache_destroy(sqfs_cache_t *cache);

void *sqfs_cache_get(sqfs_cache_t *cache, cache_key_t key);
int sqfs_cache_put(sqfs_cache_t *cache, cache_key_t key, void *value,
                   size_t memory_size);
void sqfs_cache_remove(sqfs_cache_t *cache, cache_key_t key);
```

### 5.3 各层缓存配置

```c
// 默认缓存配置
#define CACHE_INODE_MAX_ENTRIES    1024
#define CACHE_INODE_MAX_MEMORY     (4 * 1024 * 1024)   // 4 MiB

#define CACHE_DIR_MAX_ENTRIES      512
#define CACHE_DIR_MAX_MEMORY       (2 * 1024 * 1024)   // 2 MiB

#define CACHE_META_MAX_ENTRIES     256
#define CACHE_META_MAX_MEMORY      (8 * 1024 * 1024)   // 8 MiB

#define CACHE_DATA_MAX_ENTRIES     128
#define CACHE_DATA_MAX_MEMORY      (16 * 1024 * 1024)  // 16 MiB
```

---

## 6. FUSE 后端

### 6.1 FUSE 操作映射

FUSE 后端位于 `src/fuse/`，通过 `vfs_fuse.c` 将 FUSE 操作映射到 VFS 接口。

| FUSE 操作 | VFS 操作 | 说明 |
|-----------|----------|------|
| `getattr` | `sqfs_vfs_getattr` | 获取文件属性 |
| `readdir` | `sqfs_vfs_readdir` | 读取目录内容 |
| `open` | `sqfs_vfs_open` | 打开文件 |
| `read` | `sqfs_vfs_read` | 读取文件内容 |
| `release` | `sqfs_vfs_release` | 关闭文件 |
| `readlink` | `sqfs_vfs_readlink` | 读取符号链接目标 |
| `statfs` | `sqfs_vfs_statfs` | 获取文件系统统计 |
| `getxattr` | `sqfs_vfs_getxattr` | 获取扩展属性 |
| `listxattr` | `sqfs_vfs_listxattr` | 列出扩展属性 |

### 6.2 FUSE 适配器示例

```c
// src/fuse/vfs_fuse.c

static int sqfs_fuse_getattr(const char *path, struct stat *stbuf,
                             struct fuse_file_info *fi) {
    sqfs_ctx_t *ctx = fuse_get_context()->private_data;
    sqfs_vfs_attr_t attr;
    int ret;

    ret = sqfs_vfs_getattr(ctx, path, &attr);
    if (ret != 0)
        return ret;

    // 转换 sqfs_vfs_attr_t 到 struct stat
    stbuf->st_ino = attr.ino;
    stbuf->st_mode = attr.mode;
    stbuf->st_nlink = attr.nlink;
    stbuf->st_uid = attr.uid;
    stbuf->st_gid = attr.gid;
    stbuf->st_size = attr.size;
    stbuf->st_blksize = attr.blksize;
    stbuf->st_blocks = attr.blocks;
    stbuf->st_mtime = attr.mtime;
    // ...

    return 0;
}

struct fuse_operations sqfs_fuse_operations = {
    .getattr    = sqfs_fuse_getattr,
    .readdir    = sqfs_fuse_readdir,
    .open       = sqfs_fuse_open,
    .read       = sqfs_fuse_read,
    .release    = sqfs_fuse_release,
    .readlink   = sqfs_fuse_readlink,
    .statfs     = sqfs_fuse_statfs,
    .getxattr   = sqfs_fuse_getxattr,
    .listxattr  = sqfs_fuse_listxattr,
};
```

---

## 7. 内核模块框架

### 7.1 概述

内核模块框架位于 `src/kernel/`，提供 Linux VFS 适配器的骨架代码。完整实现需要在内核源码树中编译。

### 7.2 Linux VFS 操作结构

```c
// src/kernel/vfs_kernel.c

#ifdef __KERNEL__

#include <linux/fs.h>
#include <linux/module.h>

// 超级块操作
static const struct super_operations squashfs_super_ops = {
    .alloc_inode    = squashfs_alloc_inode,
    .destroy_inode  = squashfs_destroy_inode,
    .statfs         = squashfs_statfs,
};

// inode 操作
static const struct inode_operations squashfs_inode_ops = {
    .getattr    = squashfs_getattr,
    .lookup     = squashfs_lookup,
};

// 文件操作
static const struct file_operations squashfs_file_ops = {
    .read_iter  = squashfs_read_iter,
    .open       = squashfs_open,
    .release    = squashfs_release,
};

// 目录操作
static const struct file_operations squashfs_dir_ops = {
    .iterate_shared = squashfs_readdir,
};

// 从 sqfs_vfs_attr_t 填充 Linux inode
void squashfs_fill_inode(struct inode *inode, sqfs_vfs_attr_t *attr) {
    inode->i_ino = attr->ino;
    inode->i_mode = attr->mode;
    set_nlink(inode, attr->nlink);
    i_uid_write(inode, attr->uid);
    i_gid_write(inode, attr->gid);
    inode->i_size = attr->size;
    // ...
}

#endif /* __KERNEL__ */
```

---

## 8. 错误处理规范

### 8.1 错误码定义

```c
// src/core/utils.h

typedef enum {
    SQFS_OK = 0,
    SQFS_ERR_CORRUPT      = -1001,  // 镜像损坏
    SQFS_ERR_BAD_MAGIC    = -1002,  // 错误的魔数
    SQFS_ERR_BAD_VERSION  = -1003,  // 不支持的版本
    SQFS_ERR_BAD_COMP     = -1004,  // 不支持的压缩算法
    SQFS_ERR_BAD_INODE    = -1005,  // 无效的 Inode
    SQFS_ERR_BAD_DIR      = -1006,  // 无效的目录项
    SQFS_ERR_BAD_XATTR    = -1007,  // 无效的扩展属性
    SQFS_ERR_BAD_BLOCK    = -1008,  // 无效的数据块
    SQFS_ERR_NOT_FOUND    = -1009,  // 未找到
    SQFS_ERR_NO_MEMORY    = -1010,  // 内存不足
    SQFS_ERR_IO           = -1011,  // I/O 错误
    SQFS_ERR_OVERFLOW     = -1012,  // 缓冲区溢出
} sqfs_error_t;

// 转换为 errno
int sqfs_errno(sqfs_error_t err);
```

---

## 9. 构建系统

### 9.1 输出目标

| 目标 | 类型 | 说明 |
|------|------|------|
| `libsquashfs_core.a` | 静态库 | 核心库，可供多后端复用 |
| `squashfs-fuse` | 可执行文件 | FUSE 用户态文件系统 |

### 9.2 编译命令

```bash
# 安装依赖
sudo apt install libfuse3-dev libzstd-dev zlib1g-dev cmake

# 编译
mkdir -p build && cd build
cmake ..
make -j$(nproc)

# 运行测试
make test
```

---

## 10. 命令行接口

### 10.1 使用方式

```bash
squashfs-fuse [OPTIONS] <image_file> <mount_point>

选项:
  -h, --help          显示帮助信息
  -V, --version       显示版本信息
  -d, --debug LEVEL   设置调试级别 (0-3)
  -o, --options OPTS  FUSE 挂载选项
  -f, --foreground    前台运行
  -s, --single        单线程模式
  --nocache           禁用所有缓存
  --cache-size SIZE   设置缓存大小 (MiB)
```

### 10.2 默认行为

- 只读挂载 (`ro`)
- 多线程模式
- 启用所有缓存层
- 后台运行 (除非指定 `-f`)

---

## 11. 问题定位

- 阅读 doc/logging.md 查看日志
- 遇到数据异常，优先确认 Squashfs 二进制格式规范 (doc/squashfs.adoc)，并以其为绝对标准
- 参考 MEMORY.md 了解关键架构决策