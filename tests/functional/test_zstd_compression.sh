#!/bin/bash
#
# test_zstd_compression.sh - Zstd compression tests for SquashFS-FUSE
#
# Tests: zstd compression support, different compression levels
#
# Usage: ./test_zstd_compression.sh [options]
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

Zstd compression tests for SquashFS-FUSE.

Options:
  -h, --help              Show help message
  -v, --verbose           Enable verbose output
  -f, --fuse-binary PATH  Path to squashfs-fuse binary

Requirements:
  - mksquashfs with zstd support
  - zstd command-line tool
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

    # Check zstd support in mksquashfs
    if ! mksquashfs 2>&1 | grep -q "zstd"; then
        log_warn "mksquashfs may not support zstd compression"
    fi
}

setup() { TEMP_DIR=$(mktemp -d -t squashfs-zstd-test-XXXXXX); }

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

# Test: Basic zstd compression
test_basic_zstd() {
    test_start "Basic zstd compression"

    local content_dir="${TEMP_DIR}/content" image="${TEMP_DIR}/test.sqfs" mount_point="${TEMP_DIR}/mnt"
    mkdir -p "${content_dir}"

    # Create test files
    echo "zstd test content" > "${content_dir}/file.txt"
    mkdir -p "${content_dir}/subdir"
    echo "nested" > "${content_dir}/subdir/nested.txt"

    # Create zstd-compressed image
    if mksquashfs "${content_dir}" "${image}" -noappend -comp zstd > /dev/null 2>&1; then
        if mount_image "${image}" "${mount_point}"; then
            if [ -f "${mount_point}/file.txt" ] && [ -f "${mount_point}/subdir/nested.txt" ]; then
                log_debug "zstd compressed image mounted successfully"
                test_pass
            else
                test_fail "Files not accessible in zstd image"
            fi
            unmount_image "${mount_point}"
        else
            test_fail "Failed to mount zstd image"
        fi
    else
        log_warn "zstd compression not available, skipping"
        TESTS_RUN=$((TESTS_RUN - 1))
    fi
}

# Test: zstd with different compression levels
test_zstd_levels() {
    test_start "zstd compression levels"

    local content_dir="${TEMP_DIR}/content2" mount_point="${TEMP_DIR}/mnt"
    mkdir -p "${content_dir}"

    # Create test file
    dd if=/dev/urandom of="${content_dir}/data.bin" bs=1K count=100 2>/dev/null

    local levels="1 10 19"
    local success=true

    for level in ${levels}; do
        local image="${TEMP_DIR}/test_level${level}.sqfs"

        if mksquashfs "${content_dir}" "${image}" -noappend -comp zstd -Xcompression-level ${level} > /dev/null 2>&1; then
            if mount_image "${image}" "${mount_point}"; then
                if [ ! -f "${mount_point}/data.bin" ]; then
                    success=false
                    log_debug "Level ${level} failed"
                fi
                unmount_image "${mount_point}"
            else
                success=false
            fi
        else
            log_warn "Compression level ${level} not supported"
        fi
    done

    if [ "${success}" = true ]; then
        log_debug "All zstd levels work correctly"
        test_pass
    else
        test_fail "Some zstd levels failed"
    fi
}

# Test: zstd vs gzip comparison
test_zstd_vs_gzip() {
    test_start "zstd vs gzip compression"

    local content_dir="${TEMP_DIR}/content3"
    mkdir -p "${content_dir}"

    # Create compressible content
    for i in $(seq 1 100); do
        echo "This is line ${i} of a test file for compression comparison." >> "${content_dir}/large.txt"
    done

    local zstd_image="${TEMP_DIR}/zstd.sqfs"
    local gzip_image="${TEMP_DIR}/gzip.sqfs"

    # Create both compressed images
    if mksquashfs "${content_dir}" "${zstd_image}" -noappend -comp zstd > /dev/null 2>&1 && \
       mksquashfs "${content_dir}" "${gzip_image}" -noappend -comp gzip > /dev/null 2>&1; then

        local zstd_size=$(stat -c '%s' "${zstd_image}")
        local gzip_size=$(stat -c '%s' "${gzip_image}")

        log_debug "zstd size: ${zstd_size}, gzip size: ${gzip_size}"

        # Both should create valid images
        if [ "${zstd_size}" -gt 0 ] && [ "${gzip_size}" -gt 0 ]; then
            test_pass
        else
            test_fail "Invalid image sizes"
        fi
    else
        log_warn "Could not create comparison images"
        TESTS_RUN=$((TESTS_RUN - 1))
    fi
}

# Test: zstd with large files
test_zstd_large_file() {
    test_start "zstd with large file"

    local content_dir="${TEMP_DIR}/content4" image="${TEMP_DIR}/test.sqfs" mount_point="${TEMP_DIR}/mnt"
    mkdir -p "${content_dir}"

    # Create 10MB file
    dd if=/dev/urandom of="${content_dir}/large.bin" bs=1M count=10 2>/dev/null

    if mksquashfs "${content_dir}" "${image}" -noappend -comp zstd > /dev/null 2>&1; then
        if mount_image "${image}" "${mount_point}"; then
            local size=$(stat -c '%s' "${mount_point}/large.bin" 2>/dev/null)
            local expected=10485760

            if [ "${size}" -eq "${expected}" ]; then
                log_debug "Large zstd file reads correctly"
                test_pass
            else
                test_fail "Large file size mismatch"
            fi
            unmount_image "${mount_point}"
        else
            test_fail "Failed to mount large zstd image"
        fi
    else
        log_warn "zstd compression not available, skipping"
        TESTS_RUN=$((TESTS_RUN - 1))
    fi
}

# Test: zstd with many small files
test_zstd_many_files() {
    test_start "zstd with many small files"

    local content_dir="${TEMP_DIR}/content5" image="${TEMP_DIR}/test.sqfs" mount_point="${TEMP_DIR}/mnt"
    mkdir -p "${content_dir}"

    # Create many small files
    for i in $(seq 1 100); do
        echo "file ${i}" > "${content_dir}/file${i}.txt"
    done

    if mksquashfs "${content_dir}" "${image}" -noappend -comp zstd > /dev/null 2>&1; then
        if mount_image "${image}" "${mount_point}"; then
            local count=$(ls "${mount_point}" 2>/dev/null | wc -l)

            if [ "${count}" -eq 100 ]; then
                log_debug "All files accessible in zstd image"
                test_pass
            else
                test_fail "File count mismatch: expected 100, got ${count}"
            fi
            unmount_image "${mount_point}"
        else
            test_fail "Failed to mount zstd image"
        fi
    else
        log_warn "zstd compression not available, skipping"
        TESTS_RUN=$((TESTS_RUN - 1))
    fi
}

# Test: zstd with symlinks
test_zstd_symlinks() {
    test_start "zstd with symbolic links"

    local content_dir="${TEMP_DIR}/content6" image="${TEMP_DIR}/test.sqfs" mount_point="${TEMP_DIR}/mnt"
    mkdir -p "${content_dir}"

    echo "target" > "${content_dir}/target.txt"
    ln -s target.txt "${content_dir}/link.txt"

    if mksquashfs "${content_dir}" "${image}" -noappend -comp zstd > /dev/null 2>&1; then
        if mount_image "${image}" "${mount_point}"; then
            if [ -L "${mount_point}/link.txt" ]; then
                local content=$(cat "${mount_point}/link.txt")
                if [ "${content}" = "target" ]; then
                    log_debug "zstd preserves symlinks correctly"
                    test_pass
                else
                    test_fail "Symlink content incorrect"
                fi
            else
                test_fail "Symlink not recognized"
            fi
            unmount_image "${mount_point}"
        else
            test_fail "Failed to mount zstd image"
        fi
    else
        log_warn "zstd compression not available, skipping"
        TESTS_RUN=$((TESTS_RUN - 1))
    fi
}

print_summary() {
    echo ""
    echo "================================"
    echo "   ZSTD COMPRESSION TEST SUMMARY"
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
    echo "  SquashFS-FUSE Zstd Compression Tests"
    echo "================================"
    echo ""

    check_dependencies
    trap cleanup EXIT
    setup

    test_basic_zstd
    test_zstd_levels
    test_zstd_vs_gzip
    test_zstd_large_file
    test_zstd_many_files
    test_zstd_symlinks

    print_summary
}

main "$@"