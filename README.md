# SquashFS-FUSE

一个用于挂载 SquashFS 压缩只读文件系统的 FUSE 3.x 驱动程序。

## 功能特性

- 支持 SquashFS 4.0 格式
- 支持 gzip 和 zstd 压缩算法
- 多层 LRU 缓存，提升读取性能
- 支持符号链接、扩展属性 (xattr)
- 支持导出表 (export table)，可通过 inode 号快速定位文件

## 依赖项

- FUSE 3.x
- zlib (gzip 压缩支持)
- zstd (zstd 压缩支持)
- CMake 3.10+

### Ubuntu/Debian

```bash
sudo apt install libfuse3-dev libzstd-dev zlib1g-dev cmake gcc
```

### Fedora

```bash
sudo dnf install fuse3-devel libzstd-devel zlib-devel cmake gcc
```

## 编译

```bash
# 使用 CMake 编译
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

编译成功后生成 `squashfs-fuse` 可执行文件。

### 编译选项

```bash
# 启用调试日志
cmake -DSQFS_ENABLE_DEBUG_LOG=ON ..
```

## 使用方法

### 基本用法

```bash
# 挂载 SquashFS 镜像
./squashfs-fuse image.sqfs /mnt/point

# 前台运行 (用于调试)
./squashfs-fuse image.sqfs /mnt/point -f

# 前台运行并显示 FUSE 调试信息
./squashfs-fuse image.sqfs /mnt/point -f -d

# 卸载
fusermount -u /mnt/point
```

### 命令行选项

```
用法: squashfs-fuse [选项] <镜像文件> <挂载点>

选项:
  -h, --help          显示帮助信息
  -V, --version       显示版本信息
  -d, --debug LEVEL   设置调试级别 (0-3)
  -o, --options OPTS  FUSE 挂载选项
  -f, --foreground    前台运行
  -s, --single        单线程模式
  --nocache           禁用所有缓存
  --cache-size SIZE   设置缓存大小 (MiB，默认: 32)
  -l, --log PATH      日志文件路径 (默认: /tmp/squashfs-fuse.log)
```

## 创建测试镜像

```bash
# 安装 squashfs-tools
sudo apt install squashfs-tools

# 创建测试内容
mkdir -p test_content/subdir
echo "Hello, World!" > test_content/hello.txt
echo "Test content" > test_content/subdir/test.txt

# 创建 SquashFS 镜像 (带导出表)
mksquashfs test_content test.sqfs -comp gzip -b 131072 -exports -noappend

# 验证镜像
unsquashfs -l test.sqfs
```

## 测试

```bash
# 运行功能测试
./tests/functional/test_basic.sh

# 详细输出
./tests/functional/test_basic.sh -v

# 指定二进制文件
./tests/functional/test_basic.sh -f ./build/squashfs-fuse
```

## 项目结构

```
squashfs-fuse/
├── src/
│   ├── main.c          # FUSE 操作实现和主入口
│   ├── context.h       # 运行时上下文结构
│   ├── superblock.c/h  # 超级块解析
│   ├── inode.c/h       # Inode 解析和路径解析
│   ├── directory.c/h   # 目录表解析
│   ├── data.c/h        # 数据块读取
│   ├── fragment.c/h    # 碎片块读取
│   ├── xattr.c/h       # 扩展属性
│   ├── compressor.c/h  # 压缩器抽象层
│   ├── cache.c/h       # LRU 缓存实现
│   ├── utils.c/h       # 通用工具函数
│   ├── log.c/h         # 日志系统
│   └── stats.c/h       # 统计信息
├── doc/
│   ├── design.md       # 架构设计文档
│   ├── plan.md         # 实现计划
│   ├── debug.md        # 调试指南
│   └── logging.md      # 日志系统设计
├── tests/
│   ├── fixtures/       # 测试镜像
│   └── functional/     # 功能测试
└── CMakeLists.txt
```

## 架构

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
                              │
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
```

## 调试

### 查看日志

```bash
# 日志文件位置
tail -f /tmp/squashfs-fuse.log

# 查看特定模块日志
grep '"module":"inode"' /tmp/squashfs-fuse.log

# 查看错误日志
grep '"level":"ERROR"' /tmp/squashfs-fuse.log
```

### FUSE 调试模式

```bash
# 前台运行并显示 FUSE 调试信息
./squashfs-fuse image.sqfs /mnt -f -d
```

### 常见问题

详见 [doc/debug.md](doc/debug.md)

## 许可证

GPL-2.0-or-later