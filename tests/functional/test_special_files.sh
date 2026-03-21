#!/bin/bash
#
# test_special_files.sh - Special file types tests for SquashFS-FUSE
#
# Tests: block devices, character devices, FIFO, and socket files
#
# Usage: ./test_special_files.sh [options]
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
NC='\033[0m'

# Test counters
TESTS_RUN=0
TESTS_PASSED=0
TESTS_FAILED=0

# Temporary files
TEMP_DIR=""

# Logging functions
log_info() { echo -e "${GREEN}[INFO]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1"; }
log_debug() { [ "${VERBOSE}" = true ] && echo -e "[DEBUG] $1" || true; }

# Test functions
test_start() { TESTS_RUN=$((TESTS_RUN + 1)); log_info "Test ${TESTS_RUN}: $1"; }
test_pass() { TESTS_PASSED=$((TESTS_PASSED + 1)); log_debug "PASSED"; }
test_fail() { TESTS_FAILED=$((TESTS_FAILED + 1)); log_error "FAILED: $1"; }

# Usage
usage() {
    cat << EOF
Usage: $(basename "$0") [options]

Special file types tests for SquashFS-FUSE.

Options:
  -h, --help              Show help message
  -v, --verbose           Enable verbose output
  -f, --fuse-binary PATH  Path to squashfs-fuse binary

Requirements:
  - mksquashfs with device support
  - root or mknod capability for creating device files
EOF
}

# Parse arguments
parse_args() {
    while [[ $# -gt 0 ]]; do
        case $1 in
            -h|--help) usage; exit 0 ;;
            -v|--verbose) VERBOSE=true; shift ;;
            -f|--fuse-binary) FUSE_BINARY="$2"; shift 2 ;;
            *) log_error "Unknown option: $1"; usage; exit 1 ;;
        esac
    done
}

# Check dependencies
check_dependencies() {
    log_info "Checking dependencies..."

    if [ -z "${FUSE_BINARY}" ]; then
        for path in "${PROJECT_ROOT}/build/squashfs-fuse" "${PROJECT_ROOT}/squashfs-fuse"; do
            [ -x "${path}" ] && FUSE_BINARY="${path}" && break
        done
    fi

    [ -z "${FUSE_BINARY}" ] && { log_error "No FUSE binary found"; exit 2; }
    log_info "Using FUSE binary: ${FUSE_BINARY}"

    command -v mksquashfs &> /dev/null || { log_error "mksquashfs not found"; exit 2; }
}

# Setup
setup() {
    log_info "Setting up test environment..."
    TEMP_DIR=$(mktemp -d -t squashfs-special-test-XXXXXX)
}

# Cleanup
cleanup() {
    local exit_code=$?
    log_info "Cleaning up..."

    # Unmount if mounted
    if [ -n "${TEMP_DIR}" ] && [ -d "${TEMP_DIR}/mnt" ] && mountpoint -q "${TEMP_DIR}/mnt" 2>/dev/null; then
        fusermount3 -u "${TEMP_DIR}/mnt" 2>/dev/null || fusermount -u "${TEMP_DIR}/mnt" 2>/dev/null || true
    fi

    [ -n "${TEMP_DIR}" ] && [ -d "${TEMP_DIR}" ] && rm -rf "${TEMP_DIR}"
    exit ${exit_code}
}

# Mount image
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

    log_debug "Mounted successfully (PID: ${fuse_pid})"
    return 0
}

# Unmount image
unmount_image() {
    local mount_point="$1"
    mountpoint -q "${mount_point}" 2>/dev/null && \
        fusermount3 -u "${mount_point}" 2>/dev/null || \
        fusermount -u "${mount_point}" 2>/dev/null || true
}

# Test: Block device
test_block_device() {
    test_start "Block device support"

    local content_dir="${TEMP_DIR}/content"
    local image="${TEMP_DIR}/test.sqfs"
    local mount_point="${TEMP_DIR}/mnt"

    mkdir -p "${content_dir}"

    # Create block device (requires root or CAP_MKNOD)
    if mknod "${content_dir}/blockdev" b 8 0 2>/dev/null; then
        mksquashfs "${content_dir}" "${image}" -noappend -comp gzip > /dev/null 2>&1

        if mount_image "${image}" "${mount_point}"; then
            if [ -b "${mount_point}/blockdev" ]; then
                log_debug "Block device exists and has correct type"
                test_pass
            else
                test_fail "Block device not recognized"
            fi
            unmount_image "${mount_point}"
        else
            test_fail "Failed to mount image"
        fi
    else
        log_debug "Cannot create block device (requires root/CAP_MKNOD), skipping"
        TESTS_RUN=$((TESTS_RUN - 1))
    fi
}

# Test: Character device
test_char_device() {
    test_start "Character device support"

    local content_dir="${TEMP_DIR}/content2"
    local image="${TEMP_DIR}/test.sqfs"
    local mount_point="${TEMP_DIR}/mnt"

    mkdir -p "${content_dir}"

    # Create character device (requires root or CAP_MKNOD)
    if mknod "${content_dir}/chardev" c 1 3 2>/dev/null; then
        mksquashfs "${content_dir}" "${image}" -noappend -comp gzip > /dev/null 2>&1

        if mount_image "${image}" "${mount_point}"; then
            if [ -c "${mount_point}/chardev" ]; then
                log_debug "Character device exists and has correct type"
                test_pass
            else
                test_fail "Character device not recognized"
            fi
            unmount_image "${mount_point}"
        else
            test_fail "Failed to mount image"
        fi
    else
        log_debug "Cannot create character device (requires root/CAP_MKNOD), skipping"
        TESTS_RUN=$((TESTS_RUN - 1))
    fi
}

# Test: FIFO
test_fifo() {
    test_start "FIFO (named pipe) support"

    local content_dir="${TEMP_DIR}/content3"
    local image="${TEMP_DIR}/test.sqfs"
    local mount_point="${TEMP_DIR}/mnt"

    mkdir -p "${content_dir}"

    # Create FIFO
    if mkfifo "${content_dir}/myfifo"; then
        mksquashfs "${content_dir}" "${image}" -noappend -comp gzip > /dev/null 2>&1

        if mount_image "${image}" "${mount_point}"; then
            if [ -p "${mount_point}/myfifo" ]; then
                log_debug "FIFO exists and has correct type"
                test_pass
            else
                test_fail "FIFO not recognized"
            fi
            unmount_image "${mount_point}"
        else
            test_fail "Failed to mount image"
        fi
    else
        test_fail "Failed to create FIFO"
    fi
}

# Test: Socket
test_socket() {
    test_start "Unix socket support"

    local content_dir="${TEMP_DIR}/content4"
    local image="${TEMP_DIR}/test.sqfs"
    local mount_point="${TEMP_DIR}/mnt"

    mkdir -p "${content_dir}"

    # Create Unix socket
    if python3 -c "import socket; s=socket.socket(socket.AF_UNIX); s.bind('${content_dir}/mysock')" 2>/dev/null; then
        mksquashfs "${content_dir}" "${image}" -noappend -comp gzip > /dev/null 2>&1

        if mount_image "${image}" "${mount_point}"; then
            if [ -S "${mount_point}/mysock" ]; then
                log_debug "Socket exists and has correct type"
                test_pass
            else
                test_fail "Socket not recognized"
            fi
            unmount_image "${mount_point}"
        else
            test_fail "Failed to mount image"
        fi
    else
        log_debug "Cannot create socket, skipping"
        TESTS_RUN=$((TESTS_RUN - 1))
    fi
}

# Test: Device major/minor numbers
test_device_numbers() {
    test_start "Device major/minor numbers preserved"

    local content_dir="${TEMP_DIR}/content5"
    local image="${TEMP_DIR}/test.sqfs"
    local mount_point="${TEMP_DIR}/mnt"

    mkdir -p "${content_dir}"

    # Create device with specific major/minor (requires root)
    if mknod "${content_dir}/tty" c 5 0 2>/dev/null; then
        mksquashfs "${content_dir}" "${image}" -noappend -comp gzip > /dev/null 2>&1

        if mount_image "${image}" "${mount_point}"; then
            local expected_major=5
            local expected_minor=0
            local actual_major=$(stat -c '%t' "${mount_point}/tty" 2>/dev/null)
            local actual_minor=$(stat -c '%T' "${mount_point}/tty" 2>/dev/null)

            # Convert hex to decimal
            actual_major=$((16#${actual_major}))
            actual_minor=$((16#${actual_minor}))

            if [ "${actual_major}" -eq "${expected_major}" ] && [ "${actual_minor}" -eq "${expected_minor}" ]; then
                log_debug "Device numbers correct: major=${actual_major}, minor=${actual_minor}"
                test_pass
            else
                test_fail "Device numbers mismatch: expected ${expected_major}:${expected_minor}, got ${actual_major}:${actual_minor}"
            fi
            unmount_image "${mount_point}"
        else
            test_fail "Failed to mount image"
        fi
    else
        log_debug "Cannot create device (requires root/CAP_MKNOD), skipping"
        TESTS_RUN=$((TESTS_RUN - 1))
    fi
}

# Print summary
print_summary() {
    echo ""
    echo "================================"
    echo "  SPECIAL FILES TEST SUMMARY"
    echo "================================"
    echo "Tests run:    ${TESTS_RUN}"
    echo "Tests passed: ${TESTS_PASSED}"
    echo "Tests failed: ${TESTS_FAILED}"
    echo "================================"

    [ ${TESTS_FAILED} -eq 0 ]
}

# Main
main() {
    parse_args "$@"

    echo "================================"
    echo "  SquashFS-FUSE Special Files Tests"
    echo "================================"
    echo ""

    check_dependencies
    trap cleanup EXIT
    setup

    test_fifo
    test_socket
    test_block_device
    test_char_device
    test_device_numbers

    print_summary
}

main "$@"