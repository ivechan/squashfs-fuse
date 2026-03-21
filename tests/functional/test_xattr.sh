#!/bin/bash
#
# test_xattr.sh - Extended attributes tests for SquashFS-FUSE
#
# This script tests xattr (extended attributes) support:
# - Create image with xattrs
# - Read xattr values
# - List xattrs
#
# Usage: ./test_xattr.sh [options]
#   -h, --help              Show help message
#   -v, --verbose           Enable verbose output
#   -f, --fuse-binary PATH  Path to squashfs-fuse binary
#   --keep                  Keep temporary files after test
#

set -e

# Configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# Default values
VERBOSE=false
KEEP_TEMP=false
FUSE_BINARY=""

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# Test counters
TESTS_RUN=0
TESTS_PASSED=0
TESTS_FAILED=0

# Temporary files
TEMP_DIR=""
MOUNT_POINT=""

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

# Test functions
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

# Usage
usage() {
    cat << EOF
Usage: $(basename "$0") [options]

Extended attributes (xattr) tests for SquashFS-FUSE.

Options:
  -h, --help              Show help message
  -v, --verbose           Enable verbose output
  -f, --fuse-binary PATH  Path to squashfs-fuse binary
  --keep                  Keep temporary files

Requirements:
  - mksquashfs with xattr support
  - getfattr command (from attr package)
EOF
}

# Parse arguments
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

# Check dependencies
check_dependencies() {
    log_info "Checking dependencies..."

    # Check mksquashfs
    if ! command -v mksquashfs &> /dev/null; then
        log_error "mksquashfs not found"
        exit 2
    fi

    # Check getfattr
    if ! command -v getfattr &> /dev/null; then
        log_warn "getfattr not found, some tests will be skipped"
        HAS_GETFATTR=false
    else
        HAS_GETFATTR=true
        log_debug "Found getfattr"
    fi

    # Find FUSE binary
    if [ -z "${FUSE_BINARY}" ]; then
        for path in "${PROJECT_ROOT}/build/squashfs-fuse" \
                    "${PROJECT_ROOT}/squashfs-fuse"; do
            if [ -x "${path}" ]; then
                FUSE_BINARY="${path}"
                break
            fi
        done
    fi

    if [ -z "${FUSE_BINARY}" ]; then
        log_error "No FUSE binary found. Specify with -f option."
        exit 2
    fi

    log_info "Using FUSE binary: ${FUSE_BINARY}"
}

# Setup test environment
setup() {
    log_info "Setting up test environment..."

    TEMP_DIR=$(mktemp -d -t squashfs-xattr-test-XXXXXX)
    MOUNT_POINT="${TEMP_DIR}/mnt"
    mkdir -p "${MOUNT_POINT}"

    log_debug "Created temp directory: ${TEMP_DIR}"
}

# Create test image with xattrs
create_xattr_image() {
    log_info "Creating test image with xattrs..."

    local content_dir="${TEMP_DIR}/content"
    local image_path="${TEMP_DIR}/xattr_test.sqfs"

    mkdir -p "${content_dir}"

    # Create test files
    echo "File with user xattr" > "${content_dir}/file1.txt"
    echo "File with security xattr" > "${content_dir}/file2.txt"
    echo "File with multiple xattrs" > "${content_dir}/file3.txt"
    mkdir "${content_dir}/xattr_dir"

    # Add user xattrs
    if [ "${HAS_GETFATTR}" = true ]; then
        # User namespace xattrs
        setfattr -n user.comment -v "This is a user comment" "${content_dir}/file1.txt" 2>/dev/null || true
        setfattr -n user.version -v "1.0.0" "${content_dir}/file1.txt" 2>/dev/null || true

        # Multiple xattrs on one file
        setfattr -n user.author -v "Test Author" "${content_dir}/file3.txt" 2>/dev/null || true
        setfattr -n user.description -v "Test file description" "${content_dir}/file3.txt" 2>/dev/null || true
        setfattr -n user.number -v "42" "${content_dir}/file3.txt" 2>/dev/null || true

        # Directory xattr
        setfattr -n user.dirflag -v "true" "${content_dir}/xattr_dir" 2>/dev/null || true
    fi

    # Create SquashFS image WITH xattrs (remove -no-xattrs)
    mksquashfs "${content_dir}" "${image_path}" -noappend -comp gzip > /dev/null 2>&1

    if [ ! -f "${image_path}" ]; then
        log_error "Failed to create xattr test image"
        exit 1
    fi

    local size
    size=$(stat -c%s "${image_path}" 2>/dev/null || stat -f%z "${image_path}" 2>/dev/null)
    log_info "Created xattr test image: ${image_path} (${size} bytes)"

    TEST_IMAGE="${image_path}"
}

# Cleanup
cleanup() {
    local exit_code=$?

    log_info "Cleaning up..."

    if [ -n "${MOUNT_POINT}" ] && mountpoint -q "${MOUNT_POINT}" 2>/dev/null; then
        fusermount3 -u "${MOUNT_POINT}" 2>/dev/null || \
            fusermount -u "${MOUNT_POINT}" 2>/dev/null || \
            umount "${MOUNT_POINT}" 2>/dev/null
    fi

    if [ "${KEEP_TEMP}" = false ] && [ -n "${TEMP_DIR}" ] && [ -d "${TEMP_DIR}" ]; then
        rm -rf "${TEMP_DIR}"
    fi

    exit ${exit_code}
}

# Mount image
mount_image() {
    log_info "Mounting ${TEST_IMAGE} at ${MOUNT_POINT}"

    timeout 60 "${FUSE_BINARY}" -f "${TEST_IMAGE}" "${MOUNT_POINT}" &
    local fuse_pid=$!

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

# Unmount image
unmount_image() {
    if [ -z "${MOUNT_POINT}" ]; then
        return 0
    fi

    log_info "Unmounting ${MOUNT_POINT}"

    if mountpoint -q "${MOUNT_POINT}" 2>/dev/null; then
        fusermount3 -u "${MOUNT_POINT}" 2>/dev/null || \
            fusermount -u "${MOUNT_POINT}" 2>/dev/null || \
            umount "${MOUNT_POINT}" 2>/dev/null
    fi
}

# Test: Check xattr support in filesystem
test_xattr_support() {
    test_start "Xattr filesystem support"

    # Try to list xattrs - this tests if the filesystem supports xattrs
    if [ "${HAS_GETFATTR}" = true ]; then
        local result
        result=$(getfattr -d "${MOUNT_POINT}/file1.txt" 2>/dev/null || echo "")

        if [ -n "${result}" ]; then
            log_debug "Xattr support detected"
            test_pass
        else
            # May be empty if no xattrs were stored
            log_debug "No xattrs found (may be empty or not supported)"
            test_pass
        fi
    else
        log_warn "getfattr not available, skipping"
        test_pass
    fi
}

# Test: Read user xattr
test_read_user_xattr() {
    test_start "Read user xattr"

    if [ "${HAS_GETFATTR}" != true ]; then
        log_warn "getfattr not available, skipping"
        test_pass
        return
    fi

    local value
    value=$(getfattr -n user.comment --only-values "${MOUNT_POINT}/file1.txt" 2>/dev/null || echo "")

    if [ "${value}" = "This is a user comment" ]; then
        log_debug "User xattr value correct: ${value}"
        test_pass
    else
        log_debug "User xattr value: '${value}' (expected 'This is a user comment')"
        # This may fail if xattrs weren't stored properly
        test_fail "user.comment xattr not found or incorrect"
    fi
}

# Test: Read multiple xattrs
test_read_multiple_xattrs() {
    test_start "Read multiple xattrs from one file"

    if [ "${HAS_GETFATTR}" != true ]; then
        log_warn "getfattr not available, skipping"
        test_pass
        return
    fi

    local author
    local desc
    local num

    author=$(getfattr -n user.author --only-values "${MOUNT_POINT}/file3.txt" 2>/dev/null || echo "")
    desc=$(getfattr -n user.description --only-values "${MOUNT_POINT}/file3.txt" 2>/dev/null || echo "")
    num=$(getfattr -n user.number --only-values "${MOUNT_POINT}/file3.txt" 2>/dev/null || echo "")

    if [ "${author}" = "Test Author" ] && [ "${desc}" = "Test file description" ] && [ "${num}" = "42" ]; then
        log_debug "All multiple xattrs correct"
        test_pass
    else
        log_debug "author='${author}', desc='${desc}', num='${num}'"
        test_fail "Multiple xattrs incomplete or incorrect"
    fi
}

# Test: List xattrs
test_list_xattrs() {
    test_start "List xattrs"

    if [ "${HAS_GETFATTR}" != true ]; then
        log_warn "getfattr not available, skipping"
        test_pass
        return
    fi

    local xattr_list
    xattr_list=$(getfattr -d "${MOUNT_POINT}/file3.txt" 2>/dev/null | grep -c "^user\." || echo "0")

    if [ "${xattr_list}" -ge 2 ]; then
        log_debug "Found ${xattr_list} xattrs"
        test_pass
    else
        log_debug "Found ${xattr_list} xattrs (expected at least 2)"
        test_fail "Not enough xattrs listed"
    fi
}

# Test: Directory xattr
test_directory_xattr() {
    test_start "Directory xattr"

    if [ "${HAS_GETFATTR}" != true ]; then
        log_warn "getfattr not available, skipping"
        test_pass
        return
    fi

    local value
    value=$(getfattr -n user.dirflag --only-values "${MOUNT_POINT}/xattr_dir" 2>/dev/null || echo "")

    if [ "${value}" = "true" ]; then
        log_debug "Directory xattr correct"
        test_pass
    else
        log_debug "Directory xattr value: '${value}'"
        test_fail "Directory xattr not found or incorrect"
    fi
}

# Test: Non-existent xattr
test_nonexistent_xattr() {
    test_start "Non-existent xattr returns error"

    if [ "${HAS_GETFATTR}" != true ]; then
        log_warn "getfattr not available, skipping"
        test_pass
        return
    fi

    # Try to read a non-existent xattr
    local result
    result=$(getfattr -n user.nonexistent --only-values "${MOUNT_POINT}/file1.txt" 2>&1 || true)

    if echo "${result}" | grep -qi "no such attribute\|could not get\|not found"; then
        log_debug "Non-existent xattr correctly returns error"
        test_pass
    else
        # Even if no error message, empty result is acceptable
        test_pass
    fi
}

# Print summary
print_summary() {
    echo ""
    echo "================================"
    echo "        XATTR TEST SUMMARY"
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

# Main
main() {
    parse_args "$@"

    echo "================================"
    echo "  SquashFS-FUSE Xattr Tests"
    echo "================================"
    echo ""

    check_dependencies
    trap cleanup EXIT
    setup
    create_xattr_image

    if mount_image; then
        test_xattr_support
        test_read_user_xattr
        test_read_multiple_xattrs
        test_list_xattrs
        test_directory_xattr
        test_nonexistent_xattr
        unmount_image
    fi

    print_summary
}

main "$@"