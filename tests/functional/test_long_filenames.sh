#!/bin/bash
#
# test_long_filenames.sh - Long filename tests for SquashFS-FUSE
#
# Tests: maximum filename length, path components
#
# Usage: ./test_long_filenames.sh [options]
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

Long filename tests for SquashFS-FUSE.

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

setup() { TEMP_DIR=$(mktemp -d -t squashfs-longname-test-XXXXXX); }

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

# Test: 100 character filename
test_100_char_filename() {
    test_start "100 character filename"

    local content_dir="${TEMP_DIR}/content" image="${TEMP_DIR}/test.sqfs" mount_point="${TEMP_DIR}/mnt"
    mkdir -p "${content_dir}"

    # Create 100 character filename
    local long_name=$(printf 'a%.0s' {1..100})
    echo "test" > "${content_dir}/${long_name}.txt"

    mksquashfs "${content_dir}" "${image}" -noappend -comp gzip > /dev/null 2>&1

    if mount_image "${image}" "${mount_point}"; then
        if [ -f "${mount_point}/${long_name}.txt" ]; then
            log_debug "100 char filename accessible"
            test_pass
        else
            test_fail "100 char filename not accessible"
        fi
        unmount_image "${mount_point}"
    else
        test_fail "Failed to mount image"
    fi
}

# Test: 200 character filename
test_200_char_filename() {
    test_start "200 character filename"

    local content_dir="${TEMP_DIR}/content2" image="${TEMP_DIR}/test.sqfs" mount_point="${TEMP_DIR}/mnt"
    mkdir -p "${content_dir}"

    # Create 200 character filename
    local long_name=$(printf 'b%.0s' {1..200})
    echo "test" > "${content_dir}/${long_name}.txt"

    mksquashfs "${content_dir}" "${image}" -noappend -comp gzip > /dev/null 2>&1

    if mount_image "${image}" "${mount_point}"; then
        if [ -f "${mount_point}/${long_name}.txt" ]; then
            log_debug "200 char filename accessible"
            test_pass
        else
            test_fail "200 char filename not accessible"
        fi
        unmount_image "${mount_point}"
    else
        test_fail "Failed to mount image"
    fi
}

# Test: 255 character filename (maximum)
test_255_char_filename() {
    test_start "255 character filename (maximum)"

    local content_dir="${TEMP_DIR}/content3" image="${TEMP_DIR}/test.sqfs" mount_point="${TEMP_DIR}/mnt"
    mkdir -p "${content_dir}"

    # Create 255 character filename (Linux maximum)
    local long_name=$(printf 'c%.0s' {1..255})
    echo "test" > "${content_dir}/${long_name}"

    mksquashfs "${content_dir}" "${image}" -noappend -comp gzip > /dev/null 2>&1

    if mount_image "${image}" "${mount_point}"; then
        if [ -f "${mount_point}/${long_name}" ]; then
            log_debug "255 char filename accessible"
            test_pass
        else
            test_fail "255 char filename not accessible"
        fi
        unmount_image "${mount_point}"
    else
        test_fail "Failed to mount image"
    fi
}

# Test: Long directory names
test_long_directory_names() {
    test_start "Long directory names"

    local content_dir="${TEMP_DIR}/content4" image="${TEMP_DIR}/test.sqfs" mount_point="${TEMP_DIR}/mnt"
    mkdir -p "${content_dir}"

    # Create directories with long names
    local dir_name=$(printf 'd%.0s' {1..100})
    mkdir -p "${content_dir}/${dir_name}/${dir_name}"
    echo "nested" > "${content_dir}/${dir_name}/${dir_name}/file.txt"

    mksquashfs "${content_dir}" "${image}" -noappend -comp gzip > /dev/null 2>&1

    if mount_image "${image}" "${mount_point}"; then
        if [ -f "${mount_point}/${dir_name}/${dir_name}/file.txt" ]; then
            log_debug "Long directory names work correctly"
            test_pass
        else
            test_fail "Long directory names not accessible"
        fi
        unmount_image "${mount_point}"
    else
        test_fail "Failed to mount image"
    fi
}

# Test: Mixed long and short names
test_mixed_name_lengths() {
    test_start "Mixed long and short filenames"

    local content_dir="${TEMP_DIR}/content5" image="${TEMP_DIR}/test.sqfs" mount_point="${TEMP_DIR}/mnt"
    mkdir -p "${content_dir}"

    # Create files with various name lengths
    echo "short" > "${content_dir}/a"
    echo "medium" > "${content_dir}/medium_name_file.txt"
    local long_name=$(printf 'x%.0s' {1..200})
    echo "long" > "${content_dir}/${long_name}.dat"

    mksquashfs "${content_dir}" "${image}" -noappend -comp gzip > /dev/null 2>&1

    if mount_image "${image}" "${mount_point}"; then
        if [ -f "${mount_point}/a" ] && \
           [ -f "${mount_point}/medium_name_file.txt" ] && \
           [ -f "${mount_point}/${long_name}.dat" ]; then
            log_debug "Mixed name lengths work correctly"
            test_pass
        else
            test_fail "Some files not accessible"
        fi
        unmount_image "${mount_point}"
    else
        test_fail "Failed to mount image"
    fi
}

# Test: Files with same prefix
test_same_prefix() {
    test_start "Files with same long prefix"

    local content_dir="${TEMP_DIR}/content6" image="${TEMP_DIR}/test.sqfs" mount_point="${TEMP_DIR}/mnt"
    mkdir -p "${content_dir}"

    # Create files with same long prefix
    local prefix=$(printf 'prefix%.0s' {1..20})
    echo "a" > "${content_dir}/${prefix}_a.txt"
    echo "b" > "${content_dir}/${prefix}_b.txt"
    echo "c" > "${content_dir}/${prefix}_c.txt"

    mksquashfs "${content_dir}" "${image}" -noappend -comp gzip > /dev/null 2>&1

    if mount_image "${image}" "${mount_point}"; then
        if [ -f "${mount_point}/${prefix}_a.txt" ] && \
           [ -f "${mount_point}/${prefix}_b.txt" ] && \
           [ -f "${mount_point}/${prefix}_c.txt" ]; then
            log_debug "Same prefix files distinguished correctly"
            test_pass
        else
            test_fail "Same prefix files not properly distinguished"
        fi
        unmount_image "${mount_point}"
    else
        test_fail "Failed to mount image"
    fi
}

print_summary() {
    echo ""
    echo "================================"
    echo "   LONG FILENAME TEST SUMMARY"
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
    echo "  SquashFS-FUSE Long Filename Tests"
    echo "================================"
    echo ""

    check_dependencies
    trap cleanup EXIT
    setup

    test_100_char_filename
    test_200_char_filename
    test_255_char_filename
    test_long_directory_names
    test_mixed_name_lengths
    test_same_prefix

    print_summary
}

main "$@"