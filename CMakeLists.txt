cmake_minimum_required(VERSION 3.10)
project(squashfs-fuse C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# 编译选项
add_compile_options(-Wall -Wextra -Werror)

# 查找依赖
find_package(PkgConfig REQUIRED)
pkg_check_modules(FUSE3 REQUIRED fuse3)
pkg_check_modules(ZLIB REQUIRED zlib)
pkg_check_modules(ZSTD REQUIRED libzstd)

# Debug logging option
option(SQFS_ENABLE_DEBUG_LOG "Enable debug level logging" OFF)

if(SQFS_ENABLE_DEBUG_LOG)
    add_compile_definitions(SQFS_LOG_LEVEL=4)  # DEBUG
else()
    add_compile_definitions(SQFS_LOG_LEVEL=2)  # WARN
endif()

# 源文件
set(SOURCES
    src/main.c
    src/superblock.c
    src/inode.c
    src/directory.c
    src/data.c
    src/fragment.c
    src/xattr.c
    src/compressor.c
    src/cache.c
    src/utils.c
    src/log.c
    src/stats.c
)

# 可执行文件
add_executable(squashfs-fuse ${SOURCES})

# 链接库
target_include_directories(squashfs-fuse PRIVATE
    ${FUSE3_INCLUDE_DIRS}
    ${ZLIB_INCLUDE_DIRS}
    ${ZSTD_INCLUDE_DIRS}
)
target_link_libraries(squashfs-fuse
    ${FUSE3_LIBRARIES}
    ${ZLIB_LIBRARIES}
    ${ZSTD_LIBRARIES}
)

# 安装
install(TARGETS squashfs-fuse DESTINATION bin)