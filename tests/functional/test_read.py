#!/usr/bin/env python3
"""
test_read.py - Python read tests for SquashFS-FUSE

This module tests file reading functionality:
- File read correctness verification
- Large file read tests
- Random access read tests

Usage: python test_read.py [options]
  -h, --help              Show help message
  -m, --mount PATH        Mount point path
  -v, --verbose           Enable verbose output
"""

import argparse
import hashlib
import os
import random
import sys
import tempfile
import subprocess
import time
from pathlib import Path
from typing import Optional, Tuple

# Test configuration
BLOCK_SIZE = 4096
CHUNK_SIZES = [1, 64, 256, 512, 1024, 4096, 8192, 65536, 131072]
LARGE_FILE_SIZE = 10 * 1024 * 1024  # 10 MB


class TestResult:
    """Container for test results."""
    def __init__(self):
        self.passed = 0
        self.failed = 0
        self.skipped = 0
        self.errors = []

    def add_pass(self):
        self.passed += 1

    def add_fail(self, test_name: str, reason: str):
        self.failed += 1
        self.errors.append(f"{test_name}: {reason}")

    def add_skip(self, test_name: str, reason: str):
        self.skipped += 1
        self.errors.append(f"{test_name}: SKIPPED - {reason}")

    def summary(self) -> str:
        lines = [
            "",
            "=" * 50,
            "TEST SUMMARY",
            "=" * 50,
            f"Passed:  {self.passed}",
            f"Failed:  {self.failed}",
            f"Skipped: {self.skipped}",
            "=" * 50,
        ]
        if self.errors:
            lines.append("Errors:")
            for err in self.errors:
                lines.append(f"  - {err}")
        return "\n".join(lines)

    def exit_code(self) -> int:
        return 0 if self.failed == 0 else 1


class Colors:
    """ANSI color codes."""
    GREEN = '\033[0;32m'
    RED = '\033[0;31m'
    YELLOW = '\033[1;33m'
    BLUE = '\033[0;34m'
    NC = '\033[0m'


def log_info(msg: str):
    print(f"{Colors.GREEN}[INFO]{Colors.NC} {msg}")


def log_error(msg: str):
    print(f"{Colors.RED}[ERROR]{Colors.NC} {msg}")


def log_debug(msg: str, verbose: bool = False):
    if verbose:
        print(f"{Colors.BLUE}[DEBUG]{Colors.NC} {msg}")


def create_test_content(content_dir: Path) -> dict:
    """
    Create test content and return a dictionary of expected files and their hashes.

    Returns:
        dict mapping file paths to (size, md5_hash) tuples
    """
    expected = {}

    # Create directories
    (content_dir / "data").mkdir(parents=True, exist_ok=True)
    (content_dir / "small").mkdir(parents=True, exist_ok=True)

    # Small text file
    small_text = content_dir / "small.txt"
    small_text.write_text("Hello, SquashFS!")
    expected["small.txt"] = (small_text.stat().st_size, hash_file(small_text))

    # Multiline text file
    multiline = content_dir / "multiline.txt"
    lines = [f"Line {i}: {'x' * 50}" for i in range(1000)]
    multiline.write_text("\n".join(lines))
    expected["multiline.txt"] = (multiline.stat().st_size, hash_file(multiline))

    # Large binary file
    large_file = content_dir / "data" / "large.bin"
    with open(large_file, 'wb') as f:
        # Create deterministic content using seed
        random.seed(42)
        for _ in range(LARGE_FILE_SIZE // BLOCK_SIZE):
            f.write(random.randbytes(BLOCK_SIZE))
    expected["data/large.bin"] = (large_file.stat().st_size, hash_file(large_file))

    # Medium files for different block sizes
    for size in [1024, 4096, 65536, 1048576]:
        medium_file = content_dir / "data" / f"medium_{size}.bin"
        random.seed(size)
        with open(medium_file, 'wb') as f:
            f.write(random.randbytes(size))
        expected[f"data/medium_{size}.bin"] = (medium_file.stat().st_size, hash_file(medium_file))

    # Many small files for fragment testing
    for i in range(100):
        small_file = content_dir / "small" / f"file_{i:04d}.txt"
        small_file.write_text(f"Small file number {i}")
        expected[f"small/file_{i:04d}.txt"] = (small_file.stat().st_size, hash_file(small_file))

    # Empty file
    empty_file = content_dir / "empty.txt"
    empty_file.touch()
    expected["empty.txt"] = (0, "")

    # File at boundary
    boundary_file = content_dir / "data" / "boundary.bin"
    with open(boundary_file, 'wb') as f:
        # Exactly 4KB (common block size)
        f.write(b'B' * 4096)
    expected["data/boundary.bin"] = (boundary_file.stat().st_size, hash_file(boundary_file))

    # File crossing boundary
    cross_file = content_dir / "data" / "cross_boundary.bin"
    with open(cross_file, 'wb') as f:
        # 4096 + 1 byte to cross boundary
        f.write(b'C' * 4097)
    expected["data/cross_boundary.bin"] = (cross_file.stat().st_size, hash_file(cross_file))

    return expected


def hash_file(filepath: Path) -> str:
    """Calculate MD5 hash of a file."""
    if filepath.stat().st_size == 0:
        return ""

    md5 = hashlib.md5()
    with open(filepath, 'rb') as f:
        while chunk := f.read(8192):
            md5.update(chunk)
    return md5.hexdigest()


def create_squashfs_image(content_dir: Path, image_path: Path) -> bool:
    """Create a SquashFS image from content directory."""
    try:
        result = subprocess.run(
            ['mksquashfs', str(content_dir), str(image_path),
             '-noappend', '-no-xattrs', '-comp', 'gzip'],
            capture_output=True,
            timeout=60
        )
        if result.returncode == 0:
            log_info(f"Created image: {image_path} ({image_path.stat().st_size} bytes)")
            return True
        else:
            log_error(f"mksquashfs failed: {result.stderr.decode()}")
            return False
    except Exception as e:
        log_error(f"Failed to create image: {e}")
        return False


def mount_image(fuse_binary: Path, image_path: Path, mount_point: Path, timeout: int = 30) -> bool:
    """Mount SquashFS image using FUSE."""
    try:
        # Start FUSE in background
        proc = subprocess.Popen(
            [str(fuse_binary), '-f', str(image_path), str(mount_point)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE
        )

        # Wait for mount
        start_time = time.time()
        while time.time() - start_time < timeout:
            if mount_point.is_mount():
                log_info(f"Mounted {image_path} at {mount_point}")
                return True
            time.sleep(0.1)

        log_error("Timeout waiting for mount")
        proc.terminate()
        return False
    except Exception as e:
        log_error(f"Failed to mount: {e}")
        return False


def unmount_image(mount_point: Path) -> bool:
    """Unmount SquashFS image."""
    if not mount_point.is_mount():
        return True

    try:
        # Try fusermount3 first, then fusermount, then umount
        for cmd in [['fusermount3', '-u'], ['fusermount', '-u'], ['umount']]:
            try:
                subprocess.run(cmd + [str(mount_point)], check=True, timeout=10)
                log_info(f"Unmounted {mount_point}")
                return True
            except (subprocess.CalledProcessError, FileNotFoundError):
                continue

        log_error("Failed to unmount")
        return False
    except Exception as e:
        log_error(f"Unmount error: {e}")
        return False


class ReadTests:
    """File read tests."""

    def __init__(self, mount_point: Path, expected: dict, verbose: bool = False):
        self.mount_point = mount_point
        self.expected = expected
        self.verbose = verbose
        self.results = TestResult()

    def run_test(self, test_func):
        """Run a single test function and record result."""
        test_name = test_func.__name__
        log_info(f"Running: {test_name}")
        try:
            test_func()
        except AssertionError as e:
            self.results.add_fail(test_name, str(e))
            log_error(f"FAILED: {test_name} - {e}")
        except Exception as e:
            self.results.add_fail(test_name, f"Exception: {e}")
            log_error(f"ERROR: {test_name} - {e}")
        else:
            self.results.add_pass()
            log_debug(f"PASSED: {test_name}", self.verbose)

    def test_file_exists(self):
        """Test that expected files exist."""
        for rel_path in self.expected.keys():
            file_path = self.mount_point / rel_path
            assert file_path.exists(), f"File not found: {rel_path}"
            log_debug(f"File exists: {rel_path}", self.verbose)

    def test_file_sizes(self):
        """Test that file sizes match expected."""
        for rel_path, (expected_size, _) in self.expected.items():
            file_path = self.mount_point / rel_path
            actual_size = file_path.stat().st_size
            assert actual_size == expected_size, \
                f"Size mismatch for {rel_path}: expected {expected_size}, got {actual_size}"

    def test_read_small_file(self):
        """Test reading small file completely."""
        file_path = self.mount_point / "small.txt"
        content = file_path.read_text()
        assert content == "Hello, SquashFS!", f"Content mismatch: {content}"

    def test_read_empty_file(self):
        """Test reading empty file."""
        file_path = self.mount_point / "empty.txt"
        content = file_path.read_bytes()
        assert len(content) == 0, "Empty file should have 0 bytes"

    def test_read_large_file(self):
        """Test reading large file in chunks."""
        file_path = self.mount_point / "data" / "large.bin"
        expected_size, expected_hash = self.expected["data/large.bin"]

        # Read entire file and verify hash
        md5 = hashlib.md5()
        with open(file_path, 'rb') as f:
            while chunk := f.read(CHUNK_SIZES[-1]):
                md5.update(chunk)

        actual_hash = md5.hexdigest()
        assert actual_hash == expected_hash, \
            f"Hash mismatch for large.bin: expected {expected_hash}, got {actual_hash}"

    def test_sequential_read(self):
        """Test sequential read of entire file."""
        file_path = self.mount_point / "data" / "large.bin"
        expected_size = self.expected["data/large.bin"][0]

        total_read = 0
        with open(file_path, 'rb') as f:
            while chunk := f.read(BLOCK_SIZE):
                total_read += len(chunk)

        assert total_read == expected_size, \
            f"Bytes read mismatch: expected {expected_size}, got {total_read}"

    def test_random_access_small(self):
        """Test random access reads with small offsets."""
        file_path = self.mount_point / "data" / "large.bin"
        expected_size = self.expected["data/large.bin"][0]

        random.seed(12345)
        with open(file_path, 'rb') as f:
            for _ in range(100):
                offset = random.randint(0, expected_size - 100)
                f.seek(offset)
                data = f.read(100)
                assert len(data) == 100, \
                    f"Short read at offset {offset}: expected 100, got {len(data)}"

    def test_random_access_large(self):
        """Test random access reads with varying sizes."""
        file_path = self.mount_point / "data" / "large.bin"
        expected_size = self.expected["data/large.bin"][0]

        random.seed(54321)
        with open(file_path, 'rb') as f:
            for _ in range(50):
                offset = random.randint(0, max(0, expected_size - 10000))
                size = random.randint(1, 10000)
                f.seek(offset)
                data = f.read(size)
                assert len(data) == size or f.tell() == expected_size, \
                    f"Unexpected read size at offset {offset}: expected {size}, got {len(data)}"

    def test_read_at_boundaries(self):
        """Test reads at block boundaries."""
        file_path = self.mount_point / "data" / "large.bin"
        expected_size = self.expected["data/large.bin"][0]

        with open(file_path, 'rb') as f:
            # Read exactly at block boundary
            f.seek(BLOCK_SIZE)
            data = f.read(BLOCK_SIZE)
            assert len(data) == BLOCK_SIZE, "Short read at boundary"

            # Read crossing block boundary
            f.seek(BLOCK_SIZE - 100)
            data = f.read(200)
            assert len(data) == 200, "Short read crossing boundary"

            # Read at file end
            f.seek(expected_size - 100)
            data = f.read(200)
            assert len(data) == 100, "Read at end should return remaining bytes"

    def test_read_partial_blocks(self):
        """Test reading partial blocks."""
        file_path = self.mount_point / "data" / "large.bin"
        expected_size = self.expected["data/large.bin"][0]

        with open(file_path, 'rb') as f:
            for chunk_size in [1, 13, 99, 256, 1000, 4095]:
                f.seek(0)
                total = 0
                while chunk := f.read(chunk_size):
                    total += len(chunk)
                    if len(chunk) < chunk_size:
                        break
                assert total == expected_size, \
                    f"Partial read mismatch with chunk size {chunk_size}"

    def test_seek_operations(self):
        """Test seek operations."""
        file_path = self.mount_point / "data" / "large.bin"
        expected_size = self.expected["data/large.bin"][0]

        with open(file_path, 'rb') as f:
            # Seek from start
            f.seek(1000)
            assert f.tell() == 1000, "Seek from start failed"

            # Seek from current
            f.seek(100, 1)
            assert f.tell() == 1100, "Seek from current failed"

            # Seek from end
            f.seek(-100, 2)
            assert f.tell() == expected_size - 100, "Seek from end failed"

            # Seek to start
            f.seek(0)
            assert f.tell() == 0, "Seek to start failed"

            # Seek to end
            f.seek(0, 2)
            assert f.tell() == expected_size, "Seek to end failed"

    def test_read_medium_files(self):
        """Test reading various medium-sized files."""
        for size in [1024, 4096, 65536, 1048576]:
            rel_path = f"data/medium_{size}.bin"
            file_path = self.mount_point / rel_path
            expected_size, expected_hash = self.expected[rel_path]

            actual_hash = hash_file(file_path)
            assert actual_hash == expected_hash, \
                f"Hash mismatch for {rel_path}"

    def test_read_boundary_file(self):
        """Test file that exactly matches block size."""
        file_path = self.mount_point / "data" / "boundary.bin"
        expected_size, expected_hash = self.expected["data/boundary.bin"]

        # Verify size
        assert file_path.stat().st_size == 4096, "Boundary file size incorrect"

        # Verify content
        with open(file_path, 'rb') as f:
            content = f.read()
            assert content == b'B' * 4096, "Boundary file content incorrect"

    def test_read_cross_boundary_file(self):
        """Test file that crosses block boundary."""
        file_path = self.mount_point / "data" / "cross_boundary.bin"
        expected_size, expected_hash = self.expected["data/cross_boundary.bin"]

        # Verify size
        assert file_path.stat().st_size == 4097, "Cross-boundary file size incorrect"

        # Verify content
        with open(file_path, 'rb') as f:
            content = f.read()
            assert content == b'C' * 4097, "Cross-boundary file content incorrect"

    def test_read_all_small_files(self):
        """Test reading all small files."""
        for i in range(100):
            rel_path = f"small/file_{i:04d}.txt"
            file_path = self.mount_point / rel_path
            expected_content = f"Small file number {i}"
            actual_content = file_path.read_text()
            assert actual_content == expected_content, \
                f"Content mismatch for {rel_path}"

    def test_read_performance(self):
        """Test read throughput."""
        file_path = self.mount_point / "data" / "large.bin"
        expected_size = self.expected["data/large.bin"][0]

        # Sequential read performance
        start_time = time.time()
        with open(file_path, 'rb') as f:
            while f.read(65536):
                pass
        elapsed = time.time() - start_time

        throughput = expected_size / elapsed / (1024 * 1024)  # MB/s
        log_info(f"Sequential read throughput: {throughput:.2f} MB/s")

        # This is just informational, not a hard assertion
        assert throughput > 0, "Read should complete"

    def test_concurrent_reads(self):
        """Test concurrent reads from multiple file handles."""
        import threading
        import queue

        file_path = self.mount_point / "data" / "large.bin"
        expected_size, expected_hash = self.expected["data/large.bin"]
        errors = queue.Queue()

        def read_chunk(thread_id, offset, size):
            try:
                with open(file_path, 'rb') as f:
                    f.seek(offset)
                    data = f.read(size)
                    if len(data) != size and offset + size < expected_size:
                        errors.put(f"Thread {thread_id}: short read at {offset}")
            except Exception as e:
                errors.put(f"Thread {thread_id}: {e}")

        threads = []
        random.seed(99999)
        for i in range(10):
            offset = random.randint(0, max(0, expected_size - 10000))
            size = random.randint(1000, 10000)
            t = threading.Thread(target=read_chunk, args=(i, offset, size))
            threads.append(t)
            t.start()

        for t in threads:
            t.join(timeout=30)

        while not errors.empty():
            error = errors.get()
            assert False, error


def main():
    parser = argparse.ArgumentParser(description='SquashFS-FUSE read tests')
    parser.add_argument('-m', '--mount', help='Mount point (uses existing if provided)')
    parser.add_argument('-v', '--verbose', action='store_true', help='Verbose output')
    parser.add_argument('-f', '--fuse-binary', help='Path to squashfs-fuse binary')
    parser.add_argument('--keep', action='store_true', help='Keep temporary files')
    args = parser.parse_args()

    print("=" * 50)
    print("  SquashFS-FUSE Read Tests")
    print("=" * 50)
    print()

    # Find FUSE binary
    fuse_binary = None
    if args.fuse_binary:
        fuse_binary = Path(args.fuse_binary)
    else:
        project_root = Path(__file__).parent.parent.parent
        for path in [
            project_root / "build" / "squashfs-fuse",
            project_root / "squashfs-fuse",
        ]:
            if path.exists() and path.is_file():
                fuse_binary = path
                break

    if not fuse_binary:
        log_error("No FUSE binary found, skipping mount tests")
        print("Please specify with -f or build the project")
        return 1

    log_info(f"Using FUSE binary: {fuse_binary}")

    # Setup test environment
    temp_dir = None
    mount_point = None

    try:
        if args.mount:
            mount_point = Path(args.mount)
        else:
            temp_dir = Path(tempfile.mkdtemp(prefix="squashfs-read-test-"))
            mount_point = temp_dir / "mnt"
            mount_point.mkdir()

            # Create test content
            content_dir = temp_dir / "content"
            content_dir.mkdir()
            log_info("Creating test content...")
            expected = create_test_content(content_dir)

            # Create SquashFS image
            image_path = temp_dir / "test.sqfs"
            if not create_squashfs_image(content_dir, image_path):
                return 1

            # Mount image
            if not mount_image(fuse_binary, image_path, mount_point):
                return 1

        # Run tests
        log_info(f"Mount point: {mount_point}")
        tests = ReadTests(mount_point, expected, args.verbose)

        # Basic tests
        tests.run_test(tests.test_file_exists)
        tests.run_test(tests.test_file_sizes)

        # Read correctness tests
        tests.run_test(tests.test_read_small_file)
        tests.run_test(tests.test_read_empty_file)
        tests.run_test(tests.test_read_large_file)
        tests.run_test(tests.test_sequential_read)

        # Random access tests
        tests.run_test(tests.test_random_access_small)
        tests.run_test(tests.test_random_access_large)
        tests.run_test(tests.test_read_at_boundaries)

        # Partial block tests
        tests.run_test(tests.test_read_partial_blocks)

        # Seek tests
        tests.run_test(tests.test_seek_operations)

        # Various file size tests
        tests.run_test(tests.test_read_medium_files)
        tests.run_test(tests.test_read_boundary_file)
        tests.run_test(tests.test_read_cross_boundary_file)
        tests.run_test(tests.test_read_all_small_files)

        # Performance tests
        tests.run_test(tests.test_read_performance)

        # Concurrency tests
        tests.run_test(tests.test_concurrent_reads)

    finally:
        # Cleanup
        if mount_point and not args.mount:
            unmount_image(mount_point)

        if temp_dir and not args.keep:
            import shutil
            shutil.rmtree(temp_dir, ignore_errors=True)

    # Print summary
    print(tests.results.summary())
    return tests.results.exit_code()


if __name__ == '__main__':
    sys.exit(main())