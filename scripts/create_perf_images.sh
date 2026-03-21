#!/bin/bash
#
# create_perf_images.sh - Generate SquashFS performance test images
#
# This script creates SquashFS images for performance benchmarking.
# Uses fixed seed for reproducible test data.
#
# Usage: ./create_perf_images.sh [output_dir]
#

set -e

# Configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
OUTPUT_DIR="${1:-${PROJECT_ROOT}/tests/fixtures/perf}"
SEED=42

# Colors for output
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

# Check for required tools
check_dependencies() {
    log_info "Checking dependencies..."

    if ! command -v mksquashfs &> /dev/null; then
        log_error "mksquashfs not found. Please install squashfs-tools."
        exit 1
    fi

    if ! command -v openssl &> /dev/null; then
        log_error "openssl not found. Required for reproducible random data."
        exit 1
    fi

    log_info "Found mksquashfs: $(mksquashfs -version 2>&1 | head -n1)"
}

# Create test content
create_test_content() {
    local content_dir="$1"

    log_info "Creating test content in ${content_dir}..."
    rm -rf "${content_dir}"
    mkdir -p "${content_dir}"

    # Small files (fragment test)
    log_info "Creating small files for fragment testing..."
    for i in $(seq 1 100); do
        echo "Small file $i content with some additional data to vary size" > "${content_dir}/small_${i}.txt"
    done

    # Medium files (1MB, 10MB)
    log_info "Creating medium-sized test files..."

    # 1MB file with reproducible pseudo-random data
    dd if=/dev/urandom bs=1M count=1 2>/dev/null | \
        openssl enc -aes-256-ctr -pass pass:${SEED} -nosalt 2>/dev/null \
        > "${content_dir}/medium_1m.bin"

    # 10MB file
    dd if=/dev/urandom bs=1M count=10 2>/dev/null | \
        openssl enc -aes-256-ctr -pass pass:${SEED} -nosalt 2>/dev/null \
        > "${content_dir}/medium_10m.bin"

    # 100MB file (large file test)
    log_info "Creating large test file (100MB)..."
    dd if=/dev/urandom bs=1M count=100 2>/dev/null | \
        openssl enc -aes-256-ctr -pass pass:${SEED} -nosalt 2>/dev/null \
        > "${content_dir}/large_100m.bin"

    # Deep directory structure for metadata testing
    log_info "Creating deep directory structure..."
    local current="${content_dir}/deep"
    mkdir -p "${current}"
    for i in $(seq 1 20); do
        current="${current}/level${i}"
        mkdir -p "${current}"
        echo "File at level ${i}" > "${current}/file.txt"
    done

    # Many files directory for readdir testing
    log_info "Creating directory with many files..."
    mkdir -p "${content_dir}/many_files"
    for i in $(seq 1 1000); do
        echo "File ${i}" > "${content_dir}/many_files/file_${i}.txt"
    done

    # Zero-compressible file (tests compression efficiency)
    log_info "Creating zero-compressible file..."
    dd if=/dev/zero bs=1M count=10 2>/dev/null > "${content_dir}/zeros_10m.bin"

    log_info "Test content created successfully"
}

# Create SquashFS image with specified parameters
create_image() {
    local content_dir="$1"
    local output_image="$2"
    local compressor="$3"
    local block_size="$4"

    log_info "Creating image: $(basename ${output_image}) (${compressor}, ${block_size} block size)"

    mksquashfs "${content_dir}" "${output_image}" \
        -comp "${compressor}" \
        -b "${block_size}" \
        -noappend \
        -no-xattrs \
        > /dev/null 2>&1

    if [ -f "${output_image}" ]; then
        local size
        size=$(stat -c%s "${output_image}" 2>/dev/null || stat -f%z "${output_image}" 2>/dev/null)
        log_info "Created: ${output_image} (${size} bytes)"
    else
        log_error "Failed to create image: ${output_image}"
        return 1
    fi
}

# Main function
main() {
    check_dependencies

    log_info "Output directory: ${OUTPUT_DIR}"
    mkdir -p "${OUTPUT_DIR}"

    # Create temporary content directory
    local temp_dir
    temp_dir=$(mktemp -d)
    trap "rm -rf ${temp_dir}" EXIT

    local content_dir="${temp_dir}/content"

    # Create test content
    create_test_content "${content_dir}"

    # Create images with different configurations
    log_info ""
    log_info "Creating SquashFS images..."

    local compressors=("gzip" "zstd")
    local block_sizes=("4096" "65536" "131072" "1048576")

    for comp in "${compressors[@]}"; do
        for bs in "${block_sizes[@]}"; do
            local bs_human
            case "${bs}" in
                "4096") bs_human="4k" ;;
                "65536") bs_human="64k" ;;
                "131072") bs_human="128k" ;;
                "1048576") bs_human="1m" ;;
            esac

            create_image "${content_dir}" \
                "${OUTPUT_DIR}/perf_${comp}_bs${bs_human}.sqfs" \
                "${comp}" "${bs}"
        done
    done

    # Create manifest
    log_info ""
    log_info "Creating manifest..."
    {
        echo "# SquashFS Performance Test Images"
        echo "# Generated: $(date -Iseconds)"
        echo "# Seed: ${SEED}"
        echo ""
        echo "## Files"
        for image in "${OUTPUT_DIR}"/*.sqfs; do
            if [ -f "${image}" ]; then
                local name size
                name=$(basename "${image}")
                size=$(stat -c%s "${image}" 2>/dev/null || stat -f%z "${image}" 2>/dev/null)
                echo "${name}: ${size} bytes"
            fi
        done
    } > "${OUTPUT_DIR}/manifest.txt"

    log_info ""
    log_info "Performance test images created successfully!"
    log_info "Images location: ${OUTPUT_DIR}"
    log_info "Manifest: ${OUTPUT_DIR}/manifest.txt"
}

main "$@"