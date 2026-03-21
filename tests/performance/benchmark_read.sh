#!/bin/bash
#
# benchmark_read.sh - Performance benchmark for SquashFS-FUSE
#
# This script benchmarks read performance:
# - Sequential read throughput (using dd)
# - Random read IOPS (using fio if available)
# - Records throughput metrics
#
# Usage: ./benchmark_read.sh [options]
#   -h, --help              Show help message
#   -i, --image PATH        Use existing SquashFS image
#   -m, --mount PATH        Mount point (default: auto-create)
#   -f, --fuse-binary PATH  Path to squashfs-fuse binary
#   -s, --size SIZE         Test file size (default: 100M)
#   -o, --output FILE       Output results to file
#   --keep                  Keep temporary files
#
# Output format:
#   Sequential Read: XXX MB/s
#   Random Read 4K: XXX IOPS
#   Random Read 64K: XXX IOPS
#

set -e

# Configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
DEFAULT_SIZE="100M"

# Default values
VERBOSE=false
KEEP_TEMP=false
TEST_IMAGE=""
MOUNT_POINT=""
FUSE_BINARY=""
TEST_SIZE="${DEFAULT_SIZE}"
OUTPUT_FILE=""

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

# Temporary files
TEMP_DIR=""
TEMP_IMAGE=""

# Results
RESULTS=()

# Logging functions
log_info() {
    echo -e "${GREEN}[INFO]${NC} $1"
}

log_warn() {
    echo -e "${YELLOW}[WARN]${NC} $1"
}

log_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

log_debug() {
    if [ "${VERBOSE}" = true ]; then
        echo -e "${BLUE}[DEBUG]${NC} $1"
    fi
}

# Usage information
usage() {
    cat << EOF
Usage: $(basename "$0") [options]

Performance benchmark for SquashFS-FUSE read operations.

Options:
  -h, --help              Show this help message
  -v, --verbose           Enable verbose output
  -i, --image PATH        Use existing SquashFS image
  -m, --mount PATH        Mount point (default: auto-create)
  -f, --fuse-binary PATH  Path to squashfs-fuse binary
  -s, --size SIZE         Test file size (default: ${DEFAULT_SIZE})
  -o, --output FILE       Output results to file
  --keep                  Keep temporary files

Examples:
  $(basename "$0")                          # Run with defaults
  $(basename "$0") -s 500M                  # Test with 500MB file
  $(basename "$0") -i test.sqfs             # Use existing image
  $(basename "$0") -o results.txt           # Save results to file
EOF
}

# Parse command line arguments
parse_args() {
    while [[ $# -gt 0 ]]; do
        case $1 in
            -h|--help)
                usage
                exit 0
                ;;
            -v|--verbose)
                VERBOSE=true
                shift
                ;;
            -i|--image)
                TEST_IMAGE="$2"
                shift 2
                ;;
            -m|--mount)
                MOUNT_POINT="$2"
                shift 2
                ;;
            -f|--fuse-binary)
                FUSE_BINARY="$2"
                shift 2
                ;;
            -s|--size)
                TEST_SIZE="$2"
                shift 2
                ;;
            -o|--output)
                OUTPUT_FILE="$2"
                shift 2
                ;;
            --keep)
                KEEP_TEMP=true
                shift
                ;;
            *)
                log_error "Unknown option: $1"
                usage
                exit 1
                ;;
        esac
    done
}

# Check for required dependencies
check_dependencies() {
    log_info "Checking dependencies..."

    # Check dd
    if ! command -v dd &> /dev/null; then
        log_error "dd not found"
        exit 2
    fi

    # Check mksquashfs
    if ! command -v mksquashfs &> /dev/null; then
        log_error "mksquashfs not found"
        exit 2
    fi

    # Check for fio (optional)
    if command -v fio &> /dev/null; then
        HAS_FIO=true
        log_debug "Found fio: $(fio --version 2>&1 | head -n1)"
    else
        HAS_FIO=false
        log_warn "fio not found, random I/O benchmarks will be skipped"
    fi

    # Find FUSE binary
    if [ -z "${FUSE_BINARY}" ]; then
        for path in "${PROJECT_ROOT}/build/squashfs-fuse" \
                    "${PROJECT_ROOT}/squashfs-fuse" \
                    "./squashfs-fuse"; do
            if [ -x "${path}" ]; then
                FUSE_BINARY="${path}"
                break
            fi
        done
    fi

    if [ -z "${FUSE_BINARY}" ]; then
        log_error "No FUSE binary found. Specify with -f option."
        exit 2
    fi

    if [ ! -x "${FUSE_BINARY}" ]; then
        log_error "FUSE binary not executable: ${FUSE_BINARY}"
        exit 2
    fi

    log_info "Using FUSE binary: ${FUSE_BINARY}"
}

# Parse size string to bytes
parse_size() {
    local size_str="$1"
    local num="${size_str%[KMGT]*}"
    local unit="${size_str##*[0-9]}"

    case "${unit}" in
        K|k) echo $((num * 1024)) ;;
        M|m) echo $((num * 1024 * 1024)) ;;
        G|g) echo $((num * 1024 * 1024 * 1024)) ;;
        T|t) echo $((num * 1024 * 1024 * 1024 * 1024)) ;;
        *) echo "${num}" ;;
    esac
}

# Setup test environment
setup() {
    log_info "Setting up test environment..."

    # Create temporary directory
    TEMP_DIR=$(mktemp -d -t squashfs-bench-XXXXXX)
    log_debug "Created temp directory: ${TEMP_DIR}"

    # Set mount point
    if [ -z "${MOUNT_POINT}" ]; then
        MOUNT_POINT="${TEMP_DIR}/mnt"
        mkdir -p "${MOUNT_POINT}"
    fi

    # Create or use test image
    if [ -z "${TEST_IMAGE}" ]; then
        create_test_image
    else
        if [ ! -f "${TEST_IMAGE}" ]; then
            log_error "Test image not found: ${TEST_IMAGE}"
            exit 1
        fi
        log_info "Using existing test image: ${TEST_IMAGE}"
    fi
}

# Create test SquashFS image
create_test_image() {
    log_info "Creating test image (${TEST_SIZE})..."

    TEMP_IMAGE="${TEMP_DIR}/benchmark.sqfs"
    local content_dir="${TEMP_DIR}/content"
    mkdir -p "${content_dir}"

    # Create test file with random data
    local size_bytes
    size_bytes=$(parse_size "${TEST_SIZE}")

    log_debug "Creating ${size_bytes} byte test file..."
    dd if=/dev/urandom of="${content_dir}/testfile.bin" bs=1M count=$((size_bytes / 1024 / 1024)) 2>/dev/null

    # If not exact MB, append remaining bytes
    local remainder=$((size_bytes % (1024 * 1024)))
    if [ ${remainder} -gt 0 ]; then
        dd if=/dev/urandom of="${content_dir}/testfile.bin" bs=1 count=${remainder} conv=notrunc oflag=append 2>/dev/null
    fi

    # Create some additional files for variety
    dd if=/dev/zero of="${content_dir}/zeros.bin" bs=1M count=10 2>/dev/null
    for i in $(seq 1 10); do
        echo "Small file ${i}" > "${content_dir}/small_${i}.txt"
    done

    # Create SquashFS image
    mksquashfs "${content_dir}" "${TEMP_IMAGE}" -noappend -no-xattrs -comp gzip > /dev/null 2>&1

    if [ ! -f "${TEMP_IMAGE}" ]; then
        log_error "Failed to create test image"
        exit 1
    fi

    local actual_size
    actual_size=$(stat -c%s "${TEMP_IMAGE}" 2>/dev/null || stat -f%z "${TEMP_IMAGE}" 2>/dev/null)
    log_info "Created test image: ${TEMP_IMAGE} (${actual_size} bytes)"

    TEST_IMAGE="${TEMP_IMAGE}"
}

# Cleanup function
cleanup() {
    local exit_code=$?

    log_info "Cleaning up..."

    # Unmount if mounted
    if [ -n "${MOUNT_POINT}" ] && mountpoint -q "${MOUNT_POINT}" 2>/dev/null; then
        log_debug "Unmounting ${MOUNT_POINT}"
        fusermount3 -u "${MOUNT_POINT}" 2>/dev/null || \
            fusermount -u "${MOUNT_POINT}" 2>/dev/null || \
            umount "${MOUNT_POINT}" 2>/dev/null
    fi

    # Remove temporary files
    if [ "${KEEP_TEMP}" = false ] && [ -n "${TEMP_DIR}" ] && [ -d "${TEMP_DIR}" ]; then
        log_debug "Removing temp directory: ${TEMP_DIR}"
        rm -rf "${TEMP_DIR}"
    fi

    exit ${exit_code}
}

# Mount the SquashFS image
mount_image() {
    log_info "Mounting ${TEST_IMAGE} at ${MOUNT_POINT}"

    # Run FUSE in background
    timeout 60 "${FUSE_BINARY}" -f "${TEST_IMAGE}" "${MOUNT_POINT}" &
    local fuse_pid=$!

    # Wait for mount
    local wait_count=0
    while ! mountpoint -q "${MOUNT_POINT}" 2>/dev/null; do
        sleep 0.1
        wait_count=$((wait_count + 1))
        if [ ${wait_count} -gt 50 ]; then
            log_error "Timeout waiting for mount"
            kill ${fuse_pid} 2>/dev/null
            return 1
        fi
    done

    log_debug "Mounted successfully (PID: ${fuse_pid})"
    return 0
}

# Unmount the SquashFS image
unmount_image() {
    if [ -z "${MOUNT_POINT}" ]; then
        return 0
    fi

    log_info "Unmounting ${MOUNT_POINT}"

    if mountpoint -q "${MOUNT_POINT}" 2>/dev/null; then
        fusermount3 -u "${MOUNT_POINT}" 2>/dev/null || \
            fusermount -u "${MOUNT_POINT}" 2>/dev/null || \
            umount "${MOUNT_POINT}" 2>/dev/null
    fi
}

# Add result to results array
add_result() {
    local name="$1"
    local value="$2"
    local unit="$3"
    RESULTS+=("${name}: ${value} ${unit}")
    log_info "${name}: ${value} ${unit}"
}

# Benchmark: Sequential read using dd
benchmark_sequential_dd() {
    log_info "Running sequential read benchmark (dd)..."

    local test_file="${MOUNT_POINT}/testfile.bin"

    if [ ! -f "${test_file}" ]; then
        log_error "Test file not found: ${test_file}"
        return 1
    fi

    # Drop caches
    sync
    echo 3 > /proc/sys/vm/drop_caches 2>/dev/null || true

    # Run dd with various block sizes
    for bs in 4K 64K 1M; do
        log_debug "Testing with block size: ${bs}"

        # Run benchmark 3 times and take average
        local total_mb=0
        for run in 1 2 3; do
            local result
            result=$(dd if="${test_file}" of=/dev/null bs=${bs} 2>&1 | grep -o '[0-9.]* [GM]'B/s | head -1)

            local mb_per_sec
            if echo "${result}" | grep -q 'GB/s'; then
                mb_per_sec=$(echo "${result}" | awk '{printf "%.0f", $1 * 1024}')
            else
                mb_per_sec=$(echo "${result}" | awk '{printf "%.0f", $1}')
            fi

            total_mb=$((total_mb + mb_per_sec))
        done

        local avg_mb=$((total_mb / 3))
        add_result "Sequential Read (${bs})" "${avg_mb}" "MB/s"
    done
}

# Benchmark: Random read using dd (seek test)
benchmark_random_dd() {
    log_info "Running random seek benchmark (dd)..."

    local test_file="${MOUNT_POINT}/testfile.bin"
    local file_size
    file_size=$(stat -c%s "${test_file}" 2>/dev/null || stat -f%z "${test_file}" 2>/dev/null)

    # Drop caches
    sync
    echo 3 > /proc/sys/vm/drop_caches 2>/dev/null || true

    # Time random seeks
    local start_time
    start_time=$(date +%s.%N)

    local num_seeks=1000
    for i in $(seq 1 ${num_seeks}); do
        local offset=$((RANDOM * RANDOM % file_size))
        dd if="${test_file}" of=/dev/null bs=4K count=1 skip=$((offset / 4096)) 2>/dev/null
    done

    local end_time
    end_time=$(date +%s.%N)

    local elapsed
    elapsed=$(echo "${end_time} - ${start_time}" | bc)
    local iops
    iops=$(echo "scale=0; ${num_seeks} / ${elapsed}" | bc)

    add_result "Random Seek (4K)" "${iops}" "IOPS"
}

# Benchmark: Using fio
benchmark_fio() {
    if [ "${HAS_FIO}" = false ]; then
        log_warn "Skipping fio benchmarks (fio not available)"
        return 0
    fi

    log_info "Running fio benchmarks..."

    local test_dir="${MOUNT_POINT}"
    local fio_config="${TEMP_DIR}/fio_config.fio"

    # Create fio configuration
    cat > "${fio_config}" << EOF
[global]
directory=${test_dir}
ioengine=sync
direct=0
time_based
runtime=10
group_reporting

[sequential-read]
filename=testfile.bin
rw=read
bs=1M
size=${TEST_SIZE}

[random-read-4k]
filename=testfile.bin
rw=randread
bs=4K
size=${TEST_SIZE}
iodepth=1

[random-read-64k]
filename=testfile.bin
rw=randread
bs=64K
size=${TEST_SIZE}
iodepth=1

[random-read-1m]
filename=testfile.bin
rw=randread
bs=1M
size=${TEST_SIZE}
iodepth=1
EOF

    # Drop caches
    sync
    echo 3 > /proc/sys/vm/drop_caches 2>/dev/null || true

    # Run fio
    local fio_output
    fio_output=$(fio "${fio_config}" 2>&1)

    if [ $? -ne 0 ]; then
        log_warn "fio benchmark failed"
        return 1
    fi

    # Parse results
    # Sequential read
    local seq_bw
    seq_bw=$(echo "${fio_output}" | grep -A10 "sequential-read" | grep "READ:" | grep -oP 'bw=\K[0-9]+')
    if [ -n "${seq_bw}" ]; then
        # fio reports in KB/s, convert to MB/s
        local seq_mb=$((seq_bw / 1024))
        add_result "FIO Sequential Read" "${seq_mb}" "MB/s"
    fi

    # Random read 4K
    local rand4k_iops
    rand4k_iops=$(echo "${fio_output}" | grep -A10 "random-read-4k" | grep "READ:" | grep -oP 'IOPS=\K[0-9]+')
    if [ -n "${rand4k_iops}" ]; then
        add_result "FIO Random Read 4K" "${rand4k_iops}" "IOPS"
    fi

    # Random read 64K
    local rand64k_iops
    rand64k_iops=$(echo "${fio_output}" | grep -A10 "random-read-64k" | grep "READ:" | grep -oP 'IOPS=\K[0-9]+')
    if [ -n "${rand64k_iops}" ]; then
        add_result "FIO Random Read 64K" "${rand64k_iops}" "IOPS"
    fi

    # Random read 1M
    local rand1m_bw
    rand1m_bw=$(echo "${fio_output}" | grep -A10 "random-read-1m" | grep "READ:" | grep -oP 'bw=\K[0-9]+')
    if [ -n "${rand1m_bw}" ]; then
        local rand1m_mb=$((rand1m_bw / 1024))
        add_result "FIO Random Read 1M" "${rand1m_mb}" "MB/s"
    fi
}

# Benchmark: Metadata operations
benchmark_metadata() {
    log_info "Running metadata benchmark..."

    # Test file stat
    local test_file="${MOUNT_POINT}/testfile.bin"

    local start_time
    start_time=$(date +%s.%N)

    local iterations=10000
    for i in $(seq 1 ${iterations}); do
        stat "${test_file}" > /dev/null 2>&1
    done

    local end_time
    end_time=$(date +%s.%N)

    local elapsed
    elapsed=$(echo "${end_time} - ${start_time}" | bc)
    local ops_per_sec
    ops_per_sec=$(echo "scale=0; ${iterations} / ${elapsed}" | bc)

    add_result "Stat Operations" "${ops_per_sec}" "ops/s"

    # Test directory listing
    local start_time
    start_time=$(date +%s.%N)

    local iterations=1000
    for i in $(seq 1 ${iterations}); do
        ls "${MOUNT_POINT}" > /dev/null 2>&1
    done

    local end_time
    end_time=$(date +%s.%N)

    local elapsed
    elapsed=$(echo "${end_time} - ${start_time}" | bc)
    local ops_per_sec
    ops_per_sec=$(echo "scale=0; ${iterations} / ${elapsed}" | bc)

    add_result "Directory Listing" "${ops_per_sec}" "ops/s"
}

# Print results summary
print_summary() {
    echo ""
    echo "================================"
    echo "     BENCHMARK RESULTS"
    echo "================================"

    for result in "${RESULTS[@]}"; do
        echo "  ${result}"
    done

    echo "================================"

    # Write to output file if specified
    if [ -n "${OUTPUT_FILE}" ]; then
        {
            echo "# SquashFS-FUSE Benchmark Results"
            echo "# Date: $(date -Iseconds)"
            echo "# Image: ${TEST_IMAGE}"
            echo "# Size: ${TEST_SIZE}"
            echo ""
            for result in "${RESULTS[@]}"; do
                echo "${result}"
            done
        } > "${OUTPUT_FILE}"
        log_info "Results written to: ${OUTPUT_FILE}"
    fi
}

# Main function
main() {
    parse_args "$@"

    echo "================================"
    echo "  SquashFS-FUSE Read Benchmark"
    echo "================================"
    echo ""

    check_dependencies
    trap cleanup EXIT
    setup

    # Mount and run benchmarks
    if mount_image; then
        benchmark_sequential_dd
        benchmark_random_dd
        benchmark_fio
        benchmark_metadata
        unmount_image
    fi

    print_summary
}

main "$@"