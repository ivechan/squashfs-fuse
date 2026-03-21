# SquashFS-FUSE 测试方案

> 本文档定义了完整的测试策略，覆盖 SquashFS 4.0 格式规范的所有关键功能。

## 测试概述

### 测试目标

- 验证 SquashFS 镜像解析的正确性
- 确保 FUSE 文件系统操作符合 POSIX 标准
- 测试边界条件和错误处理
- 验证性能满足使用需求

### 测试分类

| 类型 | 框架 | 位置 | 目的 |
|------|------|------|------|
| 单元测试 | CMocka | `tests/unit/` | 测试单个模块/函数 |
| 功能测试 | Bash/Python | `tests/functional/` | 端到端功能验证 |
| 性能测试 | fio + 自定义脚本 | `tests/performance/` | 性能基准测试 |

### 运行测试

```bash
# 运行所有测试
make test

# 只运行单元测试
make test_unit

# 只运行功能测试
make test_functional

# 运行性能测试
make test_performance

# 详细输出
ctest -V
```

---

## 单元测试

### 1. Superblock 测试 (`test_superblock.c`)

测试 SquashFS 超级块解析功能。

| 测试用例 | 描述 | 验证点 |
|----------|------|--------|
| `test_sb_magic_valid` | 有效魔数 | 0x73717368 ("hsqs") |
| `test_sb_magic_invalid` | 无效魔数 | 返回错误 |
| `test_sb_version_valid` | 有效版本 | 4.0 (major=4, minor=0) |
| `test_sb_version_invalid` | 无效版本 | 非 4.0 返回错误 |
| `test_sb_block_size_valid` | 有效块大小 | 4K, 8K, 16K, ..., 1M (2的幂) |
| `test_sb_block_size_invalid` | 无效块大小 | 非2的幂、超出范围 |
| `test_sb_compressor_gzip` | GZIP 压缩器 | ID=1 |
| `test_sb_compressor_zstd` | ZSTD 压缩器 | ID=6 |
| `test_sb_compressor_unsupported` | 不支持的压缩器 | LZMA(2), LZO(3) 等返回错误 |
| `test_sb_flags` | 标志位解析 | uncompressed, no_fragments, nfs_export 等 |
| `test_sb_compression_options` | 压缩选项 | 可选的压缩参数块解析 |

**参考：** `doc/squashfs.adoc` 第 197-283 行

### 2. Inode 测试 (`test_inode.c`)

测试所有 14 种 inode 类型的解析。

#### 基本类型 (1-7)

| 测试用例 | 类型 | 验证点 |
|----------|------|--------|
| `test_inode_basic_dir` | Basic Directory (1) | block_index, link_count, file_size, parent_inode |
| `test_inode_basic_file` | Basic File (2) | blocks_start, frag_index, block_offset, file_size, block_sizes[] |
| `test_inode_basic_symlink` | Basic Symlink (3) | link_count, target_size, target_path |
| `test_inode_basic_blkdev` | Basic Block Device (4) | link_count, device_number (major/minor) |
| `test_inode_basic_chrdev` | Basic Character Device (5) | link_count, device_number |
| `test_inode_basic_fifo` | Basic FIFO (6) | link_count |
| `test_inode_basic_socket` | Basic Socket (7) | link_count |

#### 扩展类型 (8-14)

| 测试用例 | 类型 | 验证点 |
|----------|------|--------|
| `test_inode_ext_dir` | Extended Directory (8) | 增加 index_count, xattr_index |
| `test_inode_ext_file` | Extended File (9) | 64位大小, 64位位置, sparse, link_count |
| `test_inode_ext_symlink` | Extended Symlink (10) | 增加 xattr_index |
| `test_inode_ext_blkdev` | Extended Block Device (11) | 增加 xattr_index |
| `test_inode_ext_chrdev` | Extended Character Device (12) | 增加 xattr_index |
| `test_inode_ext_fifo` | Extended FIFO (13) | 增加 xattr_index |
| `test_inode_ext_socket` | Extended Socket (14) | 增加 xattr_index |

#### 边界条件

| 测试用例 | 描述 |
|----------|------|
| `test_inode_header` | 公共头部解析 (type, permissions, uid, gid, mtime, inode_number) |
| `test_inode_cross_block` | 跨 metadata block 边界的 inode |
| `test_inode_type_invalid` | 无效 inode 类型 |

**参考：** `doc/squashfs.adoc` 第 512-777 行

### 3. Directory Table 测试 (`test_directory.c`)

测试目录表解析。

| 测试用例 | 描述 | 验证点 |
|----------|------|--------|
| `test_dir_header` | 目录头解析 | count (off-by-one), start, inode_number |
| `test_dir_entry` | 目录项解析 | offset, inode_offset, type, name_size, name |
| `test_dir_name_max_length` | 最大文件名长度 | 256 字符 |
| `test_dir_name_encoding` | 文件名编码 | off-by-one 编码 |
| `test_dir_empty` | 空目录 | file_size < 4 |
| `test_dir_index` | 目录索引 | 扩展目录的快速查找索引 |
| `test_dir_multiple_headers` | 多个头块 | 每个 header 最多 256 entries |

**参考：** `doc/squashfs.adoc` 第 779-884 行

### 4. Fragment Table 测试 (`test_fragment.c`)

测试碎片表解析。

| 测试用例 | 描述 | 验证点 |
|----------|------|--------|
| `test_frag_entry` | 碎片条目解析 | start (64位), size (32位), unused |
| `test_frag_uncompressed` | 未压缩标志 | bit 24 设置 |
| `test_frag_lookup` | 碎片索引查找 | 两级查找表结构 |
| `test_frag_table_size` | 表大小计算 | entries_per_block = 512 |
| `test_frag_invalid_index` | 无效索引 | 返回错误 |

**参考：** `doc/squashfs.adoc` 第 886-915 行

### 5. Export Table 测试 (`test_export.c`)

测试导出表解析（NFS export 支持）。

| 测试用例 | 描述 | 验证点 |
|----------|------|--------|
| `test_export_ref` | inode 引用 | 64位值 = (block_pos << 16) \| offset |
| `test_export_lookup` | 导出表查找 | index = inode_number - 1 |
| `test_export_block_size` | 每块条目数 | 1024 entries per block |
| `test_export_invalid_inode` | 无效 inode 号 | 返回错误 |
| `test_export_missing` | 无导出表 | superblock 字段为 0xFFFFFFFFFFFFFFFF |

**参考：** `doc/squashfs.adoc` 第 917-934 行

### 6. ID Table 测试 (`test_id.c`)

测试 UID/GID 查找表。

| 测试用例 | 描述 | 验证点 |
|----------|------|--------|
| `test_id_lookup` | ID 查找 | 16位索引 -> 32位 UID/GID |
| `test_id_block_size` | 每块条目数 | 2048 IDs per block |
| `test_id_invalid_index` | 无效索引 | 返回错误 |

**参考：** `doc/squashfs.adoc` 第 936-944 行

### 7. Metadata Block 测试 (`test_metadata.c`)

测试元数据块处理。

| 测试用例 | 描述 | 验证点 |
|----------|------|--------|
| `test_meta_header` | 16位头解析 | bit 15 = 未压缩, bits 0-14 = 大小 |
| `test_meta_size_valid` | 有效大小 | < 8KiB |
| `test_meta_size_invalid` | 无效大小 | >= 8KiB 返回错误 |
| `test_meta_uncompressed` | 未压缩块 | 直接读取 |
| `test_meta_compressed` | 压缩块 | 解压到 8KiB |
| `test_meta_cross_entry` | 跨块条目 | 条目跨越 block 边界 |
| `test_meta_ref` | 元数据引用 | 64位引用编码/解码 |

**参考：** `doc/squashfs.adoc` 第 96-128 行

### 8. Compressor 测试补充 (`test_compressor.c`)

补充现有压缩器测试。

| 测试用例 | 描述 | 验证点 |
|----------|------|--------|
| `test_comp_zstd_decompress` | ZSTD 解压 | 正确解压数据 |
| `test_comp_zlib_levels` | zlib 压缩级别 | 级别 1-9 都能正确解压 |
| `test_comp_zstd_levels` | zstd 压缩级别 | 级别 1-22 都能正确解压 |
| `test_comp_overflow_zstd` | zstd 缓冲区溢出 | 返回正确错误 |
| `test_comp_corrupt_zstd` | zstd 损坏数据 | 返回正确错误 |

**参考：** `doc/squashfs.adoc` 第 174-195 行

### 9. Xattr 测试 (`test_xattr.c`)

测试扩展属性解析。

| 测试用例 | 描述 | 验证点 |
|----------|------|--------|
| `test_xattr_key` | 键解析 | type, name_size, name |
| `test_xattr_value_inline` | 内联值 | value_size, value |
| `test_xattr_value_ref` | 引用值 | out-of-line 引用 |
| `test_xattr_prefix` | 命名空间前缀 | user(0), trusted(1), security(2) |
| `test_xattr_lookup` | 查找表 | 两级查找结构 |

**参考：** `doc/squashfs.adoc` 第 946-1067 行

---

## 功能测试

### 1. 基本操作测试 (`test_basic.sh`)

**状态：** ✅ 已实现

测试基本的挂载、目录遍历、文件读取功能。

### 2. 扩展属性测试 (`test_xattr.sh`)

**状态：** ✅ 已实现

测试 xattr 的读取和列表功能。

### 3. 错误处理测试 (`test_error_paths.sh`)

**状态：** ✅ 已实现

测试无效镜像、边界条件等错误处理。

### 4. 文件读取测试 (`test_read.py`)

**状态：** ✅ 已实现

测试顺序读取、随机访问、并发读取、边界条件。

### 5. 特殊文件类型测试 (`test_special_files.sh`)

**状态：** ✅ 已实现

测试块设备、字符设备、FIFO、Socket 的创建和识别。

### 6. 硬链接测试 (`test_hardlinks.sh`)

**状态：** ✅ 已实现

测试硬链接的 link_count、inode 一致性、跨目录硬链接。

### 7. 稀疏文件测试 (`test_sparse_files.sh`)

**状态：** ✅ 已实现

测试稀疏文件创建、空洞读取、尾部数据正确性。

### 8. 大文件测试 (`test_large_files.sh`)

**状态：** ✅ 已实现

测试接近 4GB 和超过 4GB 的文件、随机访问、文件末尾读取。

### 9. 深层目录测试 (`test_deep_directories.sh`)

**状态：** ✅ 已实现

测试 10 层和 50 层深目录、多分支目录、空目录。

### 10. 长文件名测试 (`test_long_filenames.sh`)

**状态：** ✅ 已实现

测试 100/200/255 字符文件名、长目录名、混合长度文件名。

### 11. 特殊字符文件名测试 (`test_special_chars.sh`)

**状态：** ✅ 已实现

测试空格、Unicode、特殊标点、混合特殊字符。

### 12. Zstd 压缩测试 (`test_zstd_compression.sh`)

**状态：** ✅ 已实现

测试 zstd 压缩、不同压缩级别、与 gzip 对比、大文件和多个小文件。

### 13. 不同块大小测试 (`test_block_sizes.sh`)

**状态：** ✅ 已实现

测试 4K、128K、512K、1M 块大小、碎片块、跨块文件。

### 14. 导出表功能测试 (`test_export_table.sh`)

**状态：** ✅ 已实现

测试带/不带导出表的镜像、大目录、硬链接、深层路径。

### 15. 数据去重测试 (`test_deduplication.sh`)

**状态：** ❌ 待实现

```bash
# 测试内容：
# - 创建包含重复内容的文件
# - 验证 mksquashfs 进行了去重（镜像大小）
# - 验证两个文件读取内容都正确
# - 验证它们可能共享相同的数据块
```

---

## 性能测试

性能测试详细方案见 `doc/performance.md`。

### 测试场景

| 场景 | 描述 | 指标 |
|------|------|------|
| 顺序读取 | 大文件顺序读取 | MB/s |
| 随机读取 | 随机位置小文件读取 | IOPS |
| 目录遍历 | 大量小文件目录 | 文件/秒 |
| 元数据操作 | stat, getattr 等 | 操作/秒 |
| 并发读取 | 多线程读取 | 总吞吐量 |

---

## 测试镜像管理

### 固定测试镜像

位于 `tests/fixtures/`，由脚本生成。

| 镜像 | 用途 | 创建脚本 |
|------|------|----------|
| `basic.sqfs` | 基本功能测试 | `create_test_images.sh` |
| `xattr.sqfs` | 扩展属性测试 | `create_test_images.sh` |
| `large.sqfs` | 大文件测试 | 动态生成 |
| `special.sqfs` | 特殊文件测试 | 动态生成 |

### 动态测试镜像

部分测试需要动态创建镜像以确保可重复性：

```bash
# 创建测试镜像示例
mksquashfs content/ test.sqfs -comp gzip -b 131072 -noappend

# 带导出表
mksquashfs content/ test.sqfs -comp zstd -exports -noappend

# 带扩展属性
mksquashfs content/ test.sqfs -comp gzip -noappend
```

---

## 测试覆盖率目标

### 代码覆盖率

| 模块 | 目标覆盖率 | 当前状态 |
|------|------------|----------|
| superblock.c | 90% | - |
| inode.c | 85% | - |
| directory.c | 85% | - |
| data.c | 80% | - |
| fragment.c | 80% | - |
| xattr.c | 75% | - |
| compressor.c | 90% | - |
| cache.c | 90% | ✅ |
| utils.c | 90% | ✅ |

### 功能覆盖率

- 所有 SquashFS inode 类型：14/14
- 所有支持的压缩器：2/2 (gzip, zstd)
- 所有 FUSE 操作：getattr, readdir, open, read, readlink, statfs, getxattr, listxattr

---

## 持续集成

CI 配置位于 `.github/workflows/build.yml`，每次 push 和 PR 自动运行：

1. 构建测试
2. 单元测试
3. 功能测试
4. 调试构建验证

---

## 测试开发指南

### 添加新单元测试

1. 在 `tests/unit/` 创建 `test_xxx.c`
2. 使用 CMocka 框架编写测试用例
3. 在 `CMakeLists.txt` 添加测试目标
4. 在本文档记录测试用例

### 添加新功能测试

1. 在 `tests/functional/` 创建测试脚本
2. 使用统一的日志和错误报告格式
3. 在 `CMakeLists.txt` 注册测试
4. 在本文档记录测试内容

### 测试脚本模板

```bash
#!/bin/bash
# test_xxx.sh - 测试描述
#
# 测试内容：
# - 测试项 1
# - 测试项 2

set -e

# 配置
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# 颜色输出
RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

# 日志函数
log_info() { echo -e "${GREEN}[INFO]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }

# 测试计数
TESTS_RUN=0
TESTS_PASSED=0
TESTS_FAILED=0

# ... 测试实现 ...

# 打印摘要
print_summary() {
    echo "Tests run: ${TESTS_RUN}"
    echo "Tests passed: ${TESTS_PASSED}"
    echo "Tests failed: ${TESTS_FAILED}"
    [ ${TESTS_FAILED} -eq 0 ]
}

print_summary
```

---

## 参考资料

- `doc/squashfs.adoc` - SquashFS 二进制格式规范
- `doc/design.md` - 架构设计文档
- `CONTRIBUTING.md` - 贡献指南