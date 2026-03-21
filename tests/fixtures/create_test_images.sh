#!/bin/bash
#
# create_test_images.sh - Generate SquashFS test images
#
# This script creates various SquashFS images for testing purposes.
# It requires mksquashfs to be installed.
#
# Usage: ./create_test_images.sh [output_dir]
#

set -e

# Configuration
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUTPUT_DIR="${1:-${SCRIPT_DIR}}"

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

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

# Check for mksquashfs
check_mksquashfs() {
    if ! command -v mksquashfs &> /dev/null; then
        log_error "mksquashfs not found. Please install squashfs-tools."
        exit 1
    fi
    log_info "Found mksquashfs: $(mksquashfs -version 2>&1 | head -n1)"
}

# Create test directory structure
create_test_content() {
    local content_dir="$1"
    local test_name="$2"

    rm -rf "${content_dir}"
    mkdir -p "${content_dir}"

    case "${test_name}" in
        "basic")
            # Basic directory structure with files
            mkdir -p "${content_dir}/subdir1"
            mkdir -p "${content_dir}/subdir2/nested"

            # Create simple text files
            echo "Hello, World!" > "${content_dir}/hello.txt"
            echo "This is a test file" > "${content_dir}/subdir1/test.txt"
            echo "Nested content" > "${content_dir}/subdir2/nested/deep.txt"

            # Create a file with multiple lines
            for i in $(seq 1 100); do
                echo "Line ${i}: $(printf '%0*d' 50 ${i})" >> "${content_dir}/multiline.txt"
            done

            # Create empty directory
            mkdir -p "${content_dir}/empty_dir"
            ;;

        "large_files")
            # Test with larger files
            mkdir -p "${content_dir}/data"

            # 1MB file
            dd if=/dev/urandom of="${content_dir}/data/random_1mb.bin" bs=1M count=1 2>/dev/null

            # 10MB file (for testing block handling)
            dd if=/dev/zero of="${content_dir}/data/zeros_10mb.bin" bs=1M count=10 2>/dev/null

            # File with known pattern for integrity testing
            dd if=/dev/urandom of="${content_dir}/data/pattern.bin" bs=4096 count=256 2>/dev/null
            ;;

        "symlinks")
            # Test with symbolic links
            mkdir -p "${content_dir}/real"
            echo "Real file content" > "${content_dir}/real/file.txt"
            echo "Another file" > "${content_dir}/real/another.txt"

            # Create symlinks
            ln -s file.txt "${content_dir}/real/link_to_file.txt"
            ln -s ../real "${content_dir}/link_to_dir"
            ln -s /nonexistent "${content_dir}/broken_link"
            ;;

        "permissions")
            # Test with various permissions
            mkdir -p "${content_dir}/perms"
            echo "Read only" > "${content_dir}/perms/readonly.txt"
            chmod 444 "${content_dir}/perms/readonly.txt"

            echo "Executable script" > "${content_dir}/perms/script.sh"
            chmod 755 "${content_dir}/perms/script.sh"

            echo "Private file" > "${content_dir}/perms/private.txt"
            chmod 600 "${content_dir}/perms/private.txt"
            ;;

        "deep_nested")
            # Deep directory nesting
            local current="${content_dir}"
            for i in $(seq 1 20); do
                current="${current}/level${i}"
                mkdir -p "${current}"
                echo "File at level ${i}" > "${current}/file.txt"
            done
            ;;

        "many_files")
            # Many files in a directory
            mkdir -p "${content_dir}/many"
            for i in $(seq 1 1000); do
                echo "File ${i}" > "${content_dir}/many/file_${i}.txt"
            done
            ;;

        "special_names")
            # Special file names (spaces, unicode, etc.)
            mkdir -p "${content_dir}/special"
            echo "Space in name" > "${content_dir}/special/file with spaces.txt"
            echo "Tab\tin\tname" > "${content_dir}/special/file	with	tabs.txt"
            echo "Unicode content" > "${content_dir}/special/unicode_中文.txt"
            echo "Emoji content" > "${content_dir}/special/emoji_😀.txt"
            ;;

        "empty_files")
            # Empty files
            mkdir -p "${content_dir}/empty"
            touch "${content_dir}/empty/empty1.txt"
            touch "${content_dir}/empty/empty2.txt"
            touch "${content_dir}/empty/empty3.txt"
            ;;

        "fragments")
            # Small files to test fragment handling
            mkdir -p "${content_dir}/small"
            for i in $(seq 1 100); do
                echo "Small file ${i}" > "${content_dir}/small/tiny_${i}.txt"
            done
            ;;

        *)
            log_error "Unknown test content type: ${test_name}"
            return 1
            ;;
    esac

    log_info "Created test content for '${test_name}'"
}

# Create SquashFS image
create_image() {
    local content_dir="$1"
    local output_image="$2"
    local compressor="$3"
    local block_size="$4"
    local extra_opts="$5"

    local opts=(-noappend -no-xattrs)

    # Set compressor
    case "${compressor}" in
        "gzip")
            opts+=(-comp gzip)
            ;;
        "zstd")
            opts+=(-comp zstd)
            ;;
        "xz")
            opts+=(-comp xz)
            ;;
        "lzo")
            opts+=(-comp lzo)
            ;;
        "lz4")
            opts+=(-comp lz4)
            ;;
        "none")
            opts+=(-comp none)
            ;;
        *)
            opts+=(-comp gzip)  # Default
            ;;
    esac

    # Set block size
    if [ -n "${block_size}" ]; then
        opts+=(-b "${block_size}")
    fi

    # Add extra options
    if [ -n "${extra_opts}" ]; then
        opts+=(${extra_opts})
    fi

    mksquashfs "${content_dir}" "${output_image}" "${opts[@]}" > /dev/null 2>&1

    if [ $? -eq 0 ]; then
        local size
        size=$(stat -c%s "${output_image}" 2>/dev/null || stat -f%z "${output_image}" 2>/dev/null)
        log_info "Created image: ${output_image} (${size} bytes, ${compressor})"
    else
        log_error "Failed to create image: ${output_image}"
        return 1
    fi
}

# Main function
main() {
    check_mksquashfs

    log_info "Output directory: ${OUTPUT_DIR}"
    mkdir -p "${OUTPUT_DIR}"

    local temp_dir
    temp_dir=$(mktemp -d)
    trap "rm -rf ${temp_dir}" EXIT

    local content_dir="${temp_dir}/content"
    local image_dir="${OUTPUT_DIR}/images"
    mkdir -p "${image_dir}"

    # Test configurations: content_type, compressor, block_size, suffix
    local tests=(
        # Basic tests with different compressors
        "basic:gzip:131072:"
        "basic:zstd:131072:"
        "basic:none:131072:"

        # Large files tests
        "large_files:gzip:262144:"
        "large_files:zstd:262144:"

        # Symlinks tests
        "symlinks:gzip:131072:"

        # Permissions tests
        "permissions:gzip:131072:"

        # Deep nesting tests
        "deep_nested:gzip:131072:"

        # Many files tests
        "many_files:gzip:131072:"

        # Special names tests
        "special_names:gzip:131072:"

        # Empty files tests
        "empty_files:gzip:131072:"

        # Fragment tests
        "fragments:gzip:131072:"

        # Block size variations
        "basic:gzip:4096:block4k"
        "basic:gzip:65536:block64k"
        "basic:gzip:1048576:block1m"

        # No fragments
        "basic:gzip:131072:-no-fragments_nofrag"
    )

    for test_config in "${tests[@]}"; do
        IFS=':' read -r content_type compressor block_size suffix <<< "${test_config}"

        create_test_content "${content_dir}" "${content_type}"

        local image_name="${content_type}"
        [ -n "${suffix}" ] && image_name="${image_name}_${suffix}"

        local extra_opts=""
        if [[ "${suffix}" == *"nofrag"* ]]; then
            extra_opts="-no-fragments"
        fi

        create_image "${content_dir}" "${image_dir}/${image_name}.sqfs" "${compressor}" "${block_size}" "${extra_opts}"

        rm -rf "${content_dir}"
    done

    # Create combined test image with all content types
    log_info "Creating comprehensive test image..."
    mkdir -p "${content_dir}"
    for content_type in basic large_files symlinks permissions deep_nested special_names empty_files fragments; do
        create_test_content "${content_dir}/${content_type}" "${content_type}"
    done
    create_image "${content_dir}" "${image_dir}/comprehensive.sqfs" "gzip" "131072" ""

    # Create manifest file
    local manifest="${OUTPUT_DIR}/manifest.txt"
    log_info "Creating manifest file..."
    {
        echo "# SquashFS Test Images Manifest"
        echo "# Generated: $(date -Iseconds)"
        echo ""
        for image in "${image_dir}"/*.sqfs; do
            local name
            name=$(basename "${image}")
            local size
            size=$(stat -c%s "${image}" 2>/dev/null || stat -f%z "${image}" 2>/dev/null)
            echo "${name}: ${size} bytes"
        done
    } > "${manifest}"

    log_info "Test image creation complete!"
    log_info "Images location: ${image_dir}"
    log_info "Manifest: ${manifest}"

    # List created images
    echo ""
    echo "Created images:"
    ls -la "${image_dir}"/*.sqfs
}

main "$@"