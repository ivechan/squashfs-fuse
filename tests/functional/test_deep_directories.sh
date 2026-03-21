#!/bin/bash
#
# test_deep_directories.sh - Deep directory hierarchy tests for SquashFS-FUSE
#
# Tests: nested directories, path resolution, maximum depth
#
# Usage: ./test_deep_directories.sh [options]
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

Deep directory hierarchy tests for SquashFS-FUSE.

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

setup() { TEMP_DIR=$(mktemp -d -t squashfs-deep-test-XXXXXX); }

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

# Test: 10 levels of nested directories
test_10_levels() {
    test_start "10 levels of nested directories"

    local content_dir="${TEMP_DIR}/content" image="${TEMP_DIR}/test.sqfs" mount_point="${TEMP_DIR}/mnt"
    mkdir -p "${content_dir}"

    # Create 10 levels of nested directories
    local path="${content_dir}"
    for i in $(seq 1 10); do
        path="${path}/level${i}"
        mkdir -p "${path}"
    done
    echo "deep file" > "${path}/deep.txt"

    mksquashfs "${content_dir}" "${image}" -noappend -comp gzip > /dev/null 2>&1

    if mount_image "${image}" "${mount_point}"; then
        # Verify all levels exist
        local check_path="${mount_point}"
        local success=true
        for i in $(seq 1 10); do
            check_path="${check_path}/level${i}"
            if [ ! -d "${check_path}" ]; then
                success=false
                break
            fi
        done

        if [ "${success}" = true ] && [ -f "${check_path}/deep.txt" ]; then
            log_debug "All 10 directory levels accessible"
            test_pass
        else
            test_fail "Could not access all directory levels"
        fi
        unmount_image "${mount_point}"
    else
        test_fail "Failed to mount image"
    fi
}

# Test: 50 levels of nested directories
test_50_levels() {
    test_start "50 levels of nested directories"

    local content_dir="${TEMP_DIR}/content2" image="${TEMP_DIR}/test.sqfs" mount_point="${TEMP_DIR}/mnt"
    mkdir -p "${content_dir}"

    # Create 50 levels of nested directories
    local path="${content_dir}"
    for i in $(seq 1 50); do
        path="${path}/d${i}"
        mkdir -p "${path}"
    done
    echo "very deep" > "${path}/file.txt"

    mksquashfs "${content_dir}" "${image}" -noappend -comp gzip > /dev/null 2>&1

    if mount_image "${image}" "${mount_point}"; then
        local check_path="${mount_point}"
        local success=true
        for i in $(seq 1 50); do
            check_path="${check_path}/d${i}"
            if [ ! -d "${check_path}" ]; then
                success=false
                log_debug "Failed at level ${i}"
                break
            fi
        done

        if [ "${success}" = true ] && [ -f "${check_path}/file.txt" ]; then
            log_debug "All 50 directory levels accessible"
            test_pass
        else
            test_fail "Could not access all directory levels"
        fi
        unmount_image "${mount_point}"
    else
        test_fail "Failed to mount image"
    fi
}

# Test: Multiple branches at same level
test_multiple_branches() {
    test_start "Multiple branches at same level"

    local content_dir="${TEMP_DIR}/content3" image="${TEMP_DIR}/test.sqfs" mount_point="${TEMP_DIR}/mnt"
    mkdir -p "${content_dir}"

    # Create multiple branches
    for branch in a b c d e; do
        mkdir -p "${content_dir}/${branch}/sub1/sub2"
        echo "branch ${branch}" > "${content_dir}/${branch}/sub1/sub2/file.txt"
    done

    mksquashfs "${content_dir}" "${image}" -noappend -comp gzip > /dev/null 2>&1

    if mount_image "${image}" "${mount_point}"; then
        local success=true
        for branch in a b c d e; do
            if [ ! -f "${mount_point}/${branch}/sub1/sub2/file.txt" ]; then
                success=false
                break
            fi
        done

        if [ "${success}" = true ]; then
            log_debug "All branches accessible"
            test_pass
        else
            test_fail "Some branches not accessible"
        fi
        unmount_image "${mount_point}"
    else
        test_fail "Failed to mount image"
    fi
}

# Test: Files at various depths
test_files_at_various_depths() {
    test_start "Files at various depths"

    local content_dir="${TEMP_DIR}/content4" image="${TEMP_DIR}/test.sqfs" mount_point="${TEMP_DIR}/mnt"
    mkdir -p "${content_dir}"

    # Create files at different depths
    echo "level 0" > "${content_dir}/root.txt"
    mkdir -p "${content_dir}/d1"
    echo "level 1" > "${content_dir}/d1/level1.txt"
    mkdir -p "${content_dir}/d1/d2"
    echo "level 2" > "${content_dir}/d1/d2/level2.txt"
    mkdir -p "${content_dir}/d1/d2/d3"
    echo "level 3" > "${content_dir}/d1/d2/d3/level3.txt"

    mksquashfs "${content_dir}" "${image}" -noappend -comp gzip > /dev/null 2>&1

    if mount_image "${image}" "${mount_point}"; then
        if [ -f "${mount_point}/root.txt" ] && \
           [ -f "${mount_point}/d1/level1.txt" ] && \
           [ -f "${mount_point}/d1/d2/level2.txt" ] && \
           [ -f "${mount_point}/d1/d2/d3/level3.txt" ]; then
            log_debug "Files at all depths accessible"
            test_pass
        else
            test_fail "Some files not accessible"
        fi
        unmount_image "${mount_point}"
    else
        test_fail "Failed to mount image"
    fi
}

# Test: Empty directories at various depths
test_empty_directories() {
    test_start "Empty directories at various depths"

    local content_dir="${TEMP_DIR}/content5" image="${TEMP_DIR}/test.sqfs" mount_point="${TEMP_DIR}/mnt"
    mkdir -p "${content_dir}"

    # Create empty directories at various depths
    mkdir -p "${content_dir}/empty1"
    mkdir -p "${content_dir}/parent/empty2"
    mkdir -p "${content_dir}/parent/child/empty3"

    mksquashfs "${content_dir}" "${image}" -noappend -comp gzip > /dev/null 2>&1

    if mount_image "${image}" "${mount_point}"; then
        if [ -d "${mount_point}/empty1" ] && \
           [ -d "${mount_point}/parent/empty2" ] && \
           [ -d "${mount_point}/parent/child/empty3" ]; then
            # Verify they are empty
            local count1=$(ls -A "${mount_point}/empty1" 2>/dev/null | wc -l)
            local count2=$(ls -A "${mount_point}/parent/empty2" 2>/dev/null | wc -l)
            local count3=$(ls -A "${mount_point}/parent/child/empty3" 2>/dev/null | wc -l)

            if [ "${count1}" -eq 0 ] && [ "${count2}" -eq 0 ] && [ "${count3}" -eq 0 ]; then
                log_debug "Empty directories preserved correctly"
                test_pass
            else
                test_fail "Empty directories not actually empty"
            fi
        else
            test_fail "Empty directories not accessible"
        fi
        unmount_image "${mount_point}"
    else
        test_fail "Failed to mount image"
    fi
}

print_summary() {
    echo ""
    echo "================================"
    echo "   DEEP DIRECTORY TEST SUMMARY"
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
    echo "  SquashFS-FUSE Deep Directory Tests"
    echo "================================"
    echo ""

    check_dependencies
    trap cleanup EXIT
    setup

    test_10_levels
    test_50_levels
    test_multiple_branches
    test_files_at_various_depths
    test_empty_directories

    print_summary
}

main "$@"