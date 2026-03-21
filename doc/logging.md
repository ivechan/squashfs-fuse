# SquashFS-FUSE 维测日志方案

## 1. 概述

本文档定义 SquashFS-FUSE 的维测日志系统设计，用于问题定位和性能分析。

### 1.1 设计目标

- **问题定位**: 记录关键操作流程，便于追踪问题根因
- **性能分析**: 统计关键指标，支持性能调优
- **低开销**: 编译期开关，生产环境零性能损耗
- **易解析**: JSON 格式输出，便于日志分析工具处理

### 1.2 设计决策

| 决策项 | 选择 |
|--------|------|
| 日志输出方式 | 文件 |
| 日志级别 | 4级 (ERROR/WARN/INFO/DEBUG) |
| 性能控制 | 编译期开关 |
| 模块划分 | 按架构模块 |
| 日志路径 | 默认 `/tmp/squashfs-fuse.log`，可通过 `-l` 指定 |
| 日志格式 | JSON 格式 |
| 日志轮转 | 单文件最大 10MB，超出覆盖 |
| 统计功能 | 每 60 秒自动输出统计摘要 |

---

## 2. 日志级别定义

### 2.1 级别枚举

```c
// src/log.h

typedef enum {
    SQFS_LOG_ERROR = 1,   // 错误：影响功能的严重问题
    SQFS_LOG_WARN  = 2,   // 警告：潜在问题或异常情况
    SQFS_LOG_INFO  = 3,   // 信息：关键操作记录
    SQFS_LOG_DEBUG = 4,   // 调试：详细操作信息
} sqfs_log_level_t;
```

### 2.2 级别用途

| 级别 | 用途 | 示例 |
|------|------|------|
| ERROR | 无法恢复的错误、数据损坏、资源耗尽 | 解压失败、inode 解析错误、内存分配失败 |
| WARN | 可恢复的异常、性能下降、配置问题 | 缓存淘汰频繁、磁盘读取慢、参数不推荐 |
| INFO | 关键操作、状态变化、统计摘要 | 挂载成功、文件打开、定期统计 |
| DEBUG | 详细操作流程、中间状态 | 缓存命中/未命中、块读取详情、目录遍历 |

### 2.3 编译期开关

```c
// 默认日志级别，可在编译时覆盖
#ifndef SQFS_LOG_LEVEL
#define SQFS_LOG_LEVEL SQFS_LOG_WARN
#endif

// 级别检查宏
#define SQFS_LOG_ENABLED(level) ((level) <= SQFS_LOG_LEVEL)
```

---

## 3. 模块划分

### 3.1 模块枚举

```c
// src/log.h

typedef enum {
    SQFS_MOD_SUPERBLOCK = 0,   // Superblock 解析
    SQFS_MOD_INODE      = 1,   // Inode 管理
    SQFS_MOD_DIRECTORY  = 2,   // 目录表解析
    SQFS_MOD_DATA       = 3,   // 数据块读取
    SQFS_MOD_FRAGMENT   = 4,   // Fragment 处理
    SQFS_MOD_COMPRESSOR = 5,   // 压缩/解压
    SQFS_MOD_CACHE      = 6,   // 缓存系统
    SQFS_MOD_XATTR      = 7,   // 扩展属性
    SQFS_MOD_FUSE       = 8,   // FUSE 操作
    SQFS_MOD_COUNT      = 9,   // 模块总数
} sqfs_module_t;

// 模块名称
static const char *sqfs_module_names[] = {
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
```

---

## 4. 日志格式

### 4.1 JSON 结构

```json
{
    "ts": "2024-01-15T10:30:45.123Z",
    "level": "ERROR",
    "module": "inode",
    "msg": "failed to parse inode",
    "data": {
        "inode_num": 12345,
        "type": 2,
        "pos": 4096,
        "error": "invalid header"
    }
}
```

### 4.2 字段说明

| 字段 | 类型 | 必需 | 说明 |
|------|------|------|------|
| ts | string | 是 | ISO 8601 时间戳，精确到毫秒 |
| level | string | 是 | 日志级别: ERROR/WARN/INFO/DEBUG |
| module | string | 是 | 模块名称 |
| msg | string | 是 | 日志消息 |
| data | object | 否 | 附加数据，包含上下文信息 |
| file | string | 否 | 源文件名 (仅 DEBUG 级别) |
| line | number | 否 | 行号 (仅 DEBUG 级别) |
| thread | number | 否 | 线程 ID (多线程环境) |

### 4.3 日志 API

```c
// src/log.h

// 基础日志函数
void sqfs_log(sqfs_log_level_t level, sqfs_module_t module,
              const char *file, int line, const char *fmt, ...);

// 便捷宏
#define SQFS_LOG_ERROR(mod, fmt, ...) \
    SQFS_LOG_ENABLED(SQFS_LOG_ERROR) ? (void)0 : \
    sqfs_log(SQFS_LOG_ERROR, mod, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define SQFS_LOG_WARN(mod, fmt, ...) \
    SQFS_LOG_ENABLED(SQFS_LOG_WARN) ? (void)0 : \
    sqfs_log(SQFS_LOG_WARN, mod, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define SQFS_LOG_INFO(mod, fmt, ...) \
    SQFS_LOG_ENABLED(SQFS_LOG_INFO) ? (void)0 : \
    sqfs_log(SQFS_LOG_INFO, mod, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

#define SQFS_LOG_DEBUG(mod, fmt, ...) \
    SQFS_LOG_ENABLED(SQFS_LOG_DEBUG) ? (void)0 : \
    sqfs_log(SQFS_LOG_DEBUG, mod, __FILE__, __LINE__, fmt, ##__VA_ARGS__)

// 带附加数据的日志
void sqfs_log_data(sqfs_log_level_t level, sqfs_module_t module,
                   const char *file, int line,
                   const char *msg, const char *json_data);

#define SQFS_LOG_ERROR_DATA(mod, msg, json_data) \
    SQFS_LOG_ENABLED(SQFS_LOG_ERROR) ? (void)0 : \
    sqfs_log_data(SQFS_LOG_ERROR, mod, __FILE__, __LINE__, msg, json_data)
```

---

## 5. 日志系统接口

### 5.1 初始化与销毁

```c
// src/log.h

// 日志配置
typedef struct {
    const char *path;         // 日志文件路径，NULL 则使用默认
    size_t max_size;          // 最大文件大小 (字节)，默认 10MB
    sqfs_log_level_t level;   // 运行时日志级别 (不能超过编译期级别)
} sqfs_log_config_t;

// 初始化日志系统
int sqfs_log_init(const sqfs_log_config_t *config);

// 销毁日志系统
void sqfs_log_destroy(void);

// 刷新日志缓冲区
void sqfs_log_flush(void);
```

### 5.2 命令行集成

```bash
# 使用示例
squashfs-fuse -l /var/log/sqfs.log image.sqfs /mnt

# 日志级别通过编译选项控制
cmake -DSQFS_LOG_LEVEL=4 ..  # 启用 DEBUG
cmake -DSQFS_LOG_LEVEL=2 ..  # 只输出 ERROR 和 WARN
```

---

## 6. 日志文件管理

### 6.1 文件轮转

```
单文件模式:
- 默认路径: /tmp/squashfs-fuse.log
- 最大大小: 10 MB
- 写入策略: 追加写入，超出大小时从头覆盖 (环形缓冲)

实现方式:
1. 打开文件时检查大小
2. 若超出限制，ftruncate(0) 清空
3. 重新开始写入
```

### 6.2 线程安全

```c
// 使用互斥锁保护日志写入
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

void sqfs_log(sqfs_log_level_t level, sqfs_module_t module,
              const char *file, int line, const char *fmt, ...) {
    pthread_mutex_lock(&log_mutex);
    // ... 写入日志 ...
    pthread_mutex_unlock(&log_mutex);
}
```

---

## 7. 统计功能

### 7.1 统计指标

```c
// src/stats.h

typedef struct {
    // 操作计数
    uint64_t open_count;          // 文件打开次数
    uint64_t read_count;          // 读取次数
    uint64_t read_bytes;          // 读取字节数
    uint64_t readdir_count;       // 目录读取次数

    // 缓存统计 (各缓存层)
    struct {
        uint64_t hits;
        uint64_t misses;
        uint64_t evictions;
        uint64_t current_entries;
        uint64_t current_memory;
    } cache[SQFS_MOD_COUNT];  // inode, dir, meta, data

    // 压缩统计
    struct {
        uint64_t decompress_count;
        uint64_t decompress_bytes_in;
        uint64_t decompress_bytes_out;
        uint64_t decompress_errors;
        uint64_t total_time_us;    // 总解压时间 (微秒)
    } compressor;

    // 错误统计
    uint64_t error_count;
    uint64_t io_errors;
    uint64_t corrupt_errors;

    // 时间戳
    time_t start_time;             // 启动时间
    time_t last_report;            // 上次报告时间
} sqfs_stats_t;

// 全局统计实例
extern sqfs_stats_t g_stats;

// 统计操作接口
void sqfs_stats_init(void);
void sqfs_stats_report(void);  // 输出统计摘要
```

### 7.2 统计输出格式

```json
{
    "ts": "2024-01-15T10:31:45Z",
    "type": "stats",
    "uptime_sec": 3600,
    "operations": {
        "open": 1523,
        "read": 45678,
        "read_bytes": 104857600,
        "readdir": 234
    },
    "cache": {
        "inode": {"hits": 4500, "misses": 500, "evictions": 23, "entries": 1024, "memory": 4194304},
        "data": {"hits": 12000, "misses": 3000, "evictions": 150, "entries": 128, "memory": 16777216}
    },
    "compressor": {
        "calls": 3500,
        "bytes_in": 52428800,
        "bytes_out": 104857600,
        "errors": 0,
        "avg_time_us": 125
    },
    "errors": {
        "total": 0,
        "io": 0,
        "corrupt": 0
    }
}
```

### 7.3 定时输出

```c
// 每 60 秒输出一次统计摘要
#define STATS_REPORT_INTERVAL 60

// 在主循环或单独线程中
void stats_timer_thread(void) {
    while (running) {
        sleep(STATS_REPORT_INTERVAL);
        sqfs_stats_report();
    }
}
```

---

## 8. 各模块日志点设计

### 8.1 Superblock 模块

| 操作 | 级别 | 消息 | 附加数据 |
|------|------|------|----------|
| 解析开始 | DEBUG | "parsing superblock" | pos |
| 解析成功 | INFO | "superblock parsed" | magic, version, block_size, compressor, inode_count |
| 魔数错误 | ERROR | "invalid magic" | expected, actual |
| 版本不支持 | ERROR | "unsupported version" | major, minor |
| 压缩算法不支持 | ERROR | "unsupported compressor" | compressor_id |

### 8.2 Inode 模块

| 操作 | 级别 | 消息 | 附加数据 |
|------|------|------|----------|
| 解析 Inode | DEBUG | "parsing inode" | inode_num, pos |
| 解析成功 | DEBUG | "inode parsed" | inode_num, type, permissions |
| 类型无效 | ERROR | "invalid inode type" | inode_num, type |
| 缓存命中 | DEBUG | "inode cache hit" | inode_num |
| 缓存未命中 | DEBUG | "inode cache miss" | inode_num |

### 8.3 Directory 模块

| 操作 | 级别 | 消息 | 附加数据 |
|------|------|------|----------|
| 读取目录 | DEBUG | "reading directory" | inode_num |
| 遍历条目 | DEBUG | "directory entry" | name, inode_num, type |
| 目录损坏 | ERROR | "corrupt directory" | inode_num, offset, reason |

### 8.4 Data 模块

| 操作 | 级别 | 消息 | 附加数据 |
|------|------|------|----------|
| 读取数据块 | DEBUG | "reading data block" | block_idx, size, compressed |
| 读取完成 | DEBUG | "data block read" | block_idx, bytes_read, time_us |
| 读取错误 | ERROR | "data read error" | block_idx, offset, error |
| Fragment 读取 | DEBUG | "reading fragment" | frag_idx, offset, size |

### 8.5 Compressor 模块

| 操作 | 级别 | 消息 | 附加数据 |
|------|------|------|----------|
| 解压开始 | DEBUG | "decompressing" | algo, src_size, dst_size |
| 解压完成 | DEBUG | "decompress done" | algo, time_us |
| 解压失败 | ERROR | "decompress failed" | algo, src_size, error_code |

### 8.6 Cache 模块

| 操作 | 级别 | 消息 | 附加数据 |
|------|------|------|----------|
| 缓存命中 | DEBUG | "cache hit" | cache_type, key |
| 缓存未命中 | DEBUG | "cache miss" | cache_type, key |
| 缓存插入 | DEBUG | "cache insert" | cache_type, key, size |
| 缓存淘汰 | DEBUG | "cache evict" | cache_type, key, reason |
| 缓存满 | WARN | "cache full" | cache_type, entries, memory |

### 8.7 FUSE 模块

| 操作 | 级别 | 消息 | 附加数据 |
|------|------|------|----------|
| getattr | DEBUG | "getattr" | path, inode |
| open | INFO | "file open" | path, inode, flags |
| read | DEBUG | "file read" | path, offset, size |
| readdir | DEBUG | "readdir" | path, inode |
| 操作错误 | ERROR | "operation failed" | op, path, error |

---

## 9. 错误日志详情

### 9.1 错误信息结构

```c
// 错误上下文
typedef struct {
    const char *operation;     // 操作名称
    int error_code;            // 错误码 (sqfs_error_t 或 errno)
    const char *error_msg;     // 错误消息
    const char *file;          // 源文件
    int line;                  // 行号
    struct {
        const char *key;
        const char *value_fmt;
        // ... 可变参数
    } params[4];               // 最多 4 个参数
} sqfs_error_ctx_t;

// 记录详细错误
void sqfs_log_error_detail(sqfs_module_t module, const sqfs_error_ctx_t *ctx);
```

### 9.2 错误日志示例

```json
{
    "ts": "2024-01-15T10:30:45.123Z",
    "level": "ERROR",
    "module": "inode",
    "msg": "failed to parse inode header",
    "data": {
        "operation": "parse_inode",
        "error_code": -1005,
        "error_msg": "invalid inode",
        "file": "inode.c",
        "line": 234,
        "params": {
            "inode_num": 12345,
            "pos": 8192,
            "header_type": 15,
            "expected": "1-14"
        }
    }
}
```

---

## 10. 实现文件结构

```
src/
├── log.h              # 日志系统接口定义
├── log.c              # 日志系统实现
├── stats.h            # 统计系统接口定义
└── stats.c            # 统计系统实现
```

---

## 11. 使用示例

### 11.1 代码集成

```c
// src/inode.c

int sqfs_parse_inode(sqfs_fuse_ctx_t *ctx, uint64_t inode_num, sqfs_inode_t *inode) {
    SQFS_LOG_DEBUG(SQFS_MOD_INODE, "parsing inode %lu", inode_num);

    // ... 解析逻辑 ...

    if (header.type < 1 || header.type > 14) {
        SQFS_LOG_ERROR_DATA(SQFS_MOD_INODE, "invalid inode type",
            "{\"inode_num\":%lu,\"type\":%d,\"pos\":%lu}",
            inode_num, header.type, pos);
        return SQFS_ERR_BAD_INODE;
    }

    SQFS_LOG_DEBUG(SQFS_MOD_INODE, "inode %lu parsed, type=%d", inode_num, header.type);
    return SQFS_OK;
}
```

### 11.2 编译配置

```cmake
# CMakeLists.txt

option(SQFS_ENABLE_DEBUG_LOG "Enable debug logging" OFF)

if(SQFS_ENABLE_DEBUG_LOG)
    add_definitions(-DSQFS_LOG_LEVEL=4)  # DEBUG
else()
    add_definitions(-DSQFS_LOG_LEVEL=2)  # WARN
endif()
```

### 11.3 运行示例

```bash
# 默认日志级别运行 (WARN)
./squashfs-fuse image.sqfs /mnt

# 启用调试日志编译
cmake -DSQFS_ENABLE_DEBUG_LOG=ON ..
make

# 指定日志文件路径
./squashfs-fuse -l /var/log/sqfs-debug.log image.sqfs /mnt

# 查看日志
tail -f /tmp/squashfs-fuse.log | jq .
```

---

## 12. 性能考虑

### 12.1 编译期开销

- 低于配置级别的日志调用会被编译器完全优化掉
- 零运行时检查开销

### 12.2 运行时开销

| 操作 | 开销 |
|------|------|
| 日志级别判断 | 整数比较 |
| 格式化 | 仅在需要输出时执行 |
| 文件写入 | 缓冲写入，不立即刷新 |
| JSON 序列化 | 使用轻量级实现，避免复杂库 |

### 12.3 优化建议

```c
// 避免在热路径中构造复杂 JSON
// 不推荐:
SQFS_LOG_DEBUG(SQFS_MOD_DATA, "block info: %s", complex_json_string());

// 推荐:
if (SQFS_LOG_ENABLED(SQFS_LOG_DEBUG)) {
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"block\":%lu,\"size\":%lu}", block, size);
    SQFS_LOG_DEBUG_DATA(SQFS_MOD_DATA, "block info", buf);
}
```