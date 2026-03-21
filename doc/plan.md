# SquashFS-FUSE 文件系统实现计划

> **所有 Agent 必须遵循 `doc/design.md` 中的设计规范**
>
> **维测日志系统设计见 `doc/logging.md`**

## 设计决策 (已确认)

| 决策项 | 选择 |
|--------|------|
| 编程语言 | C语言 |
| FUSE版本 | FUSE3 |
| 扩展属性 | 完整xattr支持 |
| 错误处理 | 严格模式 (遇到损坏立即返回错误) |
| 并发模型 | 多线程 |
| NFS Export | 不支持 |
| 缓存层 | Inode缓存 + 目录缓存 + 元数据块缓存 + 数据块缓存 |
| 挂载选项 | ro, debug=N |
| 大文件读取 | 流式读取 |
| 稀疏文件 | 完整支持空洞处理 |

## 项目架构

```
squashfs-fuse/
├── CMakeLists.txt
├── src/
│   ├── main.c              # 入口点和FUSE初始化
│   ├── superblock.c        # Superblock解析
│   ├── superblock.h
│   ├── inode.c             # Inode解析和管理
│   ├── inode.h
│   ├── directory.c         # 目录表解析
│   ├── directory.h
│   ├── data.c              # 数据块读取和解压
│   ├── data.h
│   ├── fragment.c          # Fragment表处理
│   ├── fragment.h
│   ├── compressor.c        # 压缩抽象层(zstd/zlib)
│   ├── compressor.h
│   ├── xattr.c             # 扩展属性
│   ├── xattr.h
│   ├── cache.c             # 缓存层
│   ├── cache.h
│   └── utils.c             # 工具函数
│   └── utils.h
├── tests/
│   ├── functional/         # 功能测试
│   │   ├── test_basic.sh
│   │   ├── test_read.py
│   │   ├── test_directory.py
│   │   └── test_xattr.py
│   ├── performance/        # 性能测试
│   │   ├── benchmark_read.sh
│   │   └── benchmark_random.sh
│   └── fixtures/           # 测试镜像生成脚本
│       └── create_test_images.sh
└── doc/
    └── squashfs.adoc       # 现有文档
```

## Code Agent 团队设计

### Agent 1: 架构设计师 (Architect)
**职责**: 设计整体架构、数据结构定义、模块接口
**输出**: 头文件、数据结构定义、接口规范

### Agent 2: 核心解析开发者 (Core Parser)
**职责**: 实现superblock、inode、directory table解析
**输出**: superblock.c, inode.c, directory.c

### Agent 3: 数据层开发者 (Data Layer)
**职责**: 实现数据块读取、压缩解压、fragment处理
**输出**: data.c, fragment.c, compressor.c

### Agent 4: FUSE集成开发者 (FUSE Integration)
**职责**: 实现FUSE操作回调、文件系统挂载
**输出**: main.c, FUSE操作实现

### Agent 5: 测试工程师 (Test Engineer)
**职责**: 设计功能测试和性能测试
**输出**: 测试脚本、测试镜像生成器

### Agent 6: 缓存优化工程师 (Cache Optimizer)
**职责**: 实现元数据缓存、数据块缓存
**输出**: cache.c, 缓存策略

## 实现步骤

### Phase 1: 基础框架 (Agent 1 + Agent 4)
1. 创建CMakeLists.txt构建配置
2. 定义核心数据结构(superblock.h, inode.h)
3. 实现基本FUSE框架(main.c)
4. 验证: 编译通过，空挂载成功

### Phase 2: Superblock和压缩 (Agent 2 + Agent 3)
1. 实现superblock解析(superblock.c)
2. 实现压缩抽象层(compressor.c)
   - zlib解压
   - zstd解压
3. 验证: 能正确读取superblock信息

### Phase 3: Inode和目录 (Agent 2)
1. 实现inode解析(inode.c)
   - 基本inode类型
   - 扩展inode类型
2. 实现目录表解析(directory.c)
3. 验证: 能列出根目录内容

### Phase 4: 数据读取 (Agent 3)
1. 实现数据块读取(data.c)
2. 实现fragment处理(fragment.c)
3. 验证: 能读取文件内容

### Phase 5: FUSE完整集成 (Agent 4)
1. 实现所有FUSE操作
   - getattr, readdir, open, read
   - statfs, readlink等
2. 实现xattr支持
3. 验证: 完整文件系统操作

### Phase 6: 缓存优化 (Agent 6)
1. 元数据缓存(inode, directory)
2. 数据块缓存
3. LRU策略实现
4. 验证: 性能提升

### Phase 7: 测试 (Agent 5)
1. 功能测试脚本
   - 创建各种squashfs镜像
   - 测试读取、目录遍历、符号链接等
2. 性能测试
   - 顺序读取性能
   - 随机读取性能
   - 与原生文件系统对比

## 关键数据结构

```c
// Superblock结构 (96字节)
typedef struct {
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

// Inode类型枚举
typedef enum {
    SQUASHFS_DIR_TYPE = 1,
    SQUASHFS_FILE_TYPE = 2,
    SQUASHFS_SYMLINK_TYPE = 3,
    SQUASHFS_BLKDEV_TYPE = 4,
    SQUASHFS_CHRDEV_TYPE = 5,
    SQUASHFS_FIFO_TYPE = 6,
    SQUASHFS_SOCKET_TYPE = 7,
    SQUASHFS_LDIR_TYPE = 8,
    SQUASHFS_LREG_TYPE = 9,
    // ...
} squashfs_inode_type_t;
```

## 测试镜像生成

使用mksquashfs创建测试镜像:

```bash
# 基本测试镜像
mkdir -p test_dir/{subdir1,subdir2}
echo "Hello World" > test_dir/file.txt
mksquashfs test_dir basic.sqfs -comp zstd

# 大文件测试
dd if=/dev/urandom of=large.bin bs=1M count=100
mksquashfs large.bin large.sqfs -comp zlib
```

## 验证方法

1. **编译验证**: `cmake . && make`
2. **挂载测试**: `./squashfs-fuse test.sqfs /mnt/test -f`
3. **功能验证**:
   - `ls -la /mnt/test`
   - `cat /mnt/test/file.txt`
   - `stat /mnt/test/file.txt`
4. **性能验证**:
   - `dd if=/mnt/test/large.bin of=/dev/null`
   - `fio --name=random-read --filename=/mnt/test/large.bin --rw=randread`

## 依赖项

- libfuse3-dev
- libzstd-dev
- zlib1g-dev
- cmake
- gcc/clang
- mksquashfs/unsquashfs (测试用)