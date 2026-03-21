#!/bin/bash
#
# test_export_table.sh - Export table tests for SquashFS-FUSE
#
# Tests: export table support, NFS-style file handles, inode lookup
#
# Usage: ./test_export_table.sh [options]
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

Export table tests for SquashFS-FUSE.

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

setup() { TEMP_DIR=$(mktemp -d -t squashfs-export-test-XXXXXX); }

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

# Test: Image with export table
test_with_export_table() {
    test_start "Image with export table"

    local content_dir="${TEMP_DIR}/content" image="${TEMP_DIR}/test.sqfs" mount_point="${TEMP_DIR}/mnt"
    mkdir -p "${content_dir}"

    # Create test content
    echo "export test" > "${content_dir}/file.txt"
    mkdir -p "${content_dir}/dir"
    echo "nested" > "${content_dir}/dir/nested.txt"

    # Create image WITH export table (-exports flag)
    if mksquashfs "${content_dir}" "${image}" -noappend -comp gzip -exports > /dev/null 2>&1; then
        if mount_image "${image}" "${mount_point}"; then
            if [ -f "${mount_point}/file.txt" ] && [ -f "${mount_point}/dir/nested.txt" ]; then
                log_debug "Export table image mounts correctly"
                test_pass
            else
                test_fail "Files not accessible in export table image"
            fi
            unmount_image "${mount_point}"
        else
            test_fail "Failed to mount export table image"
        fi
    else
        log_warn "Export table creation not supported"
        TESTS_RUN=$((TESTS_RUN - 1))
    fi
}

# Test: Image without export table
test_without_export_table() {
    test_start "Image without export table"

    local content_dir="${TEMP_DIR}/content2" image="${TEMP_DIR}/test.sqfs" mount_point="${TEMP_DIR}/mnt"
    mkdir -p "${content_dir}"

    echo "no export" > "${content_dir}/file.txt"

    # Create image WITHOUT export table (default)
    if mksquashfs "${content_dir}" "${image}" -noappend -comp gzip > /dev/null 2>&1; then
        if mount_image "${image}" "${mount_point}"; then
            if [ -f "${mount_point}/file.txt" ]; then
                log_debug "Non-export table image mounts correctly"
                test_pass
            else
                test_fail "File not accessible in non-export image"
            fi
            unmount_image "${mount_point}"
        else
            test_fail "Failed to mount non-export image"
        fi
    else
        log_warn "Could not create non-export image"
        TESTS_RUN=$((TESTS_RUN - 1))
    fi
}

# Test: Large directory with export table
test_large_directory_export() {
    test_start "Large directory with export table"

    local content_dir="${TEMP_DIR}/content3" image="${TEMP_DIR}/test.sqfs" mount_point="${TEMP_DIR}/mnt"
    mkdir -p "${content_dir}"

    # Create many files
    for i in $(seq 1 200); do
        echo "file ${i}" > "${content_dir}/file${i}.txt"
    done

    if mksquashfs "${content_dir}" "${image}" -noappend -comp gzip -exports > /dev/null 2>&1; then
        if mount_image "${image}" "${mount_point}"; then
            local count=$(ls "${mount_point}" 2>/dev/null | wc -l)
            if [ "${count}" -eq 200 ]; then
                log_debug "Large directory with export table works"
                test_pass
            else
                test_fail "File count mismatch: expected 200, got ${count}"
            fi
            unmount_image "${mount_point}"
        else
            test_fail "Failed to mount large export image"
        fi
    else
        log_warn "Export table creation not supported"
        TESTS_RUN=$((TESTS_RUN - 1))
    fi
}

# Test: Hard links with export table
test_hardlinks_export() {
    test_start "Hard links with export table"

    local content_dir="${TEMP_DIR}/content4" image="${TEMP_DIR}/test.sqfs" mount_point="${TEMP_DIR}/mnt"
    mkdir -p "${content_dir}"

    echo "hardlink content" > "${content_dir}/original.txt"
    ln "${content_dir}/original.txt" "${content_dir}/link.txt"

    if mksquashfs "${content_dir}" "${image}" -noappend -comp gzip -exports > /dev/null 2>&1; then
        if mount_image "${image}" "${mount_point}"; then
            # Both files should have same inode
            local inode1=$(stat -c '%i' "${mount_point}/original.txt" 2>/dev/null)
            local inode2=$(stat -c '%i' "${mount_point}/link.txt" 2>/dev/null)

            if [ "${inode1}" = "${inode2}" ]; then
                log_debug "Hard links with export table work correctly"
                test_pass
            else
                test_fail "Hard link inodes differ"
            fi
            unmount_image "${mount_point}"
        else
            test_fail "Failed to mount hardlink export image"
        fi
    else
        log_warn "Export table creation not supported"
        TESTS_RUN=$((TESTS_RUN - 1))
    fi
}

# Test: Deep path with export table
test_deep_path_export() {
    test_start "Deep path with export table"

    local content_dir="${TEMP_DIR}/content5" image="${TEMP_DIR}/test.sqfs" mount_point="${TEMP_DIR}/mnt"
    mkdir -p "${content_dir}"

    # Create deep directory structure
    local path="${content_dir}"
    for i in $(seq 1 10); do
        path="${path}/level${i}"
        mkdir -p "${path}"
    done
    echo "deep file" > "${path}/deep.txt"

    if mksquashfs "${content_dir}" "${image}" -noappend -comp gzip -exports > /dev/null 2>&1; then
        if mount_image "${image}" "${mount_point}"; then
            # Verify deep path exists
            local check_path="${mount_point}"
            for i in $(seq 1 10); do
                check_path="${check_path}/level${i}"
            done

            if [ -f "${check_path}/deep.txt" ]; then
                log_debug "Deep path with export table works"
                test_pass
            else
                test_fail "Deep path not accessible"
            fi
            unmount_image "${mount_point}"
        else
            test_fail "Failed to mount deep export image"
        fi
    else
        log_warn "Export table creation not supported"
        TESTS_RUN=$((TESTS_RUN - 1))
    fi
}

# Test: Stat on files with export table
test_stat_export() {
    test_start "Stat on files with export table"

    local content_dir="${TEMP_DIR}/content6" image="${TEMP_DIR}/test.sqfs" mount_point="${TEMP_DIR}/mnt"
    mkdir -p "${content_dir}"

    # Create files with different types
    echo "regular" > "${content_dir}/regular.txt"
    mkdir -p "${content_dir}/directory"
    ln -s regular.txt "${content_dir}/symlink"

    if mksquashfs "${content_dir}" "${image}" -noappend -comp gzip -exports > /dev/null 2>&1; then
        if mount_image "${image}" "${mount_point}"; then
            # Verify stat works on all types
            local regular_type=$(stat -c '%F' "${mount_point}/regular.txt" 2>/dev/null)
            local dir_type=$(stat -c '%F' "${mount_point}/directory" 2>/dev/null)
            local link_type=$(stat -c '%F' "${mount_point}/symlink" 2>/dev/null)

            if [[ "${regular_type}" == *"regular"* ]] && \
               [[ "${dir_type}" == *"directory"* ]] && \
               [[ "${link_type}" == *"symbolic"* ]]; then
                log_debug "Stat on export table image works correctly"
                test_pass
            else
                test_fail "Stat types incorrect"
            fi
            unmount_image "${mount_point}"
        else
            test_fail "Failed to mount stat export image"
        fi
    else
        log_warn "Export table creation not supported"
        TESTS_RUN=$((TESTS_RUN - 1))
    fi
}

print_summary() {
    echo ""
    echo "================================"
    echo "   EXPORT TABLE TEST SUMMARY"
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
    echo "  SquashFS-FUSE Export Table Tests"
    echo "================================"
    echo ""

    check_dependencies
    trap cleanup EXIT
    setup

    test_with_export_table
    test_without_export_table
    test_large_directory_export
    test_hardlinks_export
    test_deep_path_export
    test_stat_export

    print_summary
}

main "$@"