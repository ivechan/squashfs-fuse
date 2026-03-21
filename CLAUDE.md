# CLAUDE.md

本文件为 Claude Code (claude.ai/code) 在此代码库中工作时提供指导。

## 项目概述

SquashFS-FUSE 是一个 FUSE 3.x 文件系统驱动程序，用于挂载 SquashFS 压缩只读文件系统镜像。支持 gzip 和 zstd 压缩，实现了 LRU 缓存以提升性能，遵循 SquashFS 4.0 格式规范。

## 编译命令

```bash
# 安装依赖项 (Ubuntu/Debian)
sudo apt install libfuse3-dev libzstd-dev zlib1g-dev cmake gcc

# 使用 CMake 编译
mkdir -p build && cd build
cmake ..
make -j$(nproc)

# 启用调试日志编译
cmake -DSQFS_ENABLE_DEBUG_LOG=ON ..
make -j$(nproc)
```

编译输出：build 目录中的 `squashfs-fuse` 可执行文件。

## 测试命令

### 安装测试依赖

```bash
# 安装 CMocka (单元测试)
sudo apt install libcmocka-dev

# 安装其他测试依赖
sudo apt install squashfs-tools attr
```

### 运行测试

```bash
# 运行所有测试 (单元测试 + 功能测试)
make test

# 详细输出
make test_verbose

# 只运行单元测试
make test_unit

# 只运行功能测试
make test_functional

# 使用 CTest
ctest -V                    # 详细输出
ctest -L functional         # 只运行功能测试
```

### 测试列表

| 测试名称 | 类型 | 说明 |
|---------|------|------|
| test_cache | 单元 | LRU 缓存测试 |
| test_compressor | 单元 | 压缩器测试 |
| test_utils | 单元 | 工具函数测试 |
| functional_basic | 功能 | 基本功能测试 |
| functional_xattr | 功能 | 扩展属性测试 |
| functional_error_paths | 功能 | 错误处理测试 |
| python_read | 功能 | 文件读取测试 |

### 手动测试

```bash
# 手动挂载测试
./build/squashfs-fuse tests/fixtures/basic.sqfs /mnt/test -f
ls -la /mnt/test
fusermount -u /mnt/test
```

## 调试命令

```bash
# 前台运行并显示 FUSE 调试输出
./squashfs-fuse image.sqfs /mnt/test -f -d

# 查看应用日志
tail -f /tmp/squashfs-fuse.log

# 分析镜像结构
unsquashfs -l image.sqfs
```

## 架构

```
FUSE 操作层 (src/main.c)
        │
        ▼
Inode 管理器 (src/inode.c) ── 路径解析、inode 解析/缓存
        │
        ├── 目录表 (src/directory.c) ── 目录项解析
        ├── 数据层 (src/data.c) ── 数据块 + 碎片块读取
        └── Xattr 表 (src/xattr.c) ── 扩展属性
        │
        ▼
元数据块读取器 (src/utils.c) ── 8 KiB 块读取 + 解压
        │
        ▼
压缩器层 (src/compressor.c) ── zlib/zstd 解压
        │
        ▼
超级块和文件句柄 (src/superblock.c, src/main.c)
```

## 关键数据结构

- `sqfs_fuse_ctx_t` (src/context.h): 传递给所有 FUSE 操作的运行时状态，包含超级块、压缩器、缓存、碎片/xattr 表
- `sqfs_inode_t` (src/inode.h): 解析后的 inode，包含类型特定的联合体用于目录/文件/符号链接数据
- `sqfs_superblock_t` (src/superblock.h): 解析后的超级块，包含磁盘格式和运行时标志

## 重要实现细节

### 两级表结构
SquashFS 对导出表、碎片表和 ID 表使用两级查找表：
1. 首先读取一个 64 位指针数组（查找表）
2. 每个指针指向包含实际条目的元数据块

### 碎片读取
小于 block_size 的小文件存储在碎片块中。文件读取需要：
1. 首次访问时加载碎片表（延迟初始化）
2. 通过索引查找碎片条目
3. 读取并解压碎片块
4. 在指定偏移处提取数据

### Inode 类型
- 基本类型 (1-7): 较小的磁盘格式，文件没有 link_count 字段
- 扩展类型 (8-14): 完整元数据，包含 link_count、xattr_idx

### 元数据块格式
- 头部 (16 位): bit 15 = 未压缩标志, bits 0-14 = 压缩后大小
- 最大解压大小: 8 KiB

## 规则

1. **所有计划必须保存到 `doc/plan.md`** - 实现计划和架构决策写入此文件。

2. **遵循 `doc/design.md`** - 包含完整的数据结构定义、错误码、缓存配置和 FUSE 操作映射。

3. **格式问题参考 `doc/squashfs.adoc`** - SquashFS 二进制格式规范是权威来源。

## 文档索引

| 文件 | 用途 |
|------|------|
| `doc/design.md` | 架构设计、数据结构、缓存设计、错误码 |
| `doc/plan.md` | 实现计划和进度追踪 |
| `doc/debug.md` | 调试方法和常见问题 |
| `doc/logging.md` | 日志系统配置 |