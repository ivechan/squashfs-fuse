#!/bin/bash
#
# test_sparse_files.sh - Sparse file tests for SquashFS-FUSE
#
# Tests: sparse file handling, hole detection, zero regions
#
# Usage: ./test_sparse_files.sh [options]
#   -h, --help              Show help message
#   -v, --verbose           Enable verbose output
#   -f, --fuse-binary PATH  Path to squashfs-fuse binary
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

VERBOSE=false
FUSE_BINARY=""

RED='\033[0;31m'
GREEN='\033[0;32m'
NC='\033[0m'

TESTS_RUN=0
TESTS_PASSED=0
TESTS_FAILED=0
TEMP_DIR=""

log_info() { echo -e "${GREEN}[INFO]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }
log_debug() { [ "${VERBOSE}" = true ] && echo -e "[DEBUG] $1" || true; }

test_start() { TESTS_RUN=$((TESTS_RUN + 1)); log_info "Test ${TESTS_RUN}: $1"; }
test_pass() { TESTS_PASSED=$((TESTS_PASSED + 1)); log_debug "PASSED"; }
test_fail() { TESTS_FAILED=$((TESTS_FAILED + 1)); log_error "FAILED: $1"; }

usage() { cat << EOF
Usage: $(basename "$0") [options]

Sparse file tests for SquashFS-FUSE.

Options:
  -h, --help              Show help message
  -v, --verbose           Enable verbose output
  -f, --fuse-binary PATH  Path to squashfs-fuse binary
EOF
}

parse_args() {
    while [[ $# -gt 0 ]]; do
        case $1 in
            -h|--help) usage; exit 0 ;;
            -v|--verbose) VERBOSE=true; shift ;;
            -f|--fuse-binary) FUSE_BINARY="$2"; shift 2 ;;
            *) log_error "Unknown option: $1"; exit 1 ;;
        esac
    done
}

check_dependencies() {
    log_info "Checking dependencies..."
    [ -z "${FUSE_BINARY}" ] && for path in "${PROJECT_ROOT}/build/squashfs-fuse" "${PROJECT_ROOT}/squashfs-fuse"; do
        [ -x "${path}" ] && FUSE_BINARY="${path}" && break
    done
    [ -z "${FUSE_BINARY}" ] && { log_error "No FUSE binary found"; exit 2; }
    log_info "Using FUSE binary: ${FUSE_BINARY}"
}

setup() { TEMP_DIR=$(mktemp -d -t squashfs-sparse-test-XXXXXX); }

cleanup() {
    local exit_code=$?
    [ -n "${TEMP_DIR}" ] && [ -d "${TEMP_DIR}/mnt" ] && mountpoint -q "${TEMP_DIR}/mnt" 2>/dev/null && \
        fusermount3 -u "${TEMP_DIR}/mnt" 2>/dev/null || fusermount -u "${TEMP_DIR}/mnt" 2>/dev/null || true
    [ -n "${TEMP_DIR}" ] && [ -d "${TEMP_DIR}" ] && rm -rf "${TEMP_DIR}"
    exit ${exit_code}
}

mount_image() {
    local image="$1" mount_point="$2"
    mkdir -p "${mount_point}"
    timeout 60 "${FUSE_BINARY}" -f "${image}" "${mount_point}" &
    local fuse_pid=$! wait_count=0
    while ! mountpoint -q "${mount_point}" 2>/dev/null; do
        sleep 0.1; wait_count=$((wait_count + 1))
        [ ${wait_count} -gt 50 ] && { kill ${fuse_pid} 2>/dev/null; return 1; }
    done
    return 0
}

unmount_image() {
    local mount_point="$1"
    mountpoint -q "${mount_point}" 2>/dev/null && \
        fusermount3 -u "${mount_point}" 2>/dev/null || fusermount -u "${mount_point}" 2>/dev/null || true
}

# Test: Basic sparse file
test_basic_sparse_file() {
    test_start "Basic sparse file"

    local content_dir="${TEMP_DIR}/content" image="${TEMP_DIR}/test.sqfs" mount_point="${TEMP_DIR}/mnt"
    mkdir -p "${content_dir}"

    # Create sparse file with hole at beginning
    dd if=/dev/zero of="${content_dir}/sparse.bin" bs=1K seek=100 count=10 2>/dev/null

    mksquashfs "${content_dir}" "${image}" -noappend -comp gzip > /dev/null 2>&1

    if mount_image "${image}" "${mount_point}"; then
        local size=$(stat -c '%s' "${mount_point}/sparse.bin" 2>/dev/null)
        if [ "${size}" -eq 112640 ]; then  # 100K + 10K = 110K = 112640 bytes
            log_debug "Sparse file size correct: ${size} bytes"
            test_pass
        else
            test_fail "Size incorrect: expected 112640, got ${size}"
        fi
        unmount_image "${mount_point}"
    else
        test_fail "Failed to mount image"
    fi
}

# Test: Hole reads as zeros
test_hole_reads_zeros() {
    test_start "Hole region reads as zeros"

    local content_dir="${TEMP_DIR}/content2" image="${TEMP_DIR}/test.sqfs" mount_point="${TEMP_DIR}/mnt"
    mkdir -p "${content_dir}"

    # Create sparse file: 10K data, 100K hole, 10K data
    dd if=/dev/urandom of="${content_dir}/sparse.bin" bs=1K count=10 2>/dev/null
    dd if=/dev/zero of="${content_dir}/sparse.bin" bs=1K seek=10 count=0 2>/dev/null
    dd if=/dev/urandom of="${content_dir}/sparse.bin" bs=1K seek=110 count=10 conv=notrunc 2>/dev/null

    mksquashfs "${content_dir}" "${image}" -noappend -comp gzip > /dev/null 2>&1

    if mount_image "${image}" "${mount_point}"; then
        # Read from hole region (should be all zeros)
        local hole_data=$(dd if="${mount_point}/sparse.bin" bs=1K skip=50 count=10 2>/dev/null | od -A n -v -t x1 | tr -d ' \n')
        local expected_zeros=$(printf '%0.s00' {1..10240})  # 10K of zeros

        if [ "${hole_data:0:200}" = "${expected_zeros:0:200}" ]; then  # Check first 100 bytes
            log_debug "Hole region contains zeros"
            test_pass
        else
            test_fail "Hole region does not contain zeros"
        fi
        unmount_image "${mount_point}"
    else
        test_fail "Failed to mount image"
    fi
}

# Test: Large sparse file
test_large_sparse_file() {
    test_start "Large sparse file"

    local content_dir="${TEMP_DIR}/content3" image="${TEMP_DIR}/test.sqfs" mount_point="${TEMP_DIR}/mnt"
    mkdir -p "${content_dir}"

    # Create 10MB sparse file with 1MB actual data
    dd if=/dev/urandom of="${content_dir}/large_sparse.bin" bs=1M count=1 2>/dev/null
    dd if=/dev/zero of="${content_dir}/large_sparse.bin" bs=1M seek=10 count=0 2>/dev/null

    mksquashfs "${content_dir}" "${image}" -noappend -comp gzip > /dev/null 2>&1

    if mount_image "${image}" "${mount_point}"; then
        local size=$(stat -c '%s' "${mount_point}/large_sparse.bin" 2>/dev/null)
        if [ "${size}" -eq 10485760 ]; then  # 10 MB
            log_debug "Large sparse file size correct: ${size} bytes"
            test_pass
        else
            test_fail "Size incorrect: expected 10485760, got ${size}"
        fi
        unmount_image "${mount_point}"
    else
        test_fail "Failed to mount image"
    fi
}

# Test: Sparse file with data at end
test_sparse_with_trailing_data() {
    test_start "Sparse file with trailing data"

    local content_dir="${TEMP_DIR}/content4" image="${TEMP_DIR}/test.sqfs" mount_point="${TEMP_DIR}/mnt"
    mkdir -p "${content_dir}"

    # Create file: hole at beginning, data at end
    dd if=/dev/urandom of="${content_dir}/trailing.bin" bs=1K seek=1000 count=10 2>/dev/null

    mksquashfs "${content_dir}" "${image}" -noappend -comp gzip > /dev/null 2>&1

    if mount_image "${image}" "${mount_point}"; then
        # Verify end data is correct
        local end_data=$(dd if="${mount_point}/trailing.bin" bs=1K skip=1000 count=10 2>/dev/null | wc -c)
        if [ "${end_data}" -eq 10240 ]; then
            log_debug "Trailing data correct: ${end_data} bytes"
            test_pass
        else
            test_fail "Trailing data incorrect: expected 10240, got ${end_data}"
        fi
        unmount_image "${mount_point}"
    else
        test_fail "Failed to mount image"
    fi
}

print_summary() {
    echo ""
    echo "================================"
    echo "   SPARSE FILE TEST SUMMARY"
    echo "================================"
    echo "Tests run:    ${TESTS_RUN}"
    echo "Tests passed: ${TESTS_PASSED}"
    echo "Tests failed: ${TESTS_FAILED}"
    echo "================================"
    [ ${TESTS_FAILED} -eq 0 ]
}

main() {
    parse_args "$@"
    echo "================================"
    echo "  SquashFS-FUSE Sparse File Tests"
    echo "================================"
    echo ""

    check_dependencies
    trap cleanup EXIT
    setup

    test_basic_sparse_file
    test_hole_reads_zeros
    test_large_sparse_file
    test_sparse_with_trailing_data

    print_summary
}

main "$@"