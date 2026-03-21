# SquashFS-FUSE 架构设计文档

## 1. 概述

本文档定义 SquashFS-FUSE 文件系统的架构设计，所有开发 Agent 必须遵循此规范。

### 1.1 设计原则

- **第一性原理**: 基于 SquashFS 二进制格式规范 (doc/squashfs.adoc) 进行设计
- **严格模式**: 遇到任何格式不一致立即返回错误
- **线程安全**: 所有共享数据结构必须支持多线程并发访问
- **流式处理**: 大文件读取不一次性加载到内存
- **缓存友好**: 多层缓存设计，LRU 淘汰策略

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

```
┌─────────────────────────────────────────────────────────────┐
│                      FUSE Operations                        │
│  getattr | readdir | open | read | readlink | statfs       │
│  getxattr | listxattr                                       │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                      Inode Manager                           │
│  解析 Inode | 缓存 Inode | 路径解析                          │
└─────────────────────────────────────────────────────────────┘
                              │
          ┌───────────────────┼───────────────────┐
          ▼                   ▼                   ▼
┌─────────────────┐ ┌─────────────────┐ ┌─────────────────┐
│ Directory Table │ │   Data Layer    │ │  Xattr Table    │
│   解析目录项     │ │ 数据块+Fragment │ │   扩展属性      │
└─────────────────┘ └─────────────────┘ └─────────────────┘
          │                   │                   │
          └───────────────────┼───────────────────┘
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                    Metadata Block Reader                     │
│  读取 + 解压元数据块 (8 KiB chunks)                          │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                    Compressor Layer                          │
│          zlib (deflate)  |  zstd                            │
└─────────────────────────────────────────────────────────────┘
                              │
                              ▼
┌─────────────────────────────────────────────────────────────┐
│                    Superblock & Archive                      │
│  Superblock 解析 | 文件句柄管理 | 基础信息                    │
└─────────────────────────────────────────────────────────────┘
```

---

## 3. 核心数据结构

### 3.1 Superblock (96 字节)

```c
// src/superblock.h

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

### 3.2 Inode 类型与结构

```c
// src/inode.h

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

// Basic Symlink Inode
typedef struct __attribute__((packed)) {
    sqfs_inode_header_t header;
    uint32_t link_count;
    uint32_t target_size;
    // 后跟 target_path[] 字节数组
} sqfs_inode_symlink_t;

// Device Inode (Block/Char)
typedef struct __attribute__((packed)) {
    sqfs_inode_header_t header;
    uint32_t link_count;
    uint32_t device_number;    // major/minor 编码
} sqfs_inode_dev_t;

// IPC Inode (FIFO/Socket)
typedef struct __attribute__((packed)) {
    sqfs_inode_header_t header;
    uint32_t link_count;
} sqfs_inode_ipc_t;

// 运行时 Inode 结构 (解析后的统一表示)
typedef struct {
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
} sqfs_inode_t;
```

### 3.3 目录表结构

```c
// src/directory.h

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

// 目录索引 (仅 Extended Directory)
typedef struct __attribute__((packed)) {
    uint32_t index;            // 字节偏移
    uint32_t start;            // 目录表块位置
    uint32_t name_size;        // 名称长度 - 1
    // 后跟 name[] 字节数组
} sqfs_dir_index_t;

// 运行时目录条目
typedef struct {
    char *name;
    uint64_t inode_number;
    sqfs_inode_type_t type;
} sqfs_dirent_t;
```

### 3.4 Fragment 表结构

```c
// src/fragment.h

// Fragment 表条目 (16 字节)
typedef struct __attribute__((packed)) {
    uint64_t start;            // Fragment 块位置
    uint32_t size;             // 压缩后大小, bit24 = 未压缩
    uint32_t unused;
} sqfs_frag_entry_t;

// Fragment 缓存项
typedef struct {
    uint64_t block_start;
    void *data;                // 解压后的数据 (block_size 大小)
    size_t data_size;
    bool cached;
} sqfs_fragment_t;
```

### 3.5 Xattr 表结构

```c
// src/xattr.h

// Xattr 前缀类型
typedef enum {
    SQFS_XATTR_PREFIX_USER     = 0,
    SQFS_XATTR_PREFIX_TRUSTED  = 1,
    SQFS_XATTR_PREFIX_SECURITY = 2,
} sqfs_xattr_prefix_t;

// Xattr 键结构
typedef struct __attribute__((packed)) {
    uint16_t type;             // 前缀类型 | 0x0100 表示 out-of-line
    uint16_t name_size;
    // 后跟 name[] 字节数组
} sqfs_xattr_key_t;

// Xattr 值结构
typedef struct __attribute__((packed)) {
    uint32_t value_size;
    // 后跟 value[] 字节数组
} sqfs_xattr_value_t;

// Xattr ID 表条目 (16 字节)
typedef struct __attribute__((packed)) {
    uint64_t xattr_ref;        // 元数据引用 (位置 << 16 | 偏移)
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

## 4. 缓存系统设计

### 4.1 缓存层次结构

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

### 4.2 通用 LRU 缓存接口

```c
// src/cache.h

// 缓存键类型
typedef uint64_t cache_key_t;

// 缓存值释放函数
typedef void (*cache_free_fn)(void *value);

// LRU 缓存结构
typedef struct {
    size_t max_entries;        // 最大条目数
    size_t current_entries;    // 当前条目数
    size_t max_memory;         // 最大内存使用 (字节)
    size_t current_memory;     // 当前内存使用

    pthread_rwlock_t lock;     // 读写锁
    struct cache_entry *head;  // LRU 链表头 (最近使用)
    struct cache_entry *tail;  // LRU 链表尾 (最少使用)

    cache_free_fn free_fn;     // 值释放函数

    // 哈希表 (开放寻址)
    struct cache_entry **buckets;
    size_t bucket_count;
} sqfs_cache_t;

// 缓存操作接口
int sqfs_cache_init(sqfs_cache_t *cache, size_t max_entries,
                    size_t max_memory, cache_free_fn free_fn);
void sqfs_cache_destroy(sqfs_cache_t *cache);

void *sqfs_cache_get(sqfs_cache_t *cache, cache_key_t key);
int sqfs_cache_put(sqfs_cache_t *cache, cache_key_t key, void *value,
                   size_t memory_size);
void sqfs_cache_remove(sqfs_cache_t *cache, cache_key_t key);
void sqfs_cache_clear(sqfs_cache_t *cache);

// 统计接口
size_t sqfs_cache_hits(sqfs_cache_t *cache);
size_t sqfs_cache_misses(sqfs_cache_t *cache);
```

### 4.3 各层缓存配置

```c
// 默认缓存配置
#define CACHE_INODE_MAX_ENTRIES    1024
#define CACHE_INODE_MAX_MEMORY     (4 * 1024 * 1024)   // 4 MiB

#define CACHE_DIR_MAX_ENTRIES      512
#define CACHE_DIR_MAX_MEMORY       (2 * 1024 * 1024)   // 2 MiB

#define CACHE_META_MAX_ENTRIES     256
#define CACHE_META_MAX_MEMORY      (8 * 1024 * 1024)   // 8 MiB (每个块 8 KiB)

#define CACHE_DATA_MAX_ENTRIES     128
#define CACHE_DATA_MAX_MEMORY      (16 * 1024 * 1024)  // 16 MiB
```

---

## 5. 压缩器抽象层

### 5.1 压缩器接口

```c
// src/compressor.h

// 压缩器操作接口
typedef struct {
    const char *name;
    int (*decompress)(const void *src, size_t src_size,
                      void *dst, size_t dst_size);
    int (*init)(void **ctx);
    void (*destroy)(void *ctx);
} sqfs_compressor_t;

// 压缩器注册
int sqfs_compressor_register(sqfs_compressor_t *comp);
sqfs_compressor_t *sqfs_compressor_get(sqfs_compressor_t type);

// 内置压缩器
extern sqfs_compressor_t sqfs_compressor_zlib;
extern sqfs_compressor_t sqfs_compressor_zstd;
```

### 5.2 错误码定义

```c
// 压缩器返回值
#define SQFS_COMP_OK           0    // 成功
#define SQFS_COMP_ERROR       -1    // 一般错误
#define SQFS_COMP_OVERFLOW    -2    // 输出缓冲区不足
#define SQFS_COMP_CORRUPT     -3    // 数据损坏
#define SQFS_COMP_UNSUPPORTED -4    // 不支持的压缩算法
```

---

## 6. 元数据块读取器

### 6.1 元数据引用格式

SquashFS 使用 64 位元数据引用:
- 高 48 位: 元数据块在磁盘上的位置 (相对于表起始)
- 低 16 位: 解压后块内偏移

```c
// src/utils.h

// 元数据引用解析
static inline uint64_t sqfs_meta_block_pos(uint64_t ref) {
    return ref >> 16;
}

static inline uint16_t sqfs_meta_block_offset(uint64_t ref) {
    return ref & 0xFFFF;
}

static inline uint64_t sqfs_make_meta_ref(uint64_t pos, uint16_t offset) {
    return (pos << 16) | offset;
}
```

### 6.2 元数据块读取接口

```c
// src/utils.h

// 元数据块头 (16 位)
// bit 15: 是否未压缩
// bit 0-14: 压缩后大小 (最大 8 KiB)
#define SQFS_META_UNCOMPRESSED_FLAG 0x8000
#define SQFS_META_MAX_SIZE          8192

// 读取元数据块
int sqfs_read_metadata_block(int fd, uint64_t pos,
                             void *buffer, size_t *out_size,
                             sqfs_compressor_t *comp);

// 读取跨越块边界的元数据记录
int sqfs_read_metadata(int fd, uint64_t ref,
                       void *buffer, size_t size,
                       sqfs_cache_t *block_cache,
                       sqfs_compressor_t *comp);
```

---

## 7. FUSE 操作映射

### 7.1 支持的 FUSE 操作

| FUSE 操作 | 实现函数 | 说明 |
|-----------|----------|------|
| `getattr` | `sqfs_fuse_getattr` | 获取文件属性 |
| `readdir` | `sqfs_fuse_readdir` | 读取目录内容 |
| `open` | `sqfs_fuse_open` | 打开文件 |
| `read` | `sqfs_fuse_read` | 读取文件内容 |
| `release` | `sqfs_fuse_release` | 关闭文件 |
| `readlink` | `sqfs_fuse_readlink` | 读取符号链接目标 |
| `statfs` | `sqfs_fuse_statfs` | 获取文件系统统计 |
| `getxattr` | `sqfs_fuse_getxattr` | 获取扩展属性 |
| `listxattr` | `sqfs_fuse_listxattr` | 列出扩展属性 |

### 7.2 私有数据结构

```c
// src/main.c

// FUSE 私有数据
typedef struct {
    sqfs_superblock_t *sb;
    sqfs_compressor_t *comp;
    sqfs_cache_t inode_cache;
    sqfs_cache_t dir_cache;
    sqfs_cache_t meta_cache;
    sqfs_cache_t data_cache;
    uint32_t *id_table;        // UID/GID 表 (已加载)
    size_t id_count;
} sqfs_fuse_ctx_t;
```

### 7.3 路径解析

```c
// src/inode.h

// 路径解析结果
typedef struct {
    uint64_t inode_number;
    sqfs_inode_t *inode;
    char *remaining;           // 未解析的路径部分 (对于符号链接)
} sqfs_path_resolve_t;

// 路径解析接口
int sqfs_resolve_path(sqfs_fuse_ctx_t *ctx, const char *path,
                      sqfs_path_resolve_t *result);

// 通过 Inode 号获取 Inode
int sqfs_get_inode(sqfs_fuse_ctx_t *ctx, uint64_t inode_num,
                   sqfs_inode_t **inode);
```

---

## 8. 错误处理规范

### 8.1 错误码定义

```c
// src/utils.h

// SquashFS 错误码 (负值, 与 errno 兼容)
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
static inline int sqfs_errno(sqfs_error_t err) {
    switch (err) {
        case SQFS_OK:              return 0;
        case SQFS_ERR_CORRUPT:
        case SQFS_ERR_BAD_MAGIC:
        case SQFS_ERR_BAD_VERSION:
        case SQFS_ERR_BAD_INODE:
        case SQFS_ERR_BAD_DIR:
        case SQFS_ERR_BAD_XATTR:
        case SQFS_ERR_BAD_BLOCK:   return EIO;
        case SQFS_ERR_NOT_FOUND:   return ENOENT;
        case SQFS_ERR_NO_MEMORY:   return ENOMEM;
        case SQFS_ERR_IO:          return EIO;
        case SQFS_ERR_OVERFLOW:    return EOVERFLOW;
        default:                   return EIO;
    }
}
```

### 8.2 错误处理宏

```c
// src/utils.h

// 错误检查宏
#define SQFS_CHECK(cond, err) do { \
    if (!(cond)) return (err); \
} while(0)

#define SQFS_CHECK_GOTO(cond, err, label) do { \
    if (!(cond)) { ret = (err); goto label; } \
} while(0)

// 日志宏
#ifdef SQFS_DEBUG
#define SQFS_LOG(fmt, ...) fprintf(stderr, "[sqfs] " fmt "\n", ##__VA_ARGS__)
#else
#define SQFS_LOG(fmt, ...) do {} while(0)
#endif
```

---

## 9. 线程安全设计

### 9.1 锁策略

| 数据结构 | 锁类型 | 粒度 |
|----------|--------|------|
| 缓存 | 读写锁 | 整体 |
| Superblock | 无锁 (只读) | - |
| Inode | 无锁 (缓存后只读) | - |
| 文件描述符 | 互斥锁 | 每个文件 |

### 9.2 并发读取流程

```
Thread 1                    Thread 2
    │                           │
    ▼                           ▼
cache_get (读锁)           cache_get (读锁)
    │                           │
    ▼ (命中)                     ▼ (未命中)
返回数据                    升级写锁
    │                           │
    │                           ▼
    │                      读取+解压
    │                           │
    │                           ▼
    │                      cache_put
    │                           │
    ▼                           ▼
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


## 11. 问题定位
- 阅读 doc/logging.md 查看日志
- 遇到数据异常，优先确认 Squashfs 二进制格式规范(doc/squashfs.adoc)，并以其为绝对标准
