# 贡献指南

感谢您有兴趣为 SquashFS-FUSE 做出贡献！

## 如何贡献

### 报告问题

如果您发现了 bug 或有功能建议，请通过 [GitHub Issues](https://github.com/ivechan/squashfs-fuse/issues) 提交。

提交问题时请包含：

1. **问题描述**：清晰简洁地描述问题
2. **重现步骤**：如何重现问题
3. **预期行为**：您期望发生什么
4. **实际行为**：实际发生了什么
5. **环境信息**：
   - 操作系统和版本
   - SquashFS-FUSE 版本
   - 压缩算法（gzip/zstd）
6. **日志**：如有相关日志，请附上（`/tmp/squashfs-fuse.log`）

### 提交代码

#### 1. Fork 并克隆仓库

```bash
git clone https://github.com/YOUR_USERNAME/squashfs-fuse.git
cd squashfs-fuse
```

#### 2. 创建分支

```bash
git checkout -b feature/your-feature-name
# 或
git checkout -b fix/your-bug-fix
```

#### 3. 进行更改

- 编写代码
- 添加测试
- 确保所有测试通过

#### 4. 提交更改

我们遵循约定式提交规范：

```
<类型>(<范围>): <描述>

[可选的正文]

[可选的脚注]
```

**类型：**
- `feat`: 新功能
- `fix`: Bug 修复
- `docs`: 文档更新
- `style`: 代码格式（不影响功能）
- `refactor`: 代码重构
- `test`: 添加或修改测试
- `chore`: 构建/工具变更

**示例：**
```
feat(data): add support for sparse file reading

Implement sparse file handling by tracking sparse regions
in the inode structure and returning zeros for holes.

Closes #42
```

#### 5. 推送并创建 Pull Request

```bash
git push origin feature/your-feature-name
```

然后在 GitHub 上创建 Pull Request。

## 开发设置

### 依赖项

```bash
# Ubuntu/Debian
sudo apt install libfuse3-dev libzstd-dev zlib1g-dev cmake gcc

# 开发依赖（用于测试）
sudo apt install squashfs-tools python3 libcmocka-dev attr
```

### 编译

```bash
mkdir -p build && cd build
cmake ..
make -j$(nproc)
```

### 运行测试

```bash
# 运行所有测试
make test

# 运行功能测试
make test_functional

# 详细输出
ctest -V

# 运行单个测试
./tests/functional/test_basic.sh -f ./build/squashfs-fuse -v
```

### 调试构建

```bash
cmake -DSQFS_ENABLE_DEBUG_LOG=ON ..
make -j$(nproc)
```

## 代码规范

### C 代码风格

- 使用 C11 标准
- 缩进：4 个空格（不使用制表符）
- 行宽：尽量不超过 100 字符
- 函数：使用 snake_case 命名
- 常量和宏：使用 UPPER_SNAKE_CASE
- 注释：使用 `/* */` 风格

### 文件结构

```
src/
├── module.c      # 实现
├── module.h      # 头文件
```

### 头文件保护

```c
#ifndef SQFS_MODULE_H
#define SQFS_MODULE_H

/* ... */

#endif /* SQFS_MODULE_H */
```

### 错误处理

使用 `src/utils.h` 中定义的错误码：

```c
int ret = some_function();
if (ret != SQFS_OK) {
    return ret;  // 返回适当的错误码
}
```

### 内存管理

- 使用 `sqfs_malloc`、`sqfs_calloc`、`sqfs_free` 等包装函数
- 每个分配都应有对应的释放
- 避免内存泄漏

### 日志

```c
#include "log.h"

SQFS_LOG_INFO(SQFS_MOD_DATA, "Reading %zu bytes from offset %lu", size, offset);
SQFS_LOG_ERROR(SQFS_MOD_INODE, "Failed to parse inode: %d", ret);
SQFS_LOG_DEBUG(SQFS_MOD_FUSE, "Operation completed");
```

## 测试规范

### 单元测试

- 测试文件放在 `tests/unit/`
- 使用 CMocka 框架
- 每个模块应有对应的测试文件

### 功能测试

- 测试脚本放在 `tests/functional/`
- 使用 Bash 脚本
- 测试应可独立运行

### 测试覆盖率

新功能必须包含测试。Bug 修复应包含回归测试。

## 文档

### 代码注释

- 公共 API 必须有注释
- 复杂逻辑应有解释
- 避免无意义的注释

### 文档更新

如果您的更改影响：

- 用户界面（命令行参数）
- API 变更
- 架构设计

请更新相应文档：
- `README.md` - 用户文档
- `doc/design.md` - 设计文档
- `CLAUDE.md` - 开发指南

## 代码审查

所有 PR 都需要代码审查。请：

1. 响应审查意见
2. 进行必要的修改
3. 保持讨论专业和友好

## 许可证

通过贡献代码，您同意您的贡献将根据 GPL-2.0-or-later 许可证授权。

## 问题？

如有任何问题，请：
- 在 GitHub Issues 中提问
- 查阅现有文档：`doc/` 目录