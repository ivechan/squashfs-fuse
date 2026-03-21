#!/bin/bash
#
# test_error_paths.sh - Error handling tests for SquashFS-FUSE
#
# This script tests error handling and edge cases:
# - Invalid/corrupted images
# - Invalid command line arguments
# - Missing files
# - Permission errors
# - Boundary conditions
#
# Usage: ./test_error_paths.sh [options]
#   -h, --help              Show help message
#   -v, --verbose           Enable verbose output
#   -f, --fuse-binary PATH  Path to squashfs-fuse binary
#

set -e

# Configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# Default values
VERBOSE=false
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

Error handling tests for SquashFS-FUSE.

Options:
  -h, --help              Show help message
  -v, --verbose           Enable verbose output
  -f, --fuse-binary PATH  Path to squashfs-fuse binary
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

    if [ ! -x "${FUSE_BINARY}" ]; then
        log_error "FUSE binary not executable: ${FUSE_BINARY}"
        exit 2
    fi

    log_info "Using FUSE binary: ${FUSE_BINARY}"
}

# Setup test environment
setup() {
    log_info "Setting up test environment..."

    TEMP_DIR=$(mktemp -d -t squashfs-error-test-XXXXXX)
    log_debug "Created temp directory: ${TEMP_DIR}"
}

# Cleanup
cleanup() {
    local exit_code=$?

    log_info "Cleaning up..."

    if [ -n "${TEMP_DIR}" ] && [ -d "${TEMP_DIR}" ]; then
        rm -rf "${TEMP_DIR}"
    fi

    exit ${exit_code}
}

# ============================================================================
# Command Line Error Tests
# ============================================================================

# Test: No arguments
test_no_arguments() {
    test_start "No arguments returns error"

    local output
    local ret=0
    output=$("${FUSE_BINARY}" 2>&1) || ret=$?

    if [ ${ret} -ne 0 ]; then
        log_debug "Correctly returned error code ${ret}"
        test_pass
    else
        test_fail "Should return error with no arguments"
    fi
}

# Test: Missing image file
test_missing_image() {
    test_start "Missing image file returns error"

    local output
    local ret=0
    output=$("${FUSE_BINARY}" "${TEMP_DIR}/nonexistent.sqfs" "${TEMP_DIR}/mnt" 2>&1) || ret=$?

    if [ ${ret} -ne 0 ]; then
        log_debug "Correctly returned error for missing image"
        test_pass
    else
        test_fail "Should return error for missing image file"
    fi
}

# Test: Missing mount point
test_missing_mount_point() {
    test_start "Missing mount point returns error"

    # Create a valid image first
    local valid_image="${TEMP_DIR}/valid.sqfs"
    mkdir -p "${TEMP_DIR}/content"
    echo "test" > "${TEMP_DIR}/content/file.txt"
    mksquashfs "${TEMP_DIR}/content" "${valid_image}" -noappend -comp gzip > /dev/null 2>&1

    local output
    local ret=0
    output=$("${FUSE_BINARY}" "${valid_image}" 2>&1) || ret=$?

    if [ ${ret} -ne 0 ]; then
        log_debug "Correctly returned error for missing mount point"
        test_pass
    else
        test_fail "Should return error for missing mount point"
    fi
}

# Test: Help flag
test_help_flag() {
    test_start "Help flag works"

    local output
    local ret=0
    output=$("${FUSE_BINARY}" --help 2>&1) || ret=$?

    if echo "${output}" | grep -qi "usage\|help\|options"; then
        log_debug "Help output contains usage information"
        test_pass
    else
        test_fail "Help output should contain usage information"
    fi
}

# Test: Version flag
test_version_flag() {
    test_start "Version flag works"

    local output
    output=$("${FUSE_BINARY}" --version 2>&1) || true

    if echo "${output}" | grep -qi "version"; then
        log_debug "Version output correct"
        test_pass
    else
        test_fail "Version output should contain version string"
    fi
}

# ============================================================================
# Invalid Image Tests
# ============================================================================

# Test: Invalid magic number
test_invalid_magic() {
    test_start "Invalid magic number returns error"

    local bad_image="${TEMP_DIR}/bad_magic.bin"

    # Create file with wrong magic
    echo "NOT_SQUASHFS" > "${bad_image}"

    local output
    local ret=0
    output=$("${FUSE_BINARY}" "${bad_image}" "${TEMP_DIR}/mnt" 2>&1) || ret=$?

    if [ ${ret} -ne 0 ]; then
        log_debug "Correctly rejected file with invalid magic"
        test_pass
    else
        test_fail "Should reject file with invalid magic number"
    fi
}

# Test: Empty file
test_empty_file() {
    test_start "Empty file returns error"

    local empty_image="${TEMP_DIR}/empty.bin"
    touch "${empty_image}"

    local output
    local ret=0
    output=$("${FUSE_BINARY}" "${empty_image}" "${TEMP_DIR}/mnt" 2>&1) || ret=$?

    if [ ${ret} -ne 0 ]; then
        log_debug "Correctly rejected empty file"
        test_pass
    else
        test_fail "Should reject empty file"
    fi
}

# Test: Truncated superblock
test_truncated_superblock() {
    test_start "Truncated superblock returns error"

    local trunc_image="${TEMP_DIR}/truncated.bin"

    # Create file with partial superblock (less than 96 bytes)
    dd if=/dev/zero of="${trunc_image}" bs=1 count=50 2>/dev/null

    # Write SquashFS magic but truncated
    printf '\x68\x73\x71\x73' | dd of="${trunc_image}" conv=notrunc 2>/dev/null

    local output
    local ret=0
    output=$("${FUSE_BINARY}" "${trunc_image}" "${TEMP_DIR}/mnt" 2>&1) || ret=$?

    if [ ${ret} -ne 0 ]; then
        log_debug "Correctly rejected truncated file"
        test_pass
    else
        test_fail "Should reject file with truncated superblock"
    fi
}

# Test: Wrong version
test_wrong_version() {
    test_start "Unsupported version returns error"

    local wrong_ver="${TEMP_DIR}/wrong_version.bin"

    # Create a 96-byte file with SquashFS magic but wrong version
    dd if=/dev/zero of="${wrong_ver}" bs=96 count=1 2>/dev/null

    # Write SquashFS magic (hsqs)
    printf '\x68\x73\x71\x73' | dd of="${wrong_ver}" conv=notrunc 2>/dev/null

    # Set version to 99.99 (unsupported)
    printf '\x63\x00\x63\x00' | dd of="${wrong_ver}" bs=1 seek=28 conv=notrunc 2>/dev/null

    local output
    local ret=0
    output=$("${FUSE_BINARY}" "${wrong_ver}" "${TEMP_DIR}/mnt" 2>&1) || ret=$?

    if [ ${ret} -ne 0 ]; then
        log_debug "Correctly rejected unsupported version"
        test_pass
    else
        test_fail "Should reject unsupported version"
    fi
}

# Test: Unsupported compressor
test_unsupported_compressor() {
    test_start "Unsupported compressor returns error"

    local unsup_comp="${TEMP_DIR}/unsup_comp.bin"

    # Create a 96-byte file with SquashFS magic
    dd if=/dev/zero of="${unsup_comp}" bs=96 count=1 2>/dev/null

    # Write SquashFS magic
    printf '\x68\x73\x71\x73' | dd of="${unsup_comp}" conv=notrunc 2>/dev/null

    # Set version to 4.0
    printf '\x04\x00\x00\x00' | dd of="${unsup_comp}" bs=1 seek=28 conv=notrunc 2>/dev/null

    # Set compressor to 99 (unsupported)
    printf '\x63\x00' | dd of="${unsup_comp}" bs=1 seek=20 conv=notrunc 2>/dev/null

    local output
    local ret=0
    output=$("${FUSE_BINARY}" "${unsup_comp}" "${TEMP_DIR}/mnt" 2>&1) || ret=$?

    if [ ${ret} -ne 0 ]; then
        log_debug "Correctly rejected unsupported compressor"
        test_pass
    else
        test_fail "Should reject unsupported compressor"
    fi
}

# ============================================================================
# Valid Image Error Tests
# ============================================================================

# Test: Non-existent path in mounted image
test_nonexistent_path() {
    test_start "Non-existent path returns ENOENT"

    local valid_image="${TEMP_DIR}/valid.sqfs"
    local mount_point="${TEMP_DIR}/mnt"

    # Ensure we have a valid image
    if [ ! -f "${valid_image}" ]; then
        mkdir -p "${TEMP_DIR}/content"
        echo "test" > "${TEMP_DIR}/content/file.txt"
        mksquashfs "${TEMP_DIR}/content" "${valid_image}" -noappend -comp gzip > /dev/null 2>&1
    fi

    mkdir -p "${mount_point}"

    # Mount in background
    timeout 30 "${FUSE_BINARY}" -f "${valid_image}" "${mount_point}" &
    local fuse_pid=$!

    # Wait for mount
    local wait_count=0
    while ! mountpoint -q "${mount_point}" 2>/dev/null; do
        sleep 0.1
        wait_count=$((wait_count + 1))
        if [ ${wait_count} -gt 50 ]; then
            kill ${fuse_pid} 2>/dev/null || true
            test_fail "Timeout waiting for mount"
            return
        fi
    done

    # Try to access non-existent path
    local ret=0
    ls "${mount_point}/nonexistent_path_12345" 2>/dev/null || ret=$?

    # Unmount
    fusermount3 -u "${mount_point}" 2>/dev/null || fusermount -u "${mount_point}" 2>/dev/null || true

    if [ ${ret} -ne 0 ]; then
        log_debug "Correctly returned error for non-existent path"
        test_pass
    else
        test_fail "Should return error for non-existent path"
    fi
}

# Test: Read non-existent file
test_read_nonexistent_file() {
    test_start "Read non-existent file returns error"

    local valid_image="${TEMP_DIR}/valid.sqfs"
    local mount_point="${TEMP_DIR}/mnt2"

    mkdir -p "${mount_point}"

    # Mount in background
    timeout 30 "${FUSE_BINARY}" -f "${valid_image}" "${mount_point}" &
    local fuse_pid=$!

    # Wait for mount
    local wait_count=0
    while ! mountpoint -q "${mount_point}" 2>/dev/null; do
        sleep 0.1
        wait_count=$((wait_count + 1))
        if [ ${wait_count} -gt 50 ]; then
            kill ${fuse_pid} 2>/dev/null || true
            test_fail "Timeout waiting for mount"
            return
        fi
    done

    # Try to read non-existent file
    local ret=0
    cat "${mount_point}/nonexistent_file.txt" 2>/dev/null || ret=$?

    # Unmount
    fusermount3 -u "${mount_point}" 2>/dev/null || fusermount -u "${mount_point}" 2>/dev/null || true

    if [ ${ret} -ne 0 ]; then
        log_debug "Correctly returned error for reading non-existent file"
        test_pass
    else
        test_fail "Should return error for reading non-existent file"
    fi
}

# Test: Read directory as file
test_read_directory_as_file() {
    test_start "Read directory as file returns error"

    local valid_image="${TEMP_DIR}/valid.sqfs"
    local mount_point="${TEMP_DIR}/mnt3"

    mkdir -p "${mount_point}"

    # Mount in background
    timeout 30 "${FUSE_BINARY}" -f "${valid_image}" "${mount_point}" &
    local fuse_pid=$!

    # Wait for mount
    local wait_count=0
    while ! mountpoint -q "${mount_point}" 2>/dev/null; do
        sleep 0.1
        wait_count=$((wait_count + 1))
        if [ ${wait_count} -gt 50 ]; then
            kill ${fuse_pid} 2>/dev/null || true
            test_fail "Timeout waiting for mount"
            return
        fi
    done

    # Try to read a directory as a file
    local ret=0
    cat "${mount_point}" 2>/dev/null || ret=$?

    # Unmount
    fusermount3 -u "${mount_point}" 2>/dev/null || fusermount -u "${mount_point}" 2>/dev/null || true

    if [ ${ret} -ne 0 ]; then
        log_debug "Correctly returned error for reading directory as file"
        test_pass
    else
        test_fail "Should return error for reading directory as file"
    fi
}

# ============================================================================
# Print Summary
# ============================================================================

print_summary() {
    echo ""
    echo "================================"
    echo "    ERROR PATH TEST SUMMARY"
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
    echo "  SquashFS-FUSE Error Tests"
    echo "================================"
    echo ""

    check_dependencies
    trap cleanup EXIT
    setup

    # Command line error tests
    echo ""
    log_info "=== Command Line Error Tests ==="
    test_no_arguments
    test_missing_image
    test_missing_mount_point
    test_help_flag
    test_version_flag

    # Invalid image tests
    echo ""
    log_info "=== Invalid Image Tests ==="
    test_invalid_magic
    test_empty_file
    test_truncated_superblock
    test_wrong_version
    test_unsupported_compressor

    # Valid image error tests
    echo ""
    log_info "=== Runtime Error Tests ==="

    # Create a valid image for runtime tests
    mkdir -p "${TEMP_DIR}/content"
    echo "test content" > "${TEMP_DIR}/content/file.txt"
    mkdir "${TEMP_DIR}/content/subdir"
    echo "nested file" > "${TEMP_DIR}/content/subdir/nested.txt"
    mksquashfs "${TEMP_DIR}/content" "${TEMP_DIR}/valid.sqfs" -noappend -comp gzip > /dev/null 2>&1

    test_nonexistent_path
    test_read_nonexistent_file
    test_read_directory_as_file

    print_summary
}

main "$@"