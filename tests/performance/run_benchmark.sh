#!/bin/bash
#
# run_benchmark.sh - Run SquashFS-FUSE performance benchmarks
#
# Compares SquashFS-FUSE performance with Linux kernel SquashFS module.
#
# Usage: ./run_benchmark.sh [options]
#   -h, --help              Show help message
#   -i, --image PATH        Use specific SquashFS image
#   -o, --output FILE       Output results to Markdown file
#   -f, --fuse-binary PATH  Path to squashfs-fuse binary
#   -v, --verbose           Enable verbose output
#   --quick                 Run quick test (5 seconds each)
#
# Output: Markdown report comparing FUSE vs Kernel performance
#

# Configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
FIO_CONFIG_DIR="${SCRIPT_DIR}/fio_configs"

# Default values
VERBOSE=false
QUICK_MODE=false
OUTPUT_FILE=""
FUSE_BINARY=""
TEST_IMAGE=""
RUNTIME=30

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

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
        echo -e "[DEBUG] $1"
    fi
}

# Usage information
usage() {
    cat << EOF
Usage: $(basename "$0") [options]

SquashFS-FUSE Performance Benchmark

Compares FUSE implementation with Linux kernel SquashFS module.

Options:
  -h, --help              Show this help message
  -i, --image PATH        Use specific SquashFS image
  -o, --output FILE       Output results to Markdown file
  -f, --fuse-binary PATH  Path to squashfs-fuse binary
  -v, --verbose           Enable verbose output
  --quick                 Run quick test (5 seconds each)

Examples:
  $(basename "$0")                          # Run with defaults
  $(basename "$0") -o results.md            # Save results to file
  $(basename "$0") -i test.sqfs -o out.md   # Use specific image
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
            -o|--output)
                OUTPUT_FILE="$2"
                shift 2
                ;;
            -f|--fuse-binary)
                FUSE_BINARY="$2"
                shift 2
                ;;
            --quick)
                QUICK_MODE=true
                RUNTIME=5
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

# Check dependencies
check_dependencies() {
    log_info "Checking dependencies..."

    local missing=()

    if ! command -v fio &> /dev/null; then
        missing+=("fio")
    fi

    if ! command -v bc &> /dev/null; then
        missing+=("bc")
    fi

    if ! command -v mksquashfs &> /dev/null; then
        missing+=("mksquashfs (squashfs-tools)")
    fi

    # Check kernel squashfs module
    if ! lsmod | grep -q squashfs; then
        log_warn "squashfs kernel module not loaded. Trying to load..."
        sudo modprobe squashfs 2>/dev/null || true
    fi

    if ! lsmod | grep -q loop; then
        sudo modprobe loop 2>/dev/null || true
    fi

    if [ ${#missing[@]} -gt 0 ]; then
        log_error "Missing dependencies: ${missing[*]}"
        log_error "Install with: sudo apt install ${missing[*]}"
        exit 1
    fi

    # Find FUSE binary
    if [ -z "${FUSE_BINARY}" ]; then
        for path in "${PROJECT_ROOT}/build/squashfs-fuse" \
                    "${PROJECT_ROOT}/squashfs-fuse" \
                    "./build/squashfs-fuse"; do
            if [ -x "${path}" ]; then
                FUSE_BINARY="${path}"
                break
            fi
        done
    fi

    if [ -z "${FUSE_BINARY}" ]; then
        log_error "FUSE binary not found. Build with: mkdir -p build && cd build && cmake .. && make"
        exit 1
    fi

    log_info "Using FUSE binary: ${FUSE_BINARY}"
}

# Setup test environment
setup() {
    log_info "Setting up test environment..."

    # Create temp directory
    TEMP_DIR=$(mktemp -d -t squashfs-bench-XXXXXX)
    trap "cleanup" EXIT

    FUSE_MOUNT="${TEMP_DIR}/fuse"
    KERNEL_MOUNT="${TEMP_DIR}/kernel"
    mkdir -p "${FUSE_MOUNT}" "${KERNEL_MOUNT}"

    # Find or create test image
    if [ -z "${TEST_IMAGE}" ]; then
        # Look for existing test image
        for img in "${PROJECT_ROOT}/tests/fixtures/perf/perf_gzip_bs128k.sqfs" \
                   "${PROJECT_ROOT}/tests/fixtures/basic.sqfs"; do
            if [ -f "${img}" ]; then
                TEST_IMAGE="${img}"
                break
            fi
        done

        if [ -z "${TEST_IMAGE}" ]; then
            log_info "No test image found. Creating one..."
            "${PROJECT_ROOT}/scripts/create_perf_images.sh" "${TEMP_DIR}/perf" 2>/dev/null || true
            TEST_IMAGE="${TEMP_DIR}/perf/perf_gzip_bs128k.sqfs"
        fi
    fi

    if [ ! -f "${TEST_IMAGE}" ]; then
        log_error "Test image not found: ${TEST_IMAGE}"
        exit 1
    fi

    log_info "Using test image: ${TEST_IMAGE}"
}

# Cleanup
cleanup() {
    log_debug "Cleaning up..."

    # Unmount
    if mountpoint -q "${FUSE_MOUNT}" 2>/dev/null; then
        fusermount3 -u "${FUSE_MOUNT}" 2>/dev/null || \
            fusermount -u "${FUSE_MOUNT}" 2>/dev/null || \
            umount "${FUSE_MOUNT}" 2>/dev/null || true
    fi

    if mountpoint -q "${KERNEL_MOUNT}" 2>/dev/null; then
        umount "${KERNEL_MOUNT}" 2>/dev/null || true
    fi

    # Remove temp directory
    if [ -d "${TEMP_DIR}" ]; then
        rm -rf "${TEMP_DIR}"
    fi
}

# Clear caches
clear_cache() {
    sync
    echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null 2>&1 || true
}

# Mount FUSE
mount_fuse() {
    log_debug "Mounting FUSE..."

    "${FUSE_BINARY}" -f "${TEST_IMAGE}" "${FUSE_MOUNT}" &
    local fuse_pid=$!

    # Wait for mount
    local wait_count=0
    while ! mountpoint -q "${FUSE_MOUNT}" 2>/dev/null; do
        sleep 0.1
        wait_count=$((wait_count + 1))
        if [ ${wait_count} -gt 100 ]; then
            log_error "Timeout waiting for FUSE mount"
            kill ${fuse_pid} 2>/dev/null || true
            return 1
        fi
    done

    return 0
}

# Unmount FUSE
unmount_fuse() {
    if mountpoint -q "${FUSE_MOUNT}" 2>/dev/null; then
        fusermount3 -u "${FUSE_MOUNT}" 2>/dev/null || \
            fusermount -u "${FUSE_MOUNT}" 2>/dev/null || \
            umount "${FUSE_MOUNT}" 2>/dev/null || true
    fi
}

# Mount Kernel
mount_kernel() {
    log_debug "Mounting Kernel..."

    mount -t squashfs -o loop "${TEST_IMAGE}" "${KERNEL_MOUNT}" 2>/dev/null

    if ! mountpoint -q "${KERNEL_MOUNT}" 2>/dev/null; then
        log_error "Failed to mount kernel squashfs"
        return 1
    fi

    return 0
}

# Unmount Kernel
unmount_kernel() {
    if mountpoint -q "${KERNEL_MOUNT}" 2>/dev/null; then
        umount "${KERNEL_MOUNT}" 2>/dev/null || true
    fi
}

# Store results
declare -a RESULTS_SEQ_FUSE RESULTS_SEQ_KERNEL
declare -a RESULTS_RAND_FUSE RESULTS_RAND_KERNEL
declare -a RESULTS_STAT_FUSE RESULTS_STAT_KERNEL
declare -a RESULTS_READDIR_FUSE RESULTS_READDIR_KERNEL
declare -a RESULTS_CONC_FUSE RESULTS_CONC_KERNEL

# Run sequential read benchmark
run_sequential_benchmark() {
    local mount_point="$1"
    local is_fuse="$2"

    log_debug "Running sequential read benchmark..."

    local test_file="${mount_point}/medium_10m.bin"
    if [ ! -f "${test_file}" ]; then
        test_file="${mount_point}/zeros_10m.bin"
    fi

    if [ ! -f "${test_file}" ]; then
        log_warn "No suitable test file found for sequential read"
        return
    fi

    log_debug "Using test file: ${test_file}"
    clear_cache

    # Run fio sequential read (without direct=1 for FUSE compatibility)
    local fio_output
    fio_output=$(fio --directory="${mount_point}" \
        --name=seq-read \
        --filename="$(basename ${test_file})" \
        --rw=read \
        --bs=1M \
        --ioengine=sync \
        --time_based \
        --runtime=${RUNTIME} \
        --group_reporting 2>&1) || true

    local bw
    bw=$(echo "${fio_output}" | grep "READ:" | grep -oP 'bw=\K[0-9]+' | head -1)

    if [ -n "${bw}" ]; then
        # Convert KB/s to MB/s
        local mb_ps=$((bw / 1024))
        if [ "${is_fuse}" = "true" ]; then
            RESULTS_SEQ_FUSE=("${mb_ps}")
        else
            RESULTS_SEQ_KERNEL=("${mb_ps}")
        fi
        log_info "Sequential Read: ${mb_ps} MB/s"
    fi
}

# Run random read benchmark
run_random_benchmark() {
    local mount_point="$1"
    local is_fuse="$2"

    log_debug "Running random read benchmark..."

    local test_file="${mount_point}/medium_10m.bin"
    if [ ! -f "${test_file}" ]; then
        test_file="${mount_point}/zeros_10m.bin"
    fi

    if [ ! -f "${test_file}" ]; then
        log_warn "No suitable test file found for random read"
        return
    fi

    log_debug "Using test file: ${test_file}"
    clear_cache

    # Run fio random read (without direct=1 for FUSE compatibility)
    local fio_output
    fio_output=$(fio --directory="${mount_point}" \
        --name=rand-read-4k \
        --filename="$(basename ${test_file})" \
        --rw=randread \
        --bs=4K \
        --ioengine=sync \
        --time_based \
        --runtime=${RUNTIME} \
        --iodepth=1 \
        --group_reporting 2>&1) || true

    # Parse IOPS - handle k suffix (e.g., 12.6k)
    local iops_raw
    iops_raw=$(echo "${fio_output}" | grep "IOPS=" | grep -oP 'IOPS=\K[0-9.]+k?' | head -1)

    local iops
    if [[ "${iops_raw}" == *"k" ]]; then
        # Convert k suffix to number (e.g., 12.6k -> 12600)
        local num="${iops_raw%k}"
        iops=$(echo "${num} * 1000" | bc | cut -d. -f1)
    else
        iops="${iops_raw}"
    fi

    if [ -n "${iops}" ] && [ "${iops}" != "0" ]; then
        if [ "${is_fuse}" = "true" ]; then
            RESULTS_RAND_FUSE=("${iops}")
        else
            RESULTS_RAND_KERNEL=("${iops}")
        fi
        log_info "Random Read 4K: ${iops} IOPS"
    fi
}

# Run metadata benchmark
run_metadata_benchmark() {
    local mount_point="$1"
    local is_fuse="$2"

    log_debug "Running metadata benchmark..."

    # Stat benchmark
    local test_file="${mount_point}/medium_1m.bin"
    if [ ! -f "${test_file}" ]; then
        test_file=$(find "${mount_point}" -type f | head -1)
    fi

    if [ -f "${test_file}" ]; then
        clear_cache

        local iterations=10000
        local start end elapsed ops

        start=$(date +%s.%N)
        for i in $(seq 1 ${iterations}); do
            stat "${test_file}" > /dev/null 2>&1
        done
        end=$(date +%s.%N)

        elapsed=$(echo "${end} - ${start}" | bc)
        ops=$(echo "scale=0; ${iterations} / ${elapsed}" | bc)

        if [ "${is_fuse}" = "true" ]; then
            RESULTS_STAT_FUSE=("${ops}")
        else
            RESULTS_STAT_KERNEL=("${ops}")
        fi
        log_info "Stat: ${ops} ops/s"
    fi

    # Readdir benchmark
    if [ -d "${mount_point}/many_files" ]; then
        clear_cache

        local iterations=1000
        local start end elapsed ops

        start=$(date +%s.%N)
        for i in $(seq 1 ${iterations}); do
            ls "${mount_point}/many_files" > /dev/null 2>&1
        done
        end=$(date +%s.%N)

        elapsed=$(echo "${end} - ${start}" | bc)
        ops=$(echo "scale=0; ${iterations} / ${elapsed}" | bc)

        if [ "${is_fuse}" = "true" ]; then
            RESULTS_READDIR_FUSE=("${ops}")
        else
            RESULTS_READDIR_KERNEL=("${ops}")
        fi
        log_info "Readdir: ${ops} ops/s"
    fi
}

# Run concurrent benchmark
run_concurrent_benchmark() {
    local mount_point="$1"
    local is_fuse="$2"
    local threads="$3"

    log_debug "Running concurrent read benchmark (${threads} threads)..."

    local test_file="${mount_point}/medium_10m.bin"
    if [ ! -f "${test_file}" ]; then
        test_file="${mount_point}/zeros_10m.bin"
    fi

    if [ ! -f "${test_file}" ]; then
        return
    fi

    clear_cache

    local fio_output
    fio_output=$(fio --directory="${mount_point}" \
        --name=randread \
        --filename="$(basename ${test_file})" \
        --rw=randread \
        --bs=4K \
        --ioengine=sync \
        --time_based \
        --runtime=${RUNTIME} \
        --numjobs=${threads} \
        --iodepth=1 \
        --group_reporting 2>&1) || true

    # Parse IOPS - handle k suffix
    local iops_raw
    iops_raw=$(echo "${fio_output}" | grep "IOPS=" | grep -oP 'IOPS=\K[0-9.]+k?' | head -1)

    local iops
    if [[ "${iops_raw}" == *"k" ]]; then
        local num="${iops_raw%k}"
        iops=$(echo "${num} * 1000" | bc | cut -d. -f1)
    else
        iops="${iops_raw}"
    fi

    if [ -n "${iops}" ] && [ "${iops}" != "0" ]; then
        if [ "${is_fuse}" = "true" ]; then
            RESULTS_CONC_FUSE[${threads}]=${iops}
        else
            RESULTS_CONC_KERNEL[${threads}]=${iops}
        fi
        log_info "Concurrent ${threads} threads: ${iops} IOPS"
    fi
}

# Calculate ratio
calc_ratio() {
    local fuse_val="$1"
    local kernel_val="$2"

    if [ -z "${kernel_val}" ] || [ "${kernel_val}" = "N/A" ]; then
        echo "N/A"
        return
    fi

    if [ -z "${fuse_val}" ] || [ "${fuse_val}" = "N/A" ]; then
        echo "N/A"
        return
    fi

    if [ "${kernel_val}" -eq 0 ] 2>/dev/null; then
        echo "N/A"
        return
    fi

    local ratio
    ratio=$(echo "scale=0; ${fuse_val} * 100 / ${kernel_val}" | bc 2>/dev/null)
    echo "${ratio}%"
}

# Generate Markdown report
generate_report() {
    local report=""

    report+="# SquashFS-FUSE 性能测试报告\n\n"
    report+="## 测试环境\n\n"
    report+="- 内核版本: $(uname -r)\n"
    report+="- 测试镜像: $(basename ${TEST_IMAGE})\n"
    report+="- 测试时长: ${RUNTIME}s\n"
    report+="- 日期: $(date -Iseconds)\n\n"

    # Sequential read
    report+="## 连续读性能\n\n"
    report+="| 实现 | 吞吐量 (MB/s) |\n"
    report+="|------|---------------|\n"

    local seq_fuse="${RESULTS_SEQ_FUSE[0]:-N/A}"
    local seq_kernel="${RESULTS_SEQ_KERNEL[0]:-N/A}"

    report+="| FUSE | ${seq_fuse} |\n"
    report+="| Kernel | ${seq_kernel} |\n"
    report+="| 性能比 | $(calc_ratio ${seq_fuse} ${seq_kernel}) |\n\n"

    # Random read
    report+="## 随机读性能 (4K)\n\n"
    report+="| 实现 | IOPS |\n"
    report+="|------|------|\n"

    local rand_fuse="${RESULTS_RAND_FUSE[0]:-N/A}"
    local rand_kernel="${RESULTS_RAND_KERNEL[0]:-N/A}"

    report+="| FUSE | ${rand_fuse} |\n"
    report+="| Kernel | ${rand_kernel} |\n"
    report+="| 性能比 | $(calc_ratio ${rand_fuse} ${rand_kernel}) |\n\n"

    # Metadata
    report+="## 元数据性能\n\n"
    report+="| 操作 | FUSE (ops/s) | Kernel (ops/s) | 性能比 |\n"
    report+="|------|--------------|----------------|--------|\n"

    local stat_fuse="${RESULTS_STAT_FUSE[0]:-N/A}"
    local stat_kernel="${RESULTS_STAT_KERNEL[0]:-N/A}"
    report+="| stat | ${stat_fuse} | ${stat_kernel} | $(calc_ratio ${stat_fuse} ${stat_kernel}) |\n"

    local readdir_fuse="${RESULTS_READDIR_FUSE[0]:-N/A}"
    local readdir_kernel="${RESULTS_READDIR_KERNEL[0]:-N/A}"
    report+="| readdir | ${readdir_fuse} | ${readdir_kernel} | $(calc_ratio ${readdir_fuse} ${readdir_kernel}) |\n\n"

    # Concurrent
    report+="## 并发性能\n\n"
    report+="| 线程数 | FUSE IOPS | Kernel IOPS | 性能比 |\n"
    report+="|--------|-----------|-------------|--------|\n"

    for threads in 1 2 4 8; do
        local cf="${RESULTS_CONC_FUSE[$threads]:-N/A}"
        local ck="${RESULTS_CONC_KERNEL[$threads]:-N/A}"
        report+="| ${threads} | ${cf} | ${ck} | $(calc_ratio ${cf} ${ck}) |\n"
    done

    report+="\n"

    # Output
    if [ -n "${OUTPUT_FILE}" ]; then
        echo -e "${report}" > "${OUTPUT_FILE}"
        log_info "Report saved to: ${OUTPUT_FILE}"
    fi

    echo ""
    echo -e "${report}"
}

# Main function
main() {
    parse_args "$@"

    echo "================================"
    echo "  SquashFS-FUSE Performance Benchmark"
    echo "================================"
    echo ""

    check_dependencies
    setup

    log_info "Running benchmarks..."

    # Test FUSE
    log_info ""
    log_info "=== Testing FUSE Implementation ==="
    if mount_fuse; then
        run_sequential_benchmark "${FUSE_MOUNT}" "true"
        run_random_benchmark "${FUSE_MOUNT}" "true"
        run_metadata_benchmark "${FUSE_MOUNT}" "true"

        for threads in 1 2 4 8; do
            run_concurrent_benchmark "${FUSE_MOUNT}" "true" ${threads}
        done

        unmount_fuse
    else
        log_error "Failed to mount FUSE"
    fi

    # Test Kernel
    log_info ""
    log_info "=== Testing Kernel Implementation ==="
    if mount_kernel; then
        run_sequential_benchmark "${KERNEL_MOUNT}" "false"
        run_random_benchmark "${KERNEL_MOUNT}" "false"
        run_metadata_benchmark "${KERNEL_MOUNT}" "false"

        for threads in 1 2 4 8; do
            run_concurrent_benchmark "${KERNEL_MOUNT}" "false" ${threads}
        done

        unmount_kernel
    else
        log_warn "Failed to mount Kernel squashfs (may need different permissions)"
    fi

    # Generate report
    log_info ""
    generate_report
}

main "$@"