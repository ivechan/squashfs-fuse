#!/bin/bash
#
# benchmark_lib.sh - Common functions for SquashFS-FUSE performance benchmarking
#
# This library provides functions for:
# - Mounting/unmounting SquashFS images (FUSE and Kernel)
# - Cache management
# - Running fio benchmarks
# - Parsing benchmark results
#

# Ensure script is sourced, not executed
if [[ "${BASH_SOURCE[0]}" == "${0}" ]]; then
    echo "This script should be sourced, not executed directly."
    exit 1
fi

# Colors for output
BENCH_RED='\033[0;31m'
BENCH_GREEN='\033[0;32m'
BENCH_YELLOW='\033[1;33m'
BENCH_BLUE='\033[0;34m'
BENCH_NC='\033[0m'

# Logging functions
bench_log_info() {
    echo -e "${BENCH_GREEN}[INFO]${BENCH_NC} $1"
}

bench_log_warn() {
    echo -e "${BENCH_YELLOW}[WARN]${BENCH_NC} $1"
}

bench_log_error() {
    echo -e "${BENCH_RED}[ERROR]${BENCH_NC} $1"
}

bench_log_debug() {
    if [ "${BENCH_VERBOSE:-false}" = true ]; then
        echo -e "${BENCH_BLUE}[DEBUG]${BENCH_NC} $1"
    fi
}

# Clear system caches
# Requires sudo for drop_caches
bench_clear_cache() {
    sync
    echo 3 | sudo tee /proc/sys/vm/drop_caches > /dev/null 2>&1 || true
}

# Mount SquashFS image using FUSE
# Arguments: $1 = image path, $2 = mount point
# Returns: 0 on success, 1 on failure
bench_mount_fuse() {
    local image="$1"
    local mount_point="$2"
    local fuse_binary="${BENCH_FUSE_BINARY:-./build/squashfs-fuse}"
    local timeout=60

    bench_log_debug "Mounting FUSE: ${image} -> ${mount_point}"

    # Create mount point if needed
    mkdir -p "${mount_point}"

    # Start FUSE in background
    "${fuse_binary}" -f "${image}" "${mount_point}" &
    local fuse_pid=$!

    # Wait for mount
    local wait_count=0
    while ! mountpoint -q "${mount_point}" 2>/dev/null; do
        sleep 0.1
        wait_count=$((wait_count + 1))
        if [ ${wait_count} -gt $((timeout * 10)) ]; then
            bench_log_error "Timeout waiting for FUSE mount"
            kill ${fuse_pid} 2>/dev/null || true
            return 1
        fi
    done

    bench_log_debug "FUSE mounted (PID: ${fuse_pid})"
    return 0
}

# Unmount FUSE mount
# Arguments: $1 = mount point
bench_unmount_fuse() {
    local mount_point="$1"

    if [ -z "${mount_point}" ]; then
        return 0
    fi

    bench_log_debug "Unmounting FUSE: ${mount_point}"

    if mountpoint -q "${mount_point}" 2>/dev/null; then
        fusermount3 -u "${mount_point}" 2>/dev/null || \
            fusermount -u "${mount_point}" 2>/dev/null || \
            umount "${mount_point}" 2>/dev/null || true
    fi
}

# Mount SquashFS image using Kernel
# Arguments: $1 = image path, $2 = mount point
# Returns: 0 on success, 1 on failure
bench_mount_kernel() {
    local image="$1"
    local mount_point="$2"

    bench_log_debug "Mounting Kernel: ${image} -> ${mount_point}"

    # Create mount point if needed
    mkdir -p "${mount_point}"

    # Mount using kernel squashfs module
    mount -t squashfs -o loop "${image}" "${mount_point}" 2>/dev/null

    if ! mountpoint -q "${mount_point}" 2>/dev/null; then
        bench_log_error "Failed to mount kernel squashfs"
        return 1
    fi

    bench_log_debug "Kernel mounted"
    return 0
}

# Unmount Kernel mount
# Arguments: $1 = mount point
bench_unmount_kernel() {
    local mount_point="$1"

    if [ -z "${mount_point}" ]; then
        return 0
    fi

    bench_log_debug "Unmounting Kernel: ${mount_point}"

    if mountpoint -q "${mount_point}" 2>/dev/null; then
        umount "${mount_point}" 2>/dev/null || true
    fi
}

# Generic unmount function
# Arguments: $1 = mount point
bench_unmount() {
    local mount_point="$1"
    local mount_type="${2:-fuse}"

    if [ "${mount_type}" = "kernel" ]; then
        bench_unmount_kernel "${mount_point}"
    else
        bench_unmount_fuse "${mount_point}"
    fi
}

# Run fio benchmark
# Arguments: $1 = fio config file, $2 = test directory
# Returns: fio output on stdout
bench_run_fio() {
    local config_file="$1"
    local test_dir="$2"

    if [ ! -f "${config_file}" ]; then
        bench_log_error "FIO config not found: ${config_file}"
        return 1
    fi

    bench_clear_cache

    # Run fio with the config
    fio --directory="${test_dir}" "${config_file}" 2>&1
}

# Parse fio output for throughput (MB/s)
# Arguments: $1 = fio output, $2 = job name
# Returns: throughput in MB/s
bench_parse_fio_bw() {
    local fio_output="$1"
    local job_name="$2"

    # Extract bandwidth (fio reports in KB/s by default)
    local bw_kbps
    bw_kbps=$(echo "${fio_output}" | \
        grep -A20 "${job_name}" | \
        grep "READ:" | \
        grep -oP 'bw=\K[0-9]+' | \
        head -1)

    if [ -n "${bw_kbps}" ]; then
        # Convert KB/s to MB/s
        echo $((bw_kbps / 1024))
    else
        echo "0"
    fi
}

# Parse fio output for IOPS
# Arguments: $1 = fio output, $2 = job name
# Returns: IOPS value
bench_parse_fio_iops() {
    local fio_output="$1"
    local job_name="$2"

    local iops
    iops=$(echo "${fio_output}" | \
        grep -A20 "${job_name}" | \
        grep "READ:" | \
        grep -oP 'IOPS=\K[0-9]+' | \
        head -1)

    echo "${iops:-0}"
}

# Parse fio output for latency (usec)
# Arguments: $1 = fio output, $2 = job name
# Returns: average latency in microseconds
bench_parse_fio_lat() {
    local fio_output="$1"
    local job_name="$2"

    local lat_usec
    lat_usec=$(echo "${fio_output}" | \
        grep -A20 "${job_name}" | \
        grep "READ:" | \
        grep -oP 'lat \(usec\).*avg=\K[0-9.]+' | \
        head -1)

    # Try msec if usec not found
    if [ -z "${lat_usec}" ]; then
        local lat_msec
        lat_msec=$(echo "${fio_output}" | \
            grep -A20 "${job_name}" | \
            grep "READ:" | \
            grep -oP 'lat \(msec\).*avg=\K[0-9.]+' | \
            head -1)
        if [ -n "${lat_msec}" ]; then
            # Convert msec to usec
            lat_usec=$(echo "${lat_msec} * 1000" | bc)
        fi
    fi

    echo "${lat_usec:-0}"
}

# Run metadata stat benchmark
# Arguments: $1 = test file, $2 = iterations
# Returns: ops/s
bench_benchmark_stat() {
    local test_file="$1"
    local iterations="${2:-10000}"

    bench_clear_cache

    local start end elapsed ops
    start=$(date +%s.%N)

    for i in $(seq 1 ${iterations}); do
        stat "${test_file}" > /dev/null 2>&1
    done

    end=$(date +%s.%N)
    elapsed=$(echo "${end} - ${start}" | bc)
    ops=$(echo "scale=0; ${iterations} / ${elapsed}" | bc)

    echo "${ops}"
}

# Run metadata readdir benchmark
# Arguments: $1 = test directory, $2 = iterations
# Returns: ops/s
bench_benchmark_readdir() {
    local test_dir="$1"
    local iterations="${2:-1000}"

    bench_clear_cache

    local start end elapsed ops
    start=$(date +%s.%N)

    for i in $(seq 1 ${iterations}); do
        ls "${test_dir}" > /dev/null 2>&1
    done

    end=$(date +%s.%N)
    elapsed=$(echo "${end} - ${start}" | bc)
    ops=$(echo "scale=0; ${iterations} / ${elapsed}" | bc)

    echo "${ops}"
}

# Run dd sequential read benchmark
# Arguments: $1 = test file, $2 = block size (e.g., "1M")
# Returns: throughput in MB/s
bench_benchmark_dd_seq() {
    local test_file="$1"
    local block_size="${2:-1M}"

    bench_clear_cache

    local result mb_per_sec
    result=$(dd if="${test_file}" of=/dev/null bs=${block_size} 2>&1 | \
        grep -o '[0-9.]* [GM]B/s' | head -1)

    if echo "${result}" | grep -q 'GB/s'; then
        mb_per_sec=$(echo "${result}" | awk '{printf "%.0f", $1 * 1024}')
    else
        mb_per_sec=$(echo "${result}" | awk '{printf "%.0f", $1}')
    fi

    echo "${mb_per_sec:-0}"
}

# Format percentage ratio
# Arguments: $1 = value1, $2 = value2
# Returns: percentage string (e.g., "35%")
bench_format_ratio() {
    local val1="$1"
    local val2="$2"

    if [ "${val2}" -eq 0 ] 2>/dev/null; then
        echo "N/A"
        return
    fi

    local ratio
    ratio=$(echo "scale=0; ${val1} * 100 / ${val2}" | bc)
    echo "${ratio}%"
}

# Check if required tools are available
bench_check_dependencies() {
    local missing=()

    if ! command -v fio &> /dev/null; then
        missing+=("fio")
    fi

    if ! command -v bc &> /dev/null; then
        missing+=("bc")
    fi

    if ! command -v mksquashfs &> /dev/null; then
        missing+=("mksquashfs")
    fi

    if [ ${#missing[@]} -gt 0 ]; then
        bench_log_error "Missing dependencies: ${missing[*]}"
        bench_log_error "Install with: sudo apt install ${missing[*]}"
        return 1
    fi

    return 0
}