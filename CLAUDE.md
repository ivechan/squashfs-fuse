# CLAUDE.md

本文件为 Claude Code (claude.ai/code) 在此代码库中工作时提供指导。

## 项目概述

SquashFS-FUSE 是一个 FUSE 3.x 文件系统驱动程序，用于挂载 SquashFS 压缩只读文件系统镜像。支持 gzip 和 zstd 压缩，实现了 LRU 缓存以提升性能，遵循 SquashFS 4.0 格式规范。

项目采用 VFS 抽象层架构，核心逻辑与 VFS 实现解耦，支持 FUSE 用户态和 Linux 内核模块等多种后端。

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

编译输出：
- `build/squashfs-fuse` - FUSE 可执行文件
- `build/libsquashfs_core.a` - 核心静态库

## 测试命令

### 安装测试依赖

```bash
# 安装 CMocka (单元测试)
sudo apt install libcmocka-dev

# 安装其他测试依赖
sudo apt install squashfs-tools attr

# 安装性能测试依赖
sudo apt install fio bc
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

# 运行性能测试
make test_performance

# 使用 CTest
ctest -V                    # 详细输出
ctest -L functional         # 只运行功能测试
```

### 性能测试

```bash
# 创建测试镜像
./scripts/create_perf_images.sh

# 运行性能测试
./tests/performance/run_benchmark.sh -o results.md

# 快速测试模式 (5秒每测试)
./tests/performance/run_benchmark.sh --quick -o results.md
```

性能测试详细文档见 `doc/performance.md`。

### 测试列表

| 测试名称 | 类型 | 说明 |
|---------|------|------|
| test_cache | 单元 | LRU 缓存测试 |
| test_compressor | 单元 | 压缩器测试 |
| test_utils | 单元 | 工具函数测试 |
| test_superblock | 单元 | 超级块解析测试 |
| test_inode | 单元 | Inode 解析测试 |
| test_directory | 单元 | 目录表解析测试 |
| test_fragment | 单元 | 碎片表测试 |
| test_metadata | 单元 | 元数据读取测试 |
| test_export | 单元 | 导出表测试 |
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
./build/squashfs-fuse image.sqfs /mnt/test -f -d

# 查看应用日志
tail -f /tmp/squashfs-fuse.log

# 分析镜像结构
unsquashfs -l image.sqfs
```

## 源码目录结构

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

## 架构

```
┌─────────────────────────────────────────────────────────────────┐
│                       Application Layer                          │
│  ┌─────────────────┐  ┌─────────────────┐  ┌─────────────────┐  │
│  │   FUSE Backend  │  │ Kernel Backend  │  │  Other Backend  │  │
│  │  (src/fuse/)    │  │ (src/kernel/)   │  │    (Future)     │  │
│  └────────┬────────┘  └────────┬────────┘  └────────┬────────┘  │
└───────────┼────────────────────┼────────────────────┼───────────┘
            └────────────────────┼────────────────────┘
                                 │
                                 ▼
┌─────────────────────────────────────────────────────────────────┐
│                      VFS Abstraction Layer                        │
│                          (src/vfs/)                               │
│  sqfs_vfs_attr_t | sqfs_vfs_dirent_t | sqfs_vfs_fh_t            │
│  Operations: getattr | open | read | readdir | readlink          │
└─────────────────────────────────────────────────────────────────┘
                                 │
                                 ▼
┌─────────────────────────────────────────────────────────────────┐
│                       Core Library (src/core/)                    │
│                                                                   │
│  Inode 管理器 (inode.c) ── 路径解析、inode 解析/缓存             │
│        │                                                          │
│        ├── 目录表 (directory.c) ── 目录项解析                    │
│        ├── 数据层 (data.c) ── 数据块 + 碎片块读取                │
│        └── Xattr 表 (xattr.c) ── 扩展属性                        │
│        │                                                          │
│        ▼                                                          │
│  元数据块读取器 (utils.c) ── 8 KiB 块读取 + 解压                 │
│        │                                                          │
│        ▼                                                          │
│  压缩器层 (compressor.c) ── zlib/zstd 解压                       │
│        │                                                          │
│        ▼                                                          │
│  超级块 (superblock.c)                                            │
│  日志系统 (log.c) ── 结构化 JSON 日志                            │
│  统计系统 (stats.c) ── 性能指标追踪                              │
│  缓存系统 (cache.c) ── LRU 缓存                                  │
└─────────────────────────────────────────────────────────────────┘
```

## 关键数据结构

| 结构 | 文件 | 说明 |
|------|------|------|
| `sqfs_ctx_t` | `src/core/context.h` | 运行时上下文，包含超级块、压缩器、缓存、碎片/xattr 表。`sqfs_fuse_ctx_t` 为兼容别名 |
| `sqfs_inode_t` | `src/core/inode.h` | 解析后的 inode，包含类型特定的联合体用于目录/文件/符号链接数据 |
| `sqfs_superblock_t` | `src/core/superblock.h` | 解析后的超级块，包含磁盘格式和运行时标志 |
| `sqfs_vfs_attr_t` | `src/vfs/vfs.h` | VFS 无关的文件属性结构 |
| `sqfs_vfs_dirent_t` | `src/vfs/vfs.h` | VFS 无关的目录项结构 |
| `sqfs_vfs_fh_t` | `src/vfs/vfs.h` | VFS 无关的文件句柄 |

## 重要实现细节

### VFS 抽象层
VFS 层将核心逻辑与具体 VFS 实现分离，支持：
- **FUSE 后端** (`src/fuse/`): 当前主要实现
- **内核模块** (`src/kernel/`): 未来扩展

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

2. **遵循 `doc/design.md`** - 包含完整的数据结构定义、错误码、缓存配置和 VFS 接口。

3. **格式问题参考 `doc/squashfs.adoc`** - SquashFS 二进制格式规范是权威来源。

4. **核心代码放入 `src/core/`** - 与 VFS 实现无关的核心逻辑。

5. **VFS 操作放入 `src/vfs/`** - VFS 抽象层接口和实现。

6. **后端代码放入 `src/fuse/` 或 `src/kernel/`** - 特定 VFS 后端实现。

## CI/CD

项目使用 GitHub Actions 进行持续集成：

- **构建测试**: 每次 push 和 PR 自动运行
- **调试构建**: 验证 debug 日志编译
- **代码风格**: 检查制表符和尾随空格
- **性能测试**: 快速性能基准测试，结果保存为 artifact

本地验证：

```bash
# 运行与 CI 相同的测试
mkdir -p build && cd build
cmake .. && make -j$(nproc)
make test
```

## 文档索引

| 文件 | 用途 |
|------|------|
| `doc/design.md` | 架构设计、数据结构、VFS 接口、缓存设计、错误码 |
| `doc/plan.md` | 实现计划和进度追踪 |
| `doc/performance.md` | 性能测试方案和基准测试 |
| `doc/debug.md` | 调试方法和常见问题 |
| `doc/logging.md` | 日志系统配置 |
| `CONTRIBUTING.md` | 贡献指南、代码风格、提交规范 |