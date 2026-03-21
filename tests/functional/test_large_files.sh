#!/bin/bash
#
# test_large_files.sh - Large file tests for SquashFS-FUSE
#
# Tests: files > 4GB, 64-bit file size support, random access
#
# Usage: ./test_large_files.sh [options]
#   -h, --help              Show help message
#   -v, --verbose           Enable verbose output
#   -f, --fuse-binary PATH  Path to squashfs-fuse binary
#   --quick                 Skip large file tests (for quick testing)
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

VERBOSE=false
QUICK_MODE=false
FUSE_BINARY=""

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

TESTS_RUN=0
TESTS_PASSED=0
TESTS_FAILED=0
TEMP_DIR=""

log_info() { echo -e "${GREEN}[INFO]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }
log_debug() { [ "${VERBOSE}" = true ] && echo -e "[DEBUG] $1" || true; }

test_start() { TESTS_RUN=$((TESTS_RUN + 1)); log_info "Test ${TESTS_RUN}: $1"; }
test_pass() { TESTS_PASSED=$((TESTS_PASSED + 1)); log_debug "PASSED"; }
test_fail() { TESTS_FAILED=$((TESTS_FAILED + 1)); log_error "FAILED: $1"; }

usage() { cat << EOF
Usage: $(basename "$0") [options]

Large file tests for SquashFS-FUSE.

Options:
  -h, --help              Show help message
  -v, --verbose           Enable verbose output
  -f, --fuse-binary PATH  Path to squashfs-fuse binary
  --quick                 Skip >4GB file tests (for quick testing)
EOF
}

parse_args() {
    while [[ $# -gt 0 ]]; do
        case $1 in
            -h|--help) usage; exit 0 ;;
            -v|--verbose) VERBOSE=true; shift ;;
            -f|--fuse-binary) FUSE_BINARY="$2"; shift 2 ;;
            --quick) QUICK_MODE=true; shift ;;
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

setup() { TEMP_DIR=$(mktemp -d -t squashfs-large-test-XXXXXX); }

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
    timeout 120 "${FUSE_BINARY}" -f "${image}" "${mount_point}" &
    local fuse_pid=$! wait_count=0
    while ! mountpoint -q "${mount_point}" 2>/dev/null; do
        sleep 0.1; wait_count=$((wait_count + 1))
        [ ${wait_count} -gt 100 ] && { kill ${fuse_pid} 2>/dev/null; return 1; }
    done
    return 0
}

unmount_image() {
    local mount_point="$1"
    mountpoint -q "${mount_point}" 2>/dev/null && \
        fusermount3 -u "${mount_point}" 2>/dev/null || fusermount -u "${mount_point}" 2>/dev/null || true
}

# Test: File approaching 4GB (extended file inode)
test_near_4gb_file() {
    test_start "File approaching 4GB boundary"

    if [ "${QUICK_MODE}" = true ]; then
        log_warn "Skipping large file test in quick mode"
        TESTS_RUN=$((TESTS_RUN - 1))
        return
    fi

    local content_dir="${TEMP_DIR}/content" image="${TEMP_DIR}/test.sqfs" mount_point="${TEMP_DIR}/mnt"
    mkdir -p "${content_dir}"

    # Create 3.5GB file (under 4GB but still large)
    log_info "Creating 3.5GB test file (this may take a moment)..."
    dd if=/dev/urandom of="${content_dir}/large.bin" bs=1M count=3584 2>/dev/null

    mksquashfs "${content_dir}" "${image}" -noappend -comp gzip > /dev/null 2>&1

    if mount_image "${image}" "${mount_point}"; then
        local size=$(stat -c '%s' "${mount_point}/large.bin" 2>/dev/null)
        local expected=3758096384  # 3.5GB

        if [ "${size}" -eq "${expected}" ]; then
            log_debug "File size correct: ${size} bytes"
            test_pass
        else
            test_fail "Size incorrect: expected ${expected}, got ${size}"
        fi
        unmount_image "${mount_point}"
    else
        test_fail "Failed to mount image"
    fi
}

# Test: File over 4GB (requires extended inode)
test_over_4gb_file() {
    test_start "File over 4GB (extended inode)"

    if [ "${QUICK_MODE}" = true ]; then
        log_warn "Skipping >4GB file test in quick mode"
        TESTS_RUN=$((TESTS_RUN - 1))
        return
    fi

    local content_dir="${TEMP_DIR}/content2" image="${TEMP_DIR}/test.sqfs" mount_point="${TEMP_DIR}/mnt"
    mkdir -p "${content_dir}"

    # Create 5GB file
    log_info "Creating 5GB test file (this may take a moment)..."
    dd if=/dev/urandom of="${content_dir}/huge.bin" bs=1M count=5120 2>/dev/null

    mksquashfs "${content_dir}" "${image}" -noappend -comp gzip > /dev/null 2>&1

    if mount_image "${image}" "${mount_point}"; then
        local size=$(stat -c '%s' "${mount_point}/huge.bin" 2>/dev/null)
        local expected=5368709120  # 5GB

        if [ "${size}" -eq "${expected}" ]; then
            log_debug "File size correct: ${size} bytes (uses extended inode)"
            test_pass
        else
            test_fail "Size incorrect: expected ${expected}, got ${size}"
        fi
        unmount_image "${mount_point}"
    else
        test_fail "Failed to mount image"
    fi
}

# Test: Random access in large file
test_random_access_large() {
    test_start "Random access in large file"

    local content_dir="${TEMP_DIR}/content3" image="${TEMP_DIR}/test.sqfs" mount_point="${TEMP_DIR}/mnt"
    mkdir -p "${content_dir}"

    # Create 100MB file with known pattern
    dd if=/dev/urandom of="${content_dir}/medium.bin" bs=1M count=100 2>/dev/null

    mksquashfs "${content_dir}" "${image}" -noappend -comp gzip > /dev/null 2>&1

    if mount_image "${image}" "${mount_point}"; then
        # Read from various offsets
        local offsets="0 1048576 52428800 99614720"  # 0, 1MB, 50MB, 95MB
        local success=true

        for offset in ${offsets}; do
            dd if="${mount_point}/medium.bin" bs=1 count=1024 skip=${offset} of=/dev/null 2>/dev/null || success=false
        done

        if [ "${success}" = true ]; then
            log_debug "Random access at all offsets successful"
            test_pass
        else
            test_fail "Random access failed at some offset"
        fi
        unmount_image "${mount_point}"
    else
        test_fail "Failed to mount image"
    fi
}

# Test: Read end of large file
test_read_file_end() {
    test_start "Read at end of large file"

    local content_dir="${TEMP_DIR}/content4" image="${TEMP_DIR}/test.sqfs" mount_point="${TEMP_DIR}/mnt"
    mkdir -p "${content_dir}"

    # Create 50MB file
    dd if=/dev/urandom of="${content_dir}/test.bin" bs=1M count=50 2>/dev/null

    mksquashfs "${content_dir}" "${image}" -noappend -comp gzip > /dev/null 2>&1

    if mount_image "${image}" "${mount_point}"; then
        local size=$(stat -c '%s' "${mount_point}/test.bin" 2>/dev/null)

        # Read last 1KB
        local offset=$((size - 1024))
        dd if="${mount_point}/test.bin" bs=1 skip=${offset} count=1024 of=/dev/null 2>/dev/null

        if [ $? -eq 0 ]; then
            log_debug "Read at file end successful"
            test_pass
        else
            test_fail "Failed to read at file end"
        fi
        unmount_image "${mount_point}"
    else
        test_fail "Failed to mount image"
    fi
}

print_summary() {
    echo ""
    echo "================================"
    echo "   LARGE FILE TEST SUMMARY"
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
    echo "  SquashFS-FUSE Large File Tests"
    echo "================================"
    echo ""

    check_dependencies
    trap cleanup EXIT
    setup

    test_near_4gb_file
    test_over_4gb_file
    test_random_access_large
    test_read_file_end

    print_summary
}

main "$@"