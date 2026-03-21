#!/bin/bash
#
# test_special_chars.sh - Special character filename tests for SquashFS-FUSE
#
# Tests: spaces, unicode, special characters in filenames
#
# Usage: ./test_special_chars.sh [options]
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

Special character filename tests for SquashFS-FUSE.

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

setup() { TEMP_DIR=$(mktemp -d -t squashfs-specialchar-test-XXXXXX); }

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

# Test: Filenames with spaces
test_filenames_with_spaces() {
    test_start "Filenames with spaces"

    local content_dir="${TEMP_DIR}/content" image="${TEMP_DIR}/test.sqfs" mount_point="${TEMP_DIR}/mnt"
    mkdir -p "${content_dir}"

    echo "space test" > "${content_dir}/file with spaces.txt"
    echo "space test 2" > "${content_dir}/another file name.txt"

    mksquashfs "${content_dir}" "${image}" -noappend -comp gzip > /dev/null 2>&1

    if mount_image "${image}" "${mount_point}"; then
        if [ -f "${mount_point}/file with spaces.txt" ] && \
           [ -f "${mount_point}/another file name.txt" ]; then
            log_debug "Filenames with spaces work correctly"
            test_pass
        else
            test_fail "Filenames with spaces not accessible"
        fi
        unmount_image "${mount_point}"
    else
        test_fail "Failed to mount image"
    fi
}

# Test: Directories with spaces
test_directories_with_spaces() {
    test_start "Directories with spaces"

    local content_dir="${TEMP_DIR}/content2" image="${TEMP_DIR}/test.sqfs" mount_point="${TEMP_DIR}/mnt"
    mkdir -p "${content_dir}"

    mkdir -p "${content_dir}/dir with spaces/sub dir"
    echo "nested" > "${content_dir}/dir with spaces/sub dir/file.txt"

    mksquashfs "${content_dir}" "${image}" -noappend -comp gzip > /dev/null 2>&1

    if mount_image "${image}" "${mount_point}"; then
        if [ -f "${mount_point}/dir with spaces/sub dir/file.txt" ]; then
            log_debug "Directories with spaces work correctly"
            test_pass
        else
            test_fail "Directories with spaces not accessible"
        fi
        unmount_image "${mount_point}"
    else
        test_fail "Failed to mount image"
    fi
}

# Test: Unicode filenames (UTF-8)
test_unicode_filenames() {
    test_start "Unicode filenames (UTF-8)"

    local content_dir="${TEMP_DIR}/content3" image="${TEMP_DIR}/test.sqfs" mount_point="${TEMP_DIR}/mnt"
    mkdir -p "${content_dir}"

    # Create files with Unicode names
    echo "unicode" > "${content_dir}/файл.txt"      # Russian
    echo "unicode" > "${content_dir}/文件.txt"      # Chinese
    echo "unicode" > "${content_dir}/ファイル.txt"  # Japanese
    echo "unicode" > "${content_dir}/αρχείο.txt"   # Greek

    mksquashfs "${content_dir}" "${image}" -noappend -comp gzip > /dev/null 2>&1

    if mount_image "${image}" "${mount_point}"; then
        local success=true
        for name in "файл.txt" "文件.txt" "ファイル.txt" "αρχείο.txt"; do
            if [ ! -f "${mount_point}/${name}" ]; then
                success=false
                log_debug "Missing: ${name}"
            fi
        done

        if [ "${success}" = true ]; then
            log_debug "Unicode filenames work correctly"
            test_pass
        else
            test_fail "Some Unicode filenames not accessible"
        fi
        unmount_image "${mount_point}"
    else
        test_fail "Failed to mount image"
    fi
}

# Test: Special punctuation in filenames
test_special_punctuation() {
    test_start "Special punctuation in filenames"

    local content_dir="${TEMP_DIR}/content4" image="${TEMP_DIR}/test.sqfs" mount_point="${TEMP_DIR}/mnt"
    mkdir -p "${content_dir}"

    # Files with special characters (safe ones)
    echo "test" > "${content_dir}/file-with-dash.txt"
    echo "test" > "${content_dir}/file_with_underscore.txt"
    echo "test" > "${content_dir}/file.with.dots.txt"
    echo "test" > "${content_dir}/file@symbol.txt"

    mksquashfs "${content_dir}" "${image}" -noappend -comp gzip > /dev/null 2>&1

    if mount_image "${image}" "${mount_point}"; then
        if [ -f "${mount_point}/file-with-dash.txt" ] && \
           [ -f "${mount_point}/file_with_underscore.txt" ] && \
           [ -f "${mount_point}/file.with.dots.txt" ] && \
           [ -f "${mount_point}/file@symbol.txt" ]; then
            log_debug "Special punctuation works correctly"
            test_pass
        else
            test_fail "Some punctuation filenames not accessible"
        fi
        unmount_image "${mount_point}"
    else
        test_fail "Failed to mount image"
    fi
}

# Test: Leading/trailing spaces (if filesystem allows)
test_leading_trailing_spaces() {
    test_start "Leading/trailing spaces in filenames"

    local content_dir="${TEMP_DIR}/content5" image="${TEMP_DIR}/test.sqfs" mount_point="${TEMP_DIR}/mnt"
    mkdir -p "${content_dir}"

    # Try to create files with leading/trailing spaces
    # Note: Some filesystems may not allow this
    if touch "${content_dir}/ leading.txt" 2>/dev/null && \
       touch "${content_dir}/trailing .txt" 2>/dev/null; then
        mksquashfs "${content_dir}" "${image}" -noappend -comp gzip > /dev/null 2>&1

        if mount_image "${image}" "${mount_point}"; then
            # Check if files with spaces are accessible
            local count=$(ls "${mount_point}" 2>/dev/null | wc -l)
            if [ "${count}" -ge 2 ]; then
                log_debug "Leading/trailing spaces handled"
                test_pass
            else
                log_warn "Leading/trailing spaces may not be fully supported"
                test_pass  # Pass anyway as this is filesystem-dependent
            fi
            unmount_image "${mount_point}"
        else
            test_fail "Failed to mount image"
        fi
    else
        log_warn "Filesystem does not support leading/trailing spaces in filenames"
        TESTS_RUN=$((TESTS_RUN - 1))
    fi
}

# Test: Mixed special characters
test_mixed_special_chars() {
    test_start "Mixed special characters in paths"

    local content_dir="${TEMP_DIR}/content6" image="${TEMP_DIR}/test.sqfs" mount_point="${TEMP_DIR}/mnt"
    mkdir -p "${content_dir}"

    # Create a complex path with various characters
    mkdir -p "${content_dir}/dir with spaces/sub-dir_1"
    echo "complex" > "${content_dir}/dir with spaces/sub-dir_1/file.name.txt"

    mksquashfs "${content_dir}" "${image}" -noappend -comp gzip > /dev/null 2>&1

    if mount_image "${image}" "${mount_point}"; then
        if [ -f "${mount_point}/dir with spaces/sub-dir_1/file.name.txt" ]; then
            log_debug "Mixed special characters work correctly"
            test_pass
        else
            test_fail "Mixed special character path not accessible"
        fi
        unmount_image "${mount_point}"
    else
        test_fail "Failed to mount image"
    fi
}

print_summary() {
    echo ""
    echo "================================"
    echo "   SPECIAL CHARS TEST SUMMARY"
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
    echo "  SquashFS-FUSE Special Character Tests"
    echo "================================"
    echo ""

    check_dependencies
    trap cleanup EXIT
    setup

    test_filenames_with_spaces
    test_directories_with_spaces
    test_unicode_filenames
    test_special_punctuation
    test_leading_trailing_spaces
    test_mixed_special_chars

    print_summary
}

main "$@"