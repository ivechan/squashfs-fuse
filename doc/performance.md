# SquashFS-FUSE 性能测试设计方案

## 背景

本文档定义 SquashFS-FUSE 与 Linux 内核原生 SquashFS 模块的性能对比测试方案，用于评估用户态 FUSE 实现的性能开销。

**目的**:
1. 量化 FUSE 实现相对于内核模块的性能差距
2. 识别性能瓶颈，指导后续优化
3. 建立可重复的性能基准测试流程

**测试范围**:
- 连续读、随机读、元数据操作、并发读取
- 不同压缩算法 (gzip/zstd) 和块大小 (4K-1M)
- 最大测试文件: 100MB

## 测试目标

1. **连续读性能** - 评估大文件顺序读取吞吐量
2. **随机读性能** - 评估随机 I/O 操作的处理能力
3. **元数据性能** - 评估 inode 查找、目录遍历等操作
4. **并发性能** - 评估多线程并发读取的扩展性

## 测试维度

| 维度 | 配置项 |
|------|--------|
| 压缩算法 | gzip, zstd |
| 块大小 | 4K, 64K, 128K, 1M |
| 文件大小 | 小文件 (≤4K, 碎片), 中等 (1M-100M), 大文件 (≥100M) |
| 并发数 | 1, 2, 4, 8 线程 |
| 缓存策略 | 冷缓存 (每次测试前清除) |

## 测试环境要求

### 软件依赖

```bash
# 测试工具
sudo apt install fio         # I/O 基准测试
sudo apt install bc          # 数学计算
sudo apt install jq          # JSON 处理

# SquashFS 工具
sudo apt install squashfs-tools  # mksquashfs, unsquashfs

# 内核模块 (用于对比)
sudo modprobe squashfs
sudo modprobe loop
```

### 权限说明

- **Kernel SquashFS 挂载**: 不需要 sudo，只要挂载点目录有写权限即可
- **缓存清理**: 需要 sudo 执行 `echo 3 > /proc/sys/vm/drop_caches`

### 系统配置

```bash
# 允许非 root 用户使用 fio direct I/O
sudo sysctl -w fs.protected_regular=0

# 禁用透明大页 (减少干扰)
echo never | sudo tee /sys/kernel/mm/transparent_hugepage/enabled
```

## 运行测试

### 快速测试

```bash
# 编译项目
mkdir -p build && cd build
cmake .. && make -j$(nproc)

# 创建测试镜像
../scripts/create_perf_images.sh

# 运行性能测试
../tests/performance/run_benchmark.sh -o results.md
```

### 详细测试

```bash
# 使用特定镜像
./tests/performance/run_benchmark.sh -i tests/fixtures/perf/perf_zstd_bs128k.sqfs -o zstd_results.md

# 快速模式 (5秒每测试)
./tests/performance/run_benchmark.sh --quick -o quick_results.md
```

## 测试镜像准备

使用固定种子的伪随机数据生成可重现的测试镜像：

```bash
# scripts/create_perf_images.sh

# 小文件 (碎片测试)
for i in $(seq 1 100); do
  echo "Small file $i content" > "content/small_${i}.txt"
done

# 中等文件 (1MB - 100MB)
dd if=/dev/urandom bs=1M count=1 | openssl enc -aes-256-ctr -pass pass:42 -nosalt > content/medium_1m.bin
dd if=/dev/urandom bs=1M count=10 | openssl enc -aes-256-ctr -pass pass:42 -nosalt > content/medium_10m.bin
dd if=/dev/urandom bs=1M count=100 | openssl enc -aes-256-ctr -pass pass:42 -nosalt > content/large_100m.bin

# 创建不同配置的镜像
for comp in gzip zstd; do
  for bs in 4096 65536 131072 1048576; do
    mksquashfs content perf_${comp}_bs${bs}.sqfs -comp ${comp} -b ${bs} -noappend -no-xattrs
  done
done
```

## 测试方案

### 1. 连续读性能测试

**测试命令**: fio sequential read

```ini
[global]
ioengine=sync
direct=1
time_based
runtime=30
group_reporting

[sequential-read]
filename=large_100m.bin
rw=read
bs=1M
```

**测试矩阵**:
- 文件: medium_1m.bin, medium_10m.bin, large_100m.bin
- 块大小: 4K, 64K, 128K, 1M
- 压缩: gzip, zstd
- 实现: FUSE, Kernel

**指标**: 吞吐量 (MB/s)

### 2. 随机读性能测试

**测试命令**: fio random read

```ini
[global]
ioengine=sync
direct=1
time_based
runtime=30
group_reporting

[random-read-4k]
rw=randread
bs=4k
iodepth=1
```

**测试矩阵**:
- 文件: medium_10m.bin, large_100m.bin
- 随机块大小: 4K, 64K
- 实现: FUSE, Kernel

**指标**: IOPS, 平均延迟 (μs)

### 3. 元数据性能测试

**测试项目**:

| 测试项 | 方法 | 指标 |
|--------|------|------|
| stat() 性能 | 循环 stat 同一文件 10000 次 | ops/s |
| readdir() 性能 | 循环列出目录 1000 次 | ops/s |
| 路径解析 | 遍历深层次目录结构 | ops/s |

### 4. 并发读取测试

**测试命令**: fio 多线程随机读

```ini
[global]
ioengine=sync
direct=1
time_based
runtime=30
group_reporting

[randread-1thread]
rw=randread
bs=4k
numjobs=1
iodepth=1

[randread-4thread]
rw=randread
bs=4k
numjobs=4
iodepth=1
```

**指标**: 总 IOPS, 扩展性比率

## 挂载方式对比

### FUSE 挂载

```bash
# 挂载
./build/squashfs-fuse -f tests/fixtures/perf/perf_gzip_bs128k.sqfs /mnt/fuse_test &

# 等待挂载完成
while ! mountpoint -q /mnt/fuse_test; do sleep 0.1; done

# 卸载
fusermount3 -u /mnt/fuse_test
```

### Kernel 挂载

```bash
# 挂载 (不需要 sudo，挂载点需要有写权限)
mount -t squashfs -o loop tests/fixtures/perf/perf_gzip_bs128k.sqfs /mnt/kernel_test

# 卸载
umount /mnt/kernel_test
```

## 输出格式

测试结果以 Markdown 表格形式输出：

```markdown
# SquashFS-FUSE 性能测试报告

## 测试环境
- 内核版本: Linux 6.6.87
- 压缩算法: gzip
- 块大小: 128K

## 连续读性能 (MB/s)

| 实现 | 吞吐量 (MB/s) |
|------|---------------|
| FUSE | 200 |
| Kernel | 540 |
| 性能比 | 37% |

## 随机读性能 (4K IOPS)

| 实现 | IOPS |
|------|------|
| FUSE | 1200 |
| Kernel | 8500 |
| 性能比 | 14% |

## 元数据性能 (ops/s)

| 操作 | FUSE | Kernel | 性能比 |
|------|------|--------|--------|
| stat | 8500 | 45000 | 19% |
| readdir | 3200 | 18000 | 18% |

## 并发性能 (IOPS)

| 线程数 | FUSE | Kernel | 性能比 |
|--------|------|--------|--------|
| 1 | 1200 | 8500 | 14% |
| 2 | 2100 | 16000 | 13% |
| 4 | 3800 | 31000 | 12% |
| 8 | 6500 | 58000 | 11% |
```

## 注意事项

1. **权限要求**: 缓存清理需要 sudo 权限 (`echo 3 > /proc/sys/vm/drop_caches`)
2. **挂载点**: Kernel 挂载不需要 sudo，但挂载点目录需要有写权限
3. **缓存清理**: 每次测试前必须清除缓存确保冷启动
4. **系统干扰**: 测试时关闭其他 I/O 密集型进程
5. **多次运行**: 每个测试运行 3 次取平均值

## 文件结构

```
tests/performance/
├── run_benchmark.sh      # 主测试脚本
├── benchmark_lib.sh      # 公共函数库
└── fio_configs/
    ├── sequential_read.fio
    ├── random_read.fio
    └── concurrent_read.fio

scripts/
└── create_perf_images.sh # 测试镜像生成脚本
```