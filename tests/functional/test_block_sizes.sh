#!/bin/bash
#
# test_block_sizes.sh - Block size tests for SquashFS-FUSE
#
# Tests: various block sizes (4K to 1MB), block alignment
#
# Usage: ./test_block_sizes.sh [options]
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
YELLOW='\033[1;33m'
NC='\033[0m'

TESTS_RUN=0
TESTS_PASSED=0
TESTS_FAILED=0
TEMP_DIR=""

log_info() { echo -e "${GREEN}[INFO]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }
log_warn() { echo -e "${YELLOW}[WARN]${NC} $1"; }
log_debug() { [ "${VERBOSE}" = true ] && echo -e "[DEBUG] $1" || true; }

test_start() { TESTS_RUN=$((TESTS_RUN + 1)); log_info "Test ${TESTS_RUN}: $1"; }
test_pass() { TESTS_PASSED=$((TESTS_PASSED + 1)); log_debug "PASSED"; }
test_fail() { TESTS_FAILED=$((TESTS_FAILED + 1)); log_error "FAILED: $1"; }

usage() { cat << EOF
Usage: $(basename "$0") [options]

Block size tests for SquashFS-FUSE.

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

setup() { TEMP_DIR=$(mktemp -d -t squashfs-blocksize-test-XXXXXX); }

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

# Test: 4K block size
test_4k_block() {
    test_start "4K block size"

    local content_dir="${TEMP_DIR}/content" image="${TEMP_DIR}/test.sqfs" mount_point="${TEMP_DIR}/mnt"
    mkdir -p "${content_dir}"

    dd if=/dev/urandom of="${content_dir}/file.bin" bs=1K count=10 2>/dev/null

    if mksquashfs "${content_dir}" "${image}" -noappend -comp gzip -b 4096 > /dev/null 2>&1; then
        if mount_image "${image}" "${mount_point}"; then
            if [ -f "${mount_point}/file.bin" ]; then
                test_pass
            else
                test_fail "4K block size file not accessible"
            fi
            unmount_image "${mount_point}"
        else
            test_fail "Failed to mount 4K block image"
        fi
    else
        log_warn "4K block size not supported"
        TESTS_RUN=$((TESTS_RUN - 1))
    fi
}

# Test: 128K block size (default)
test_128k_block() {
    test_start "128K block size (default)"

    local content_dir="${TEMP_DIR}/content2" image="${TEMP_DIR}/test.sqfs" mount_point="${TEMP_DIR}/mnt"
    mkdir -p "${content_dir}"

    dd if=/dev/urandom of="${content_dir}/file.bin" bs=1K count=500 2>/dev/null

    if mksquashfs "${content_dir}" "${image}" -noappend -comp gzip -b 131072 > /dev/null 2>&1; then
        if mount_image "${image}" "${mount_point}"; then
            if [ -f "${mount_point}/file.bin" ]; then
                test_pass
            else
                test_fail "128K block size file not accessible"
            fi
            unmount_image "${mount_point}"
        else
            test_fail "Failed to mount 128K block image"
        fi
    else
        log_warn "128K block size not supported"
        TESTS_RUN=$((TESTS_RUN - 1))
    fi
}

# Test: 512K block size
test_512k_block() {
    test_start "512K block size"

    local content_dir="${TEMP_DIR}/content3" image="${TEMP_DIR}/test.sqfs" mount_point="${TEMP_DIR}/mnt"
    mkdir -p "${content_dir}"

    dd if=/dev/urandom of="${content_dir}/file.bin" bs=1K count=1000 2>/dev/null

    if mksquashfs "${content_dir}" "${image}" -noappend -comp gzip -b 524288 > /dev/null 2>&1; then
        if mount_image "${image}" "${mount_point}"; then
            if [ -f "${mount_point}/file.bin" ]; then
                test_pass
            else
                test_fail "512K block size file not accessible"
            fi
            unmount_image "${mount_point}"
        else
            test_fail "Failed to mount 512K block image"
        fi
    else
        log_warn "512K block size not supported"
        TESTS_RUN=$((TESTS_RUN - 1))
    fi
}

# Test: 1MB block size (maximum)
test_1mb_block() {
    test_start "1MB block size (maximum)"

    local content_dir="${TEMP_DIR}/content4" image="${TEMP_DIR}/test.sqfs" mount_point="${TEMP_DIR}/mnt"
    mkdir -p "${content_dir}"

    dd if=/dev/urandom of="${content_dir}/file.bin" bs=1M count=3 2>/dev/null

    if mksquashfs "${content_dir}" "${image}" -noappend -comp gzip -b 1048576 > /dev/null 2>&1; then
        if mount_image "${image}" "${mount_point}"; then
            if [ -f "${mount_point}/file.bin" ]; then
                test_pass
            else
                test_fail "1MB block size file not accessible"
            fi
            unmount_image "${mount_point}"
        else
            test_fail "Failed to mount 1MB block image"
        fi
    else
        log_warn "1MB block size not supported"
        TESTS_RUN=$((TESTS_RUN - 1))
    fi
}

# Test: Fragment blocks with small files
test_fragment_blocks() {
    test_start "Fragment blocks with small files"

    local content_dir="${TEMP_DIR}/content5" image="${TEMP_DIR}/test.sqfs" mount_point="${TEMP_DIR}/mnt"
    mkdir -p "${content_dir}"

    # Create files smaller than block size (should go to fragments)
    for i in $(seq 1 20); do
        echo "small file ${i}" > "${content_dir}/small${i}.txt"
    done

    if mksquashfs "${content_dir}" "${image}" -noappend -comp gzip -b 131072 > /dev/null 2>&1; then
        if mount_image "${image}" "${mount_point}"; then
            local count=$(ls "${mount_point}" 2>/dev/null | wc -l)
            if [ "${count}" -eq 20 ]; then
                test_pass
            else
                test_fail "Fragment files count mismatch: expected 20, got ${count}"
            fi
            unmount_image "${mount_point}"
        else
            test_fail "Failed to mount image with fragments"
        fi
    else
        log_warn "Could not create image with fragments"
        TESTS_RUN=$((TESTS_RUN - 1))
    fi
}

# Test: Mixed block sizes in same image
test_file_spanning_blocks() {
    test_start "File spanning multiple blocks"

    local content_dir="${TEMP_DIR}/content6" image="${TEMP_DIR}/test.sqfs" mount_point="${TEMP_DIR}/mnt"
    mkdir -p "${content_dir}"

    # Create file larger than 128K block
    dd if=/dev/urandom of="${content_dir}/large.bin" bs=1K count=300 2>/dev/null

    if mksquashfs "${content_dir}" "${image}" -noappend -comp gzip -b 131072 > /dev/null 2>&1; then
        if mount_image "${image}" "${mount_point}"; then
            local size=$(stat -c '%s' "${mount_point}/large.bin" 2>/dev/null)
            if [ "${size}" -eq 307200 ]; then
                test_pass
            else
                test_fail "Multi-block file size incorrect"
            fi
            unmount_image "${mount_point}"
        else
            test_fail "Failed to mount multi-block image"
        fi
    else
        log_warn "Could not create multi-block image"
        TESTS_RUN=$((TESTS_RUN - 1))
    fi
}

# Test: Block alignment edge cases
test_block_alignment() {
    test_start "Block alignment edge cases"

    local content_dir="${TEMP_DIR}/content7" image="${TEMP_DIR}/test.sqfs" mount_point="${TEMP_DIR}/mnt"
    mkdir -p "${content_dir}"

    # Create files at exact block boundaries
    dd if=/dev/urandom of="${content_dir}/exact.bin" bs=131072 count=2 2>/dev/null
    dd if=/dev/urandom of="${content_dir}/partial.bin" bs=1K count=100 2>/dev/null

    if mksquashfs "${content_dir}" "${image}" -noappend -comp gzip -b 131072 > /dev/null 2>&1; then
        if mount_image "${image}" "${mount_point}"; then
            local exact_size=$(stat -c '%s' "${mount_point}/exact.bin" 2>/dev/null)
            local partial_size=$(stat -c '%s' "${mount_point}/partial.bin" 2>/dev/null)

            if [ "${exact_size}" -eq 262144 ] && [ "${partial_size}" -eq 102400 ]; then
                test_pass
            else
                test_fail "Block alignment sizes incorrect"
            fi
            unmount_image "${mount_point}"
        else
            test_fail "Failed to mount alignment test image"
        fi
    else
        log_warn "Could not create alignment test image"
        TESTS_RUN=$((TESTS_RUN - 1))
    fi
}

print_summary() {
    echo ""
    echo "================================"
    echo "   BLOCK SIZE TEST SUMMARY"
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
    echo "  SquashFS-FUSE Block Size Tests"
    echo "================================"
    echo ""

    check_dependencies
    trap cleanup EXIT
    setup

    test_4k_block
    test_128k_block
    test_512k_block
    test_1mb_block
    test_fragment_blocks
    test_file_spanning_blocks
    test_block_alignment

    print_summary
}

main "$@"