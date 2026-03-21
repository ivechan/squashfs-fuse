# SquashFS-FUSE 项目规范

## 规则

1. **所有计划必须保存到 `doc/plan.md`** - 任何实现计划、架构设计、任务规划都必须写入此文件，确保团队成员可以追踪项目进展。

## 设计规范

所有 Agent 必须遵循 `doc/design.md` 中的设计规范进行开发：
- 数据结构定义
- 模块接口定义
- 错误码定义
- 缓存策略
- 线程安全设计
- FUSE操作映射

## 编译方法

### 依赖项

```bash
# Ubuntu/Debian
sudo apt install libfuse3-dev libzstd-dev zlib1g-dev cmake gcc

# Fedora
sudo dnf install fuse3-devel libzstd-devel zlib-devel cmake gcc
```

### 编译步骤

```bash
# 使用 CMake 编译
mkdir -p build && cd build
cmake ..
make -j$(nproc)

# 或使用项目提供的 Makefile（简化版）
make
```

### 输出

编译成功后生成 `squashfs-fuse` 可执行文件。

## 测试方法

### 基本功能测试

```bash
# 1. 挂载测试镜像
./squashfs-fuse tests/fixtures/basic.sqfs /mnt/test

# 2. 验证基本操作
ls -la /mnt/test
cat /mnt/test/hello.txt
stat /mnt/test/hello.txt
ls -la /mnt/test/subdir
cat /mnt/test/subdir/test.txt

# 3. 卸载
fusermount -u /mnt/test
```

### 调试模式

```bash
# 前台运行并显示调试信息
./squashfs-fuse tests/fixtures/basic.sqfs /mnt/test -f -d

# 查看日志
tail -f /tmp/squashfs-fuse.log
```

### 创建测试镜像

```bash
# 使用 mksquashfs 创建带导出表的镜像
mkdir -p test_content/subdir
echo "Hello, World!" > test_content/hello.txt
echo "Test content" > test_content/subdir/test.txt
mksquashfs test_content test.sqfs -comp gzip -b 131072 -exports -noappend

# 验证镜像
unsquashfs -l test.sqfs
```

## 文档索引

| 文档 | 说明 |
|------|------|
| `doc/design.md` | 系统设计规范 |
| `doc/plan.md` | 实现计划与进度 |
| `doc/logging.md` | 日志系统设计 |
| `doc/debug.md` | 问题定位与调试经验 |