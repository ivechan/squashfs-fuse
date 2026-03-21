# SquashFS-FUSE 调试指南

本文档记录了在开发 SquashFS-FUSE 过程中遇到的问题及其解决方案，供后续开发和调试参考。

## 目录

1. [调试方法论](#调试方法论)
2. [常见问题与解决方案](#常见问题与解决方案)
3. [工具与技巧](#工具与技巧)
4. [日志系统使用](#日志系统使用)

---

## 调试方法论

### 1. 问题定位流程

```
发现问题 → 复现问题 → 分析日志 → 定位代码 → 验证假设 → 修复测试
```

### 2. 关键步骤

1. **阅读错误信息**：不要跳过错误信息，它们通常包含解决方案
2. **检查最近变更**：使用 `git diff` 查看最近的修改
3. **添加诊断日志**：在关键路径添加日志输出
4. **逐层排查**：从外层接口向内层实现逐步排查
5. **验证假设**：使用 Python 等工具验证数据格式

### 3. 调试工具

- **Python**：快速验证数据结构和格式
- **hexdump/xxd**：查看二进制数据
- **strace**：追踪系统调用
- **FUSE debug 模式**：`-f -d` 参数启用调试输出

---

## 常见问题与解决方案

### 问题 1：FUSE 参数传递错误

**症状**：
```
fuse: unknown option(s): '-o foreground'
```

**原因**：FUSE 低级 API 和高级 API 参数处理方式不同

**解决方案**：
使用 `fuse_main()` 而不是 `fuse_new()`，并正确构建参数列表：
```c
// 正确做法：只传递挂载点和 FUSE 选项，不传递镜像文件
fuse_opt_add_arg(&args, argv[0]);  /* 程序名 */
fuse_opt_add_arg(&args, mount_point);
fuse_opt_add_arg(&args, "-oro");
fuse_main(args.argc, args.argv, &sqfs_oper, NULL);
```

---

### 问题 2：元数据块解压失败

**症状**：
```
Metadata decompression failed: -3
```

**原因**：SquashFS 使用原始 deflate 格式，但某些实现使用带 zlib 头的格式

**诊断方法**：
```python
import zlib
# 检查数据头
if src_bytes[0] == 0x78 and src_bytes[1] in (0x01, 0x5e, 0x9c, 0xda):
    print("zlib 格式")
else:
    print("原始 deflate 格式")
```

**解决方案**：
在解压器中自动检测格式：
```c
if (src_size >= 2 && src_bytes[0] == 0x78 &&
    (src_bytes[1] == 0x01 || src_bytes[1] == 0x5e ||
     src_bytes[1] == 0x9c || src_bytes[1] == 0xda)) {
    window_bits = 15;  /* zlib 格式 */
} else {
    window_bits = -15; /* 原始 deflate */
}
ret = inflateInit2(&strm, window_bits);
```

---

### 问题 3：导出表读取失败

**症状**：
```
Invalid metadata block size: 11172
```

**原因**：导出表是两级结构，不能直接当作元数据块读取

**SquashFS 导出表结构**：
```
export_table (superblock field)
    │
    ▼
┌─────────────────────────────────┐
│ 查找表 (Lookup Table)            │
│ - 64-bit 指针数组                │
│ - 每个指针指向一个元数据块        │
└─────────────────────────────────┘
    │
    ▼
┌─────────────────────────────────┐
│ 元数据块                         │
│ - 最多 1024 个 inode 引用         │
│ - 每个 64-bit: block_pos + offset │
└─────────────────────────────────┘
```

**诊断方法**：
```python
# 读取查找表
f.seek(export_table)
ptr = struct.unpack('<Q', f.read(8))[0]

# 读取元数据块
f.seek(ptr)
header = struct.unpack('<H', f.read(2))[0]
# 然后解压读取 inode 引用
```

**解决方案**：
```c
// 1. 计算块索引和块内偏移
uint64_t block_index = (inode_num - 1) / 1024;
uint64_t index_in_block = (inode_num - 1) % 1024;

// 2. 读取查找表获取元数据块地址
uint64_t lookup_offset = export_table + block_index * sizeof(uint64_t);
uint64_t meta_block_addr;
pread(fd, &meta_block_addr, sizeof(meta_block_addr), lookup_offset);

// 3. 读取并解压元数据块
sqfs_meta_read_block(fd, meta_block_addr, buffer, &size, comp);

// 4. 从元数据块中读取 inode 引用
memcpy(&inode_ref, buffer + index_in_block * 8, 8);
```

---

### 问题 4：文件读取返回零或 I/O 错误

**症状**：
- `cat file` 显示空白或零
- 或返回 `Input/output error`

**诊断步骤**：

1. 检查日志中的 `open` 和 `read` 调用：
```bash
grep -E '"open"|"read"' /tmp/squashfs-fuse.log
```

2. 确认操作是否到达 FUSE 层：
```bash
./squashfs-fuse image.sqfs /mnt -f -d 2>&1 | grep -E "open|read"
```

**可能原因及解决方案**：

| 原因 | 解决方案 |
|------|----------|
| `sqfs_data_read` 的 ops 未初始化 | 确保 `ops.fd`, `ops.sb`, `ops.comp` 正确设置 |
| 碎片块读取未实现 | 实现 `sqfs_fragment_read` 调用 |
| Fragment table 未加载 | 在初始化时调用 `sqfs_fragment_table_load` |
| Context 结构不一致 | 统一使用 `context.h` 中的定义 |

---

### 问题 5：链接数显示为 0

**症状**：
```
stat file.txt
Links: 0
```

**原因**：基本文件 inode 不存储 `link_count` 字段

**解决方案**：
在解析基本文件 inode 时设置默认值：
```c
static int parse_inode_file(...) {
    // ...
    inode->link_count = 1;  /* 基本文件 inode 默认为 1 */
    // ...
}
```

---

### 问题 6：Segmentation Fault

**诊断步骤**：

1. 检查指针是否为 NULL：
```c
if (!ctx || !ctx->sb || !ctx->comp) {
    return SQFS_ERR_CORRUPT;
}
```

2. 检查结构体定义是否一致：
- 确保 `src/context.h` 被所有模块正确包含
- 避免在多个文件中重复定义结构体

3. 使用调试器：
```bash
gdb --args ./squashfs-fuse image.sqfs /mnt -f
(gdb) run
(gdb) bt  # 崩溃后查看调用栈
```

---

## 工具与技巧

### Python 快速验证脚本

```python
#!/usr/bin/env python3
"""验证 SquashFS 镜像结构"""
import struct
import zlib

def analyze_squashfs(filename):
    with open(filename, 'rb') as f:
        # 读取超级块
        f.seek(0)
        sb = f.read(96)

        magic, inode_count = struct.unpack('<II', sb[0:8])
        block_size = struct.unpack('<I', sb[12:16])[0]

        # 表位置
        inode_table = struct.unpack('<Q', sb[72:80])[0]
        dir_table = struct.unpack('<Q', sb[80:88])[0]
        frag_table = struct.unpack('<Q', sb[88:96])[0]
        export_table = struct.unpack('<Q', sb[104:112])[0]

        print(f"Magic: {hex(magic)}")
        print(f"Inodes: {inode_count}")
        print(f"Block size: {block_size}")
        print(f"Inode table: {hex(inode_table)}")
        print(f"Dir table: {hex(dir_table)}")
        print(f"Frag table: {hex(frag_table)}")
        print(f"Export table: {hex(export_table)}")

        # 读取元数据块头
        if inode_table != 0xFFFFFFFFFFFFFFFF:
            f.seek(inode_table)
            header = struct.unpack('<H', f.read(2))[0]
            print(f"\nInode table header: {hex(header)}")
            print(f"  Uncompressed: {bool(header & 0x8000)}")
            print(f"  Size: {header & 0x7FFF}")

if __name__ == '__main__':
    import sys
    analyze_squashfs(sys.argv[1])
```

### FUSE 调试模式

```bash
# 前台运行并显示 FUSE 调试信息
./squashfs-fuse image.sqfs /mnt -f -d

# 查看操作序列
# 输出示例：
# unique: 6, opcode: LOOKUP (1), nodeid: 1, insize: 50
# LOOKUP /hello.txt
#    NODEID: 2
# unique: 8, opcode: OPEN (14), nodeid: 2, insize: 48
# open flags: 0x8000 /hello.txt
```

### 日志分析

```bash
# 查看特定模块的日志
grep '"module":"inode"' /tmp/squashfs-fuse.log

# 查看错误日志
grep '"level":"ERROR"' /tmp/squashfs-fuse.log

# 查看特定函数的调用
grep 'sqfs_meta_read_block' /tmp/squashfs-fuse.log
```

---

## 日志系统使用

### 日志级别

```c
SQFS_LOG_ERROR   // 错误信息，始终输出
SQFS_LOG_WARN    // 警告信息
SQFS_LOG_INFO    // 一般信息
SQFS_LOG_DEBUG   // 调试信息（需要编译时启用）
```

### 模块定义

```c
SQFS_MOD_SUPERBLOCK  // 超级块
SQFS_MOD_INODE       // Inode 操作
SQFS_MOD_DIRECTORY   // 目录操作
SQFS_MOD_DATA        // 数据读取
SQFS_MOD_FRAGMENT    // 碎片操作
SQFS_MOD_COMPRESSOR  // 压缩器
SQFS_MOD_CACHE       // 缓存
SQFS_MOD_FUSE        // FUSE 接口
```

### 使用示例

```c
// 简单日志
SQFS_LOG_INFO(SQFS_MOD_FUSE, "Mounting filesystem");

// 格式化日志
SQFS_LOG_DEBUG(SQFS_MOD_INODE, "Loading inode %lu", inode_num);

// 错误日志
SQFS_LOG_ERROR(SQFS_MOD_DATA, "Failed to read block at %lu", pos);

// 带数据的日志（JSON 格式）
SQFS_LOG_DATA_DEBUG(SQFS_MOD_INODE, "Inode details",
    "{\"type\":%d,\"size\":%lu}", type, size);
```

### 日志配置

```bash
# 日志文件位置
/tmp/squashfs-fuse.log

# 最大文件大小（字节）
# 默认：10MB
#define SQFS_LOG_DEFAULT_MAX_SIZE (10 * 1024 * 1024)
```

---

## 检查清单

当遇到问题时，按以下顺序检查：

- [ ] 日志中是否有错误信息？
- [ ] FUSE 调试模式显示了什么？
- [ ] 数据结构是否正确对齐和初始化？
- [ ] Context 结构是否在所有模块中一致？
- [ ] 是否正确处理了大小端转换？
- [ ] 压缩数据格式是否正确识别？
- [ ] 表查找是否正确理解了两级结构？
- [ ] 边界检查是否完备？

---

## 参考资料

- [SquashFS 格式规范](https://github.com/plougher/squashfs-tools/blob/master/README-4.6)
- [FUSE 文档](https://libfuse.github.io/doxygen/)
- 项目设计文档: `doc/design.md`
- 项目计划: `doc/plan.md`