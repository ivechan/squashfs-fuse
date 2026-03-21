#!/bin/bash
#
# test_hardlinks.sh - Hard link tests for SquashFS-FUSE
#
# Tests: hard link creation, link count, inode consistency
#
# Usage: ./test_hardlinks.sh [options]
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
NC='\033[0m'

# Test counters
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

usage() {
    cat << EOF
Usage: $(basename "$0") [options]

Hard link tests for SquashFS-FUSE.

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

setup() {
    log_info "Setting up test environment..."
    TEMP_DIR=$(mktemp -d -t squashfs-hardlink-test-XXXXXX)
}

cleanup() {
    local exit_code=$?
    log_info "Cleaning up..."

    [ -n "${TEMP_DIR}" ] && [ -d "${TEMP_DIR}/mnt" ] && mountpoint -q "${TEMP_DIR}/mnt" 2>/dev/null && \
        fusermount3 -u "${TEMP_DIR}/mnt" 2>/dev/null || fusermount -u "${TEMP_DIR}/mnt" 2>/dev/null || true

    [ -n "${TEMP_DIR}" ] && [ -d "${TEMP_DIR}" ] && rm -rf "${TEMP_DIR}"
    exit ${exit_code}
}

mount_image() {
    local image="$1"
    local mount_point="$2"

    mkdir -p "${mount_point}"
    timeout 60 "${FUSE_BINARY}" -f "${image}" "${mount_point}" &
    local fuse_pid=$!

    local wait_count=0
    while ! mountpoint -q "${mount_point}" 2>/dev/null; do
        sleep 0.1
        wait_count=$((wait_count + 1))
        [ ${wait_count} -gt 50 ] && { kill ${fuse_pid} 2>/dev/null; return 1; }
    done
    return 0
}

unmount_image() {
    local mount_point="$1"
    mountpoint -q "${mount_point}" 2>/dev/null && \
        fusermount3 -u "${mount_point}" 2>/dev/null || \
        fusermount -u "${mount_point}" 2>/dev/null || true
}

# Test: Basic hard link
test_basic_hardlink() {
    test_start "Basic hard link"

    local content_dir="${TEMP_DIR}/content"
    local image="${TEMP_DIR}/test.sqfs"
    local mount_point="${TEMP_DIR}/mnt"

    mkdir -p "${content_dir}"
    echo "hardlink test content" > "${content_dir}/original.txt"
    ln "${content_dir}/original.txt" "${content_dir}/link.txt"

    mksquashfs "${content_dir}" "${image}" -noappend -comp gzip > /dev/null 2>&1

    if mount_image "${image}" "${mount_point}"; then
        # Both files should exist
        if [ -f "${mount_point}/original.txt" ] && [ -f "${mount_point}/link.txt" ]; then
            # Content should be identical
            if cmp -s "${mount_point}/original.txt" "${mount_point}/link.txt"; then
                log_debug "Both files exist with identical content"
                test_pass
            else
                test_fail "File contents differ"
            fi
        else
            test_fail "Hard link files not found"
        fi
        unmount_image "${mount_point}"
    else
        test_fail "Failed to mount image"
    fi
}

# Test: Link count
test_link_count() {
    test_start "Link count correct"

    local content_dir="${TEMP_DIR}/content2"
    local image="${TEMP_DIR}/test.sqfs"
    local mount_point="${TEMP_DIR}/mnt"

    mkdir -p "${content_dir}"
    echo "content" > "${content_dir}/file.txt"
    ln "${content_dir}/file.txt" "${content_dir}/link1.txt"
    ln "${content_dir}/file.txt" "${content_dir}/link2.txt"

    mksquashfs "${content_dir}" "${image}" -noappend -comp gzip > /dev/null 2>&1

    if mount_image "${image}" "${mount_point}"; then
        local link_count=$(stat -c '%h' "${mount_point}/file.txt" 2>/dev/null)

        if [ "${link_count}" -eq 3 ]; then
            log_debug "Link count correct: ${link_count}"
            test_pass
        else
            test_fail "Link count incorrect: expected 3, got ${link_count}"
        fi
        unmount_image "${mount_point}"
    else
        test_fail "Failed to mount image"
    fi
}

# Test: Same inode number
test_same_inode() {
    test_start "Same inode number for hard links"

    local content_dir="${TEMP_DIR}/content3"
    local image="${TEMP_DIR}/test.sqfs"
    local mount_point="${TEMP_DIR}/mnt"

    mkdir -p "${content_dir}"
    echo "content" > "${content_dir}/original.txt"
    ln "${content_dir}/original.txt" "${content_dir}/link.txt"

    mksquashfs "${content_dir}" "${image}" -noappend -comp gzip > /dev/null 2>&1

    if mount_image "${image}" "${mount_point}"; then
        local inode1=$(stat -c '%i' "${mount_point}/original.txt" 2>/dev/null)
        local inode2=$(stat -c '%i' "${mount_point}/link.txt" 2>/dev/null)

        if [ "${inode1}" = "${inode2}" ]; then
            log_debug "Both files have same inode: ${inode1}"
            test_pass
        else
            test_fail "Inode numbers differ: ${inode1} vs ${inode2}"
        fi
        unmount_image "${mount_point}"
    else
        test_fail "Failed to mount image"
    fi
}

# Test: Modify through link
test_modify_through_link() {
    test_start "Content modification visible through all links"

    local content_dir="${TEMP_DIR}/content4"
    local image="${TEMP_DIR}/test.sqfs"
    local mount_point="${TEMP_DIR}/mnt"

    mkdir -p "${content_dir}"
    echo "original content" > "${content_dir}/file.txt"
    ln "${content_dir}/file.txt" "${content_dir}/link.txt"

    mksquashfs "${content_dir}" "${image}" -noappend -comp gzip > /dev/null 2>&1

    if mount_image "${image}" "${mount_point}"; then
        # Both should have same content (SquashFS is read-only, so just verify)
        local content1=$(cat "${mount_point}/file.txt")
        local content2=$(cat "${mount_point}/link.txt")

        if [ "${content1}" = "${content2}" ]; then
            log_debug "Content consistent across all links"
            test_pass
        else
            test_fail "Content differs between links"
        fi
        unmount_image "${mount_point}"
    else
        test_fail "Failed to mount image"
    fi
}

# Test: Hard links in subdirectories
test_hardlinks_in_subdirs() {
    test_start "Hard links across subdirectories"

    local content_dir="${TEMP_DIR}/content5"
    local image="${TEMP_DIR}/test.sqfs"
    local mount_point="${TEMP_DIR}/mnt"

    mkdir -p "${content_dir}/dir1" "${content_dir}/dir2"
    echo "shared content" > "${content_dir}/dir1/file.txt"
    ln "${content_dir}/dir1/file.txt" "${content_dir}/dir2/file.txt"

    mksquashfs "${content_dir}" "${image}" -noappend -comp gzip > /dev/null 2>&1

    if mount_image "${image}" "${mount_point}"; then
        local inode1=$(stat -c '%i' "${mount_point}/dir1/file.txt" 2>/dev/null)
        local inode2=$(stat -c '%i' "${mount_point}/dir2/file.txt" 2>/dev/null)

        if [ "${inode1}" = "${inode2}" ]; then
            log_debug "Cross-directory hard links work correctly"
            test_pass
        else
            test_fail "Inode numbers differ across directories"
        fi
        unmount_image "${mount_point}"
    else
        test_fail "Failed to mount image"
    fi
}

print_summary() {
    echo ""
    echo "================================"
    echo "   HARDLINK TEST SUMMARY"
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
    echo "  SquashFS-FUSE Hard Link Tests"
    echo "================================"
    echo ""

    check_dependencies
    trap cleanup EXIT
    setup

    test_basic_hardlink
    test_link_count
    test_same_inode
    test_modify_through_link
    test_hardlinks_in_subdirs

    print_summary
}

main "$@"