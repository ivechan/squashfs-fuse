#!/bin/bash
#
# test_basic.sh - Basic functionality tests for SquashFS-FUSE
#
# This script performs basic tests to verify SquashFS-FUSE functionality:
# - mksquashfs availability check
# - Test directory and file creation
# - SquashFS image generation
# - Mount the image
# - Test ls, cat, stat operations
# - Unmount and cleanup
#
# Usage: ./test_basic.sh [options]
#   -h, --help              Show this help message
#   -v, --verbose           Enable verbose output
#   -i, --image PATH        Use existing image instead of creating one
#   -m, --mount PATH        Specify mount point (default: auto-create temp dir)
#   -f, --fuse-binary PATH  Path to squashfs-fuse binary
#   --keep                  Keep temporary files after test
#
# Exit codes:
#   0 - All tests passed
#   1 - Test failure
#   2 - Missing dependencies
#   3 - Mount/unmount error
#

set -e

# Configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
FIXTURES_DIR="${PROJECT_ROOT}/tests/fixtures"

# Default values
VERBOSE=false
KEEP_TEMP=false
MOUNT_POINT=""
TEST_IMAGE=""
FUSE_BINARY=""

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Test counters
TESTS_RUN=0
TESTS_PASSED=0
TESTS_FAILED=0

# Temporary files
TEMP_DIR=""
TEMP_CONTENT=""
TEMP_IMAGE=""

# Logging functions
log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

log_debug() {
    if [ "${VERBOSE}" = true ]; then
        echo -e "${BLUE}[DEBUG]${NC} $1"
    fi
}

# Test assertion functions
test_start() {
    local test_name="$1"
    TESTS_RUN=$((TESTS_RUN + 1))
    log_info "Test ${TESTS_RUN}: ${test_name}"
}

test_pass() {
    TESTS_PASSED=$((TESTS_PASSED + 1))
    log_debug "PASSED"
}

test_fail() {
    local reason="$1"
    TESTS_FAILED=$((TESTS_FAILED + 1))
    log_error "FAILED: ${reason}"
}

# Usage information
usage() {
    cat << EOF
Usage: $(basename "$0") [options]

Basic functionality tests for SquashFS-FUSE.

Options:
  -h, --help              Show this help message
  -v, --verbose           Enable verbose output
  -i, --image PATH        Use existing image instead of creating one
  -m, --mount PATH        Specify mount point (default: auto-create temp dir)
  -f, --fuse-binary PATH  Path to squashfs-fuse binary
  --keep                  Keep temporary files after test

Examples:
  $(basename "$0")                          # Run with defaults
  $(basename "$0") -v                       # Verbose mode
  $(basename "$0") -i test.sqfs             # Use existing image
  $(basename "$0") -f ./squashfs-fuse       # Specify binary
EOF
}

# Parse command line arguments
parse_args() {
    while [[ $# -gt 0 ]]; do
        case $1 in
            -h|--help)
                usage
                exit 0
                ;;
            -v|--verbose)
                VERBOSE=true
                shift
                ;;
            -i|--image)
                TEST_IMAGE="$2"
                shift 2
                ;;
            -m|--mount)
                MOUNT_POINT="$2"
                shift 2
                ;;
            -f|--fuse-binary)
                FUSE_BINARY="$2"
                shift 2
                ;;
            --keep)
                KEEP_TEMP=true
                shift
                ;;
            *)
                log_error "Unknown option: $1"
                usage
                exit 1
                ;;
        esac
    done
}

# Check for required dependencies
check_dependencies() {
    log_info "Checking dependencies..."

    # Check mksquashfs
    if ! command -v mksquashfs &> /dev/null; then
        log_error "mksquashfs not found. Please install squashfs-tools."
        exit 2
    fi
    log_debug "Found mksquashfs: $(mksquashfs -version 2>&1 | head -n1)"

    # Check fusermount (for FUSE)
    if ! command -v fusermount &> /dev/null && ! command -v fusermount3 &> /dev/null; then
        log_warn "fusermount not found. FUSE may not be properly installed."
    fi
    log_debug "Found fusermount: $(command -v fusermount3 || command -v fusermount || echo 'not found')"

    # Check FUSE binary
    if [ -z "${FUSE_BINARY}" ]; then
        # Try to find in build directory
        for path in "${PROJECT_ROOT}/build/squashfs-fuse" \
                    "${PROJECT_ROOT}/squashfs-fuse" \
                    "./squashfs-fuse"; do
            if [ -x "${path}" ]; then
                FUSE_BINARY="${path}"
                break
            fi
        done
    fi

    if [ -n "${FUSE_BINARY}" ]; then
        if [ ! -x "${FUSE_BINARY}" ]; then
            log_error "FUSE binary not executable: ${FUSE_BINARY}"
            exit 2
        fi
        log_debug "Using FUSE binary: ${FUSE_BINARY}"
    else
        log_warn "No FUSE binary specified or found. Mount tests will be skipped."
    fi

    # Check stat command
    if ! command -v stat &> /dev/null; then
        log_error "stat command not found."
        exit 2
    fi

    log_info "All dependencies satisfied"
}

# Setup test environment
setup() {
    log_info "Setting up test environment..."

    # Create temporary directory
    if [ -z "${TEMP_DIR}" ]; then
        TEMP_DIR=$(mktemp -d -t squashfs-test-XXXXXX)
        log_debug "Created temp directory: ${TEMP_DIR}"
    fi

    # Set mount point
    if [ -z "${MOUNT_POINT}" ]; then
        MOUNT_POINT="${TEMP_DIR}/mnt"
        mkdir -p "${MOUNT_POINT}"
        log_debug "Created mount point: ${MOUNT_POINT}"
    fi

    # Create test content if needed
    if [ -z "${TEST_IMAGE}" ]; then
        create_test_image
    else
        if [ ! -f "${TEST_IMAGE}" ]; then
            log_error "Test image not found: ${TEST_IMAGE}"
            exit 1
        fi
        log_info "Using existing test image: ${TEST_IMAGE}"
    fi
}

# Create test SquashFS image
create_test_image() {
    log_info "Creating test SquashFS image..."

    TEMP_CONTENT="${TEMP_DIR}/content"
    TEMP_IMAGE="${TEMP_DIR}/test.sqfs"

    mkdir -p "${TEMP_CONTENT}"

    # Create test directory structure
    mkdir -p "${TEMP_CONTENT}/subdir1"
    mkdir -p "${TEMP_CONTENT}/subdir2/nested"
    mkdir -p "${TEMP_CONTENT}/empty_dir"

    # Create test files
    echo "Hello, SquashFS World!" > "${TEMP_CONTENT}/hello.txt"
    echo "This is a test file" > "${TEMP_CONTENT}/test.txt"
    echo "File in subdirectory 1" > "${TEMP_CONTENT}/subdir1/file1.txt"
    echo "File in subdirectory 2" > "${TEMP_CONTENT}/subdir2/file2.txt"
    echo "Nested deep file" > "${TEMP_CONTENT}/subdir2/nested/deep.txt"

    # Create multiline file
    for i in $(seq 1 50); do
        echo "Line ${i}: $(printf '%0*d' 30 ${i})" >> "${TEMP_CONTENT}/multiline.txt"
    done

    # Create binary file
    dd if=/dev/urandom of="${TEMP_CONTENT}/binary.bin" bs=1024 count=10 2>/dev/null

    # Create symlink
    ln -s hello.txt "${TEMP_CONTENT}/link_to_hello.txt"

    # Create the SquashFS image
    mksquashfs "${TEMP_CONTENT}" "${TEMP_IMAGE}" -noappend -no-xattrs -comp gzip > /dev/null 2>&1

    if [ ! -f "${TEMP_IMAGE}" ]; then
        log_error "Failed to create test image"
        exit 1
    fi

    local size
    size=$(stat -c%s "${TEMP_IMAGE}" 2>/dev/null || stat -f%z "${TEMP_IMAGE}" 2>/dev/null)
    log_info "Created test image: ${TEMP_IMAGE} (${size} bytes)"

    TEST_IMAGE="${TEMP_IMAGE}"
}

# Cleanup function
cleanup() {
    local exit_code=$?

    log_info "Cleaning up..."

    # Unmount if mounted
    if [ -n "${MOUNT_POINT}" ] && mountpoint -q "${MOUNT_POINT}" 2>/dev/null; then
        log_debug "Unmounting ${MOUNT_POINT}"
        fusermount3 -u "${MOUNT_POINT}" 2>/dev/null || fusermount -u "${MOUNT_POINT}" 2>/dev/null || umount "${MOUNT_POINT}" 2>/dev/null
    fi

    # Remove temporary files
    if [ "${KEEP_TEMP}" = false ] && [ -n "${TEMP_DIR}" ] && [ -d "${TEMP_DIR}" ]; then
        log_debug "Removing temp directory: ${TEMP_DIR}"
        rm -rf "${TEMP_DIR}"
    fi

    exit ${exit_code}
}

# Mount the SquashFS image
mount_image() {
    if [ -z "${FUSE_BINARY}" ]; then
        log_warn "No FUSE binary available, skipping mount tests"
        return 1
    fi

    if [ ! -x "${FUSE_BINARY}" ]; then
        log_error "FUSE binary not executable: ${FUSE_BINARY}"
        return 1
    fi

    log_info "Mounting ${TEST_IMAGE} at ${MOUNT_POINT}"

    # Run in foreground mode with timeout
    timeout 60 "${FUSE_BINARY}" -f "${TEST_IMAGE}" "${MOUNT_POINT}" &
    local fuse_pid=$!

    # Wait for mount
    local wait_count=0
    while ! mountpoint -q "${MOUNT_POINT}" 2>/dev/null; do
        sleep 0.1
        wait_count=$((wait_count + 1))
        if [ ${wait_count} -gt 50 ]; then
            log_error "Timeout waiting for mount"
            kill ${fuse_pid} 2>/dev/null
            return 1
        fi
    done

    log_debug "Mounted successfully (PID: ${fuse_pid})"
    return 0
}

# Unmount the SquashFS image
unmount_image() {
    if [ -z "${MOUNT_POINT}" ]; then
        return 0
    fi

    log_info "Unmounting ${MOUNT_POINT}"

    if mountpoint -q "${MOUNT_POINT}" 2>/dev/null; then
        if fusermount3 -u "${MOUNT_POINT}" 2>/dev/null || \
           fusermount -u "${MOUNT_POINT}" 2>/dev/null || \
           umount "${MOUNT_POINT}" 2>/dev/null; then
            log_debug "Unmounted successfully"
            return 0
        else
            log_error "Failed to unmount"
            return 1
        fi
    fi
    return 0
}

# Test: Check mksquashfs availability
test_mksquashfs_available() {
    test_start "mksquashfs availability"

    if command -v mksquashfs &> /dev/null; then
        local version
        version=$(mksquashfs -version 2>&1 | head -n1)
        log_debug "Version: ${version}"
        test_pass
    else
        test_fail "mksquashfs not found"
    fi
}

# Test: Image creation
test_image_creation() {
    test_start "SquashFS image creation"

    if [ -f "${TEST_IMAGE}" ]; then
        local size
        size=$(stat -c%s "${TEST_IMAGE}" 2>/dev/null || stat -f%z "${TEST_IMAGE}" 2>/dev/null)

        # Check magic bytes (hsqs = 0x68737173 in file order)
        local magic
        magic=$(xxd -l 4 -p "${TEST_IMAGE}" 2>/dev/null)
        if [ "${magic}" = "68737173" ]; then
            log_debug "Valid SquashFS magic (hsqs)"
            log_debug "Image size: ${size} bytes"
            test_pass
        else
            test_fail "Invalid SquashFS magic: ${magic}"
        fi
    else
        test_fail "Image file not found"
    fi
}

# Test: Mount operation
test_mount() {
    test_start "Mount operation"

    if mount_image; then
        test_pass
    else
        test_fail "Failed to mount image"
    fi
}

# Test: List root directory
test_ls_root() {
    test_start "List root directory (ls)"

    if [ ! -d "${MOUNT_POINT}" ]; then
        test_fail "Mount point not accessible"
        return
    fi

    local files
    files=$(ls "${MOUNT_POINT}" 2>/dev/null)

    if [ -n "${files}" ]; then
        log_debug "Root contents: ${files}"
        # Check for expected files
        if echo "${files}" | grep -q "hello.txt" && \
           echo "${files}" | grep -q "test.txt" && \
           echo "${files}" | grep -q "subdir1" && \
           echo "${files}" | grep -q "subdir2"; then
            test_pass
        else
            test_fail "Missing expected files"
        fi
    else
        test_fail "Empty root directory"
    fi
}

# Test: List subdirectory
test_ls_subdir() {
    test_start "List subdirectory (ls)"

    if [ -d "${MOUNT_POINT}/subdir1" ]; then
        local files
        files=$(ls "${MOUNT_POINT}/subdir1" 2>/dev/null)

        if echo "${files}" | grep -q "file1.txt"; then
            log_debug "subdir1 contents: ${files}"
            test_pass
        else
            test_fail "Missing file1.txt in subdir1"
        fi
    else
        test_fail "subdir1 not found"
    fi
}

# Test: Read file content
test_cat_file() {
    test_start "Read file content (cat)"

    local expected="Hello, SquashFS World!"
    local actual
    actual=$(cat "${MOUNT_POINT}/hello.txt" 2>/dev/null)

    if [ "${actual}" = "${expected}" ]; then
        log_debug "Content matches"
        test_pass
    else
        test_fail "Content mismatch: expected '${expected}', got '${actual}'"
    fi
}

# Test: Read multiline file
test_cat_multiline() {
    test_start "Read multiline file"

    local lines
    lines=$(wc -l < "${MOUNT_POINT}/multiline.txt" 2>/dev/null)

    if [ "${lines}" -eq 50 ]; then
        log_debug "Line count correct: ${lines}"
        test_pass
    else
        test_fail "Line count mismatch: expected 50, got ${lines}"
    fi
}

# Test: Read binary file
test_cat_binary() {
    test_start "Read binary file"

    if [ -f "${MOUNT_POINT}/binary.bin" ]; then
        local size
        size=$(stat -c%s "${MOUNT_POINT}/binary.bin" 2>/dev/null || stat -f%z "${MOUNT_POINT}/binary.bin" 2>/dev/null)

        if [ "${size}" -eq 10240 ]; then
            log_debug "Binary file size correct: ${size} bytes"
            test_pass
        else
            test_fail "Binary size mismatch: expected 10240, got ${size}"
        fi
    else
        test_fail "binary.bin not found"
    fi
}

# Test: File stat
test_stat_file() {
    test_start "File stat"

    if [ -f "${MOUNT_POINT}/hello.txt" ]; then
        local size
        size=$(stat -c%s "${MOUNT_POINT}/hello.txt" 2>/dev/null || stat -f%z "${MOUNT_POINT}/hello.txt" 2>/dev/null)

        if [ "${size}" -gt 0 ]; then
            log_debug "File size: ${size} bytes"
            test_pass
        else
            test_fail "Invalid file size"
        fi
    else
        test_fail "hello.txt not found"
    fi
}

# Test: Directory stat
test_stat_dir() {
    test_start "Directory stat"

    if [ -d "${MOUNT_POINT}/subdir1" ]; then
        local type
        type=$(stat -c%F "${MOUNT_POINT}/subdir1" 2>/dev/null || stat -f%HT "${MOUNT_POINT}/subdir1" 2>/dev/null)

        # Check if it's a directory
        if [ -d "${MOUNT_POINT}/subdir1" ]; then
            log_debug "subdir1 is a directory"
            test_pass
        else
            test_fail "subdir1 is not a directory"
        fi
    else
        test_fail "subdir1 not found"
    fi
}

# Test: Symlink
test_symlink() {
    test_start "Symlink support"

    if [ -L "${MOUNT_POINT}/link_to_hello.txt" ]; then
        local target
        target=$(readlink "${MOUNT_POINT}/link_to_hello.txt" 2>/dev/null)
        log_debug "Symlink target: ${target}"

        # Read through symlink
        local content
        content=$(cat "${MOUNT_POINT}/link_to_hello.txt" 2>/dev/null)
        if [ -n "${content}" ]; then
            test_pass
        else
            test_fail "Cannot read through symlink"
        fi
    else
        test_fail "Symlink not found"
    fi
}

# Test: Empty directory
test_empty_dir() {
    test_start "Empty directory"

    if [ -d "${MOUNT_POINT}/empty_dir" ]; then
        local count
        count=$(ls -A "${MOUNT_POINT}/empty_dir" 2>/dev/null | wc -l)

        if [ "${count}" -eq 0 ]; then
            log_debug "Directory is empty"
            test_pass
        else
            test_fail "Directory not empty: ${count} items"
        fi
    else
        test_fail "empty_dir not found"
    fi
}

# Test: Nested directory
test_nested_dir() {
    test_start "Nested directory access"

    local deep_file="${MOUNT_POINT}/subdir2/nested/deep.txt"

    if [ -f "${deep_file}" ]; then
        local content
        content=$(cat "${deep_file}" 2>/dev/null)

        if [ "${content}" = "Nested deep file" ]; then
            log_debug "Nested file content correct"
            test_pass
        else
            test_fail "Content mismatch: '${content}'"
        fi
    else
        test_fail "Nested file not found: ${deep_file}"
    fi
}

# Test: Unmount operation
test_unmount() {
    test_start "Unmount operation"

    if unmount_image; then
        test_pass
    else
        test_fail "Failed to unmount"
    fi
}

# Print test summary
print_summary() {
    echo ""
    echo "================================"
    echo "        TEST SUMMARY"
    echo "================================"
    echo "Tests run:    ${TESTS_RUN}"
    echo "Tests passed: ${TESTS_PASSED}"
    echo "Tests failed: ${TESTS_FAILED}"
    echo "================================"

    if [ ${TESTS_FAILED} -eq 0 ]; then
        echo -e "${GREEN}All tests passed!${NC}"
        return 0
    else
        echo -e "${RED}Some tests failed.${NC}"
        return 1
    fi
}

# Main function
main() {
    parse_args "$@"

    echo "================================"
    echo "  SquashFS-FUSE Basic Tests"
    echo "================================"
    echo ""

    # Setup
    check_dependencies
    trap cleanup EXIT
    setup

    # Run tests that don't require mounting
    test_mksquashfs_available
    test_image_creation

    # Mount and run filesystem tests
    if [ -n "${FUSE_BINARY}" ]; then
        if mount_image; then
            test_ls_root
            test_ls_subdir
            test_cat_file
            test_cat_multiline
            test_cat_binary
            test_stat_file
            test_stat_dir
            test_symlink
            test_empty_dir
            test_nested_dir
            unmount_image
        fi
    fi

    # Print summary
    print_summary
}

main "$@"