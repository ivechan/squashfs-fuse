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

# ============================================================================
# Testing
# ============================================================================

option(BUILD_TESTS "Build tests" ON)

if(BUILD_TESTS)
    enable_testing()

    # Find CMocka for unit testing
    find_package(CMocka QUIET)

    if(CMocka_FOUND)
        # Test sources
        set(UNIT_TEST_SOURCES
            tests/unit/test_cache.c
            tests/unit/test_compressor.c
            tests/unit/test_utils.c
            tests/unit/test_superblock.c
            tests/unit/test_inode.c
            tests/unit/test_directory.c
            tests/unit/test_fragment.c
            tests/unit/test_metadata.c
            tests/unit/test_export.c
        )

        # Common sources for tests (exclude main.c)
        set(TEST_LIB_SOURCES
            src/cache.c
            src/compressor.c
            src/utils.c
            src/superblock.c
            src/log.c
            src/stats.c
            src/inode.c
            src/directory.c
            src/fragment.c
            src/data.c
        )

        # Include directories for tests
        set(TEST_INCLUDE_DIRS
            ${CMAKE_SOURCE_DIR}/src
            ${ZLIB_INCLUDE_DIRS}
            ${ZSTD_INCLUDE_DIRS}
        )

        # Build unit test executables
        foreach(TEST_SOURCE ${UNIT_TEST_SOURCES})
            get_filename_component(TEST_NAME ${TEST_SOURCE} NAME_WE)

            add_executable(${TEST_NAME} ${TEST_SOURCE} ${TEST_LIB_SOURCES})

            target_include_directories(${TEST_NAME} PRIVATE ${TEST_INCLUDE_DIRS})
            target_link_libraries(${TEST_NAME} ${CMOCKA_LIBRARIES} ${ZLIB_LIBRARIES} ${ZSTD_LIBRARIES} m)
            target_include_directories(${TEST_NAME} PRIVATE ${CMOCKA_INCLUDE_DIRS})

            # Add unit test with label
            add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
            set_tests_properties(${TEST_NAME} PROPERTIES LABELS "unit")
        endforeach()

        message(STATUS "Unit tests enabled (CMocka found)")
    else()
        message(STATUS "Unit tests disabled (CMocka not found - install libcmocka-dev)")
    endif()

    # Functional tests (shell scripts)
    find_program(BASH_PROGRAM bash)

    if(BASH_PROGRAM)
        # Basic functional test
        add_test(
            NAME functional_basic
            COMMAND ${BASH_PROGRAM} ${CMAKE_SOURCE_DIR}/tests/functional/test_basic.sh -f $<TARGET_FILE:squashfs-fuse>
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        )
        set_tests_properties(functional_basic PROPERTIES
            TIMEOUT 120
            LABELS "functional"
        )

        # Xattr test
        add_test(
            NAME functional_xattr
            COMMAND ${BASH_PROGRAM} ${CMAKE_SOURCE_DIR}/tests/functional/test_xattr.sh -f $<TARGET_FILE:squashfs-fuse>
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        )
        set_tests_properties(functional_xattr PROPERTIES
            TIMEOUT 120
            LABELS "functional"
        )

        # Error paths test
        add_test(
            NAME functional_error_paths
            COMMAND ${BASH_PROGRAM} ${CMAKE_SOURCE_DIR}/tests/functional/test_error_paths.sh -f $<TARGET_FILE:squashfs-fuse>
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        )
        set_tests_properties(functional_error_paths PROPERTIES
            TIMEOUT 120
            LABELS "functional"
        )

        # Special files test (block/char devices, FIFO, sockets)
        add_test(
            NAME functional_special_files
            COMMAND ${BASH_PROGRAM} ${CMAKE_SOURCE_DIR}/tests/functional/test_special_files.sh -f $<TARGET_FILE:squashfs-fuse>
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        )
        set_tests_properties(functional_special_files PROPERTIES
            TIMEOUT 120
            LABELS "functional"
        )

        # Hardlinks test
        add_test(
            NAME functional_hardlinks
            COMMAND ${BASH_PROGRAM} ${CMAKE_SOURCE_DIR}/tests/functional/test_hardlinks.sh -f $<TARGET_FILE:squashfs-fuse>
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        )
        set_tests_properties(functional_hardlinks PROPERTIES
            TIMEOUT 120
            LABELS "functional"
        )

        # Sparse files test
        add_test(
            NAME functional_sparse_files
            COMMAND ${BASH_PROGRAM} ${CMAKE_SOURCE_DIR}/tests/functional/test_sparse_files.sh -f $<TARGET_FILE:squashfs-fuse>
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        )
        set_tests_properties(functional_sparse_files PROPERTIES
            TIMEOUT 120
            LABELS "functional"
        )

        # Large files test (>4GB)
        add_test(
            NAME functional_large_files
            COMMAND ${BASH_PROGRAM} ${CMAKE_SOURCE_DIR}/tests/functional/test_large_files.sh --quick -f $<TARGET_FILE:squashfs-fuse>
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        )
        set_tests_properties(functional_large_files PROPERTIES
            TIMEOUT 300
            LABELS "functional"
        )

        # Deep directories test
        add_test(
            NAME functional_deep_directories
            COMMAND ${BASH_PROGRAM} ${CMAKE_SOURCE_DIR}/tests/functional/test_deep_directories.sh -f $<TARGET_FILE:squashfs-fuse>
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        )
        set_tests_properties(functional_deep_directories PROPERTIES
            TIMEOUT 120
            LABELS "functional"
        )

        # Long filenames test
        add_test(
            NAME functional_long_filenames
            COMMAND ${BASH_PROGRAM} ${CMAKE_SOURCE_DIR}/tests/functional/test_long_filenames.sh -f $<TARGET_FILE:squashfs-fuse>
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        )
        set_tests_properties(functional_long_filenames PROPERTIES
            TIMEOUT 120
            LABELS "functional"
        )

        # Special characters in filenames test
        add_test(
            NAME functional_special_chars
            COMMAND ${BASH_PROGRAM} ${CMAKE_SOURCE_DIR}/tests/functional/test_special_chars.sh -f $<TARGET_FILE:squashfs-fuse>
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        )
        set_tests_properties(functional_special_chars PROPERTIES
            TIMEOUT 120
            LABELS "functional"
        )

        # Zstd compression test
        add_test(
            NAME functional_zstd_compression
            COMMAND ${BASH_PROGRAM} ${CMAKE_SOURCE_DIR}/tests/functional/test_zstd_compression.sh -f $<TARGET_FILE:squashfs-fuse>
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        )
        set_tests_properties(functional_zstd_compression PROPERTIES
            TIMEOUT 180
            LABELS "functional"
        )

        # Block sizes test
        add_test(
            NAME functional_block_sizes
            COMMAND ${BASH_PROGRAM} ${CMAKE_SOURCE_DIR}/tests/functional/test_block_sizes.sh -f $<TARGET_FILE:squashfs-fuse>
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        )
        set_tests_properties(functional_block_sizes PROPERTIES
            TIMEOUT 180
            LABELS "functional"
        )

        # Export table test
        add_test(
            NAME functional_export_table
            COMMAND ${BASH_PROGRAM} ${CMAKE_SOURCE_DIR}/tests/functional/test_export_table.sh -f $<TARGET_FILE:squashfs-fuse>
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        )
        set_tests_properties(functional_export_table PROPERTIES
            TIMEOUT 120
            LABELS "functional"
        )

        message(STATUS "Functional tests enabled")
    endif()

    # Python tests
    find_package(Python3 QUIET COMPONENTS Interpreter)

    if(Python3_Interpreter_FOUND)
        # Read test
        add_test(
            NAME python_read
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/functional/test_read.py -f $<TARGET_FILE:squashfs-fuse>
            WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
        )
        set_tests_properties(python_read PROPERTIES
            TIMEOUT 180
            LABELS "functional"
        )

        message(STATUS "Python tests enabled")
    endif()

    # Custom target for running all tests with verbose output
    add_custom_target(test_verbose
        COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -V
        DEPENDS squashfs-fuse
        COMMENT "Running all tests with verbose output"
    )

    # Custom target for running only unit tests
    if(CMocka_FOUND)
        add_custom_target(test_unit
            COMMAND ${CMAKE_CTEST_COMMAND} -L unit --output-on-failure
            DEPENDS test_cache test_compressor test_utils test_superblock test_inode test_directory test_fragment test_metadata test_export
            COMMENT "Running unit tests"
        )
    endif()

    # Custom target for running functional tests
    add_custom_target(test_functional
        COMMAND ${CMAKE_CTEST_COMMAND} -L functional --output-on-failure
        DEPENDS squashfs-fuse
        COMMENT "Running functional tests"
    )

    # Custom target for running performance benchmarks
    add_custom_target(test_performance
        COMMAND ${BASH_PROGRAM} ${CMAKE_SOURCE_DIR}/tests/performance/run_benchmark.sh
        DEPENDS squashfs-fuse
        COMMENT "Running performance benchmarks"
        WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
    )
endif()