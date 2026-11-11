#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""Trim the local clang-tidy cache to an allocated-size limit, oldest files first."""

import argparse
from pathlib import Path
import subprocess


def cache_size(cache_dir: Path) -> int:
    return int(subprocess.check_output(["du", "-sk", str(cache_dir)], text=True, encoding="utf8").split()[0]) * 1024


def cleanup(cache_dir: Path, max_size_mb: int) -> None:
    limit = max_size_mb * 1024 * 1024
    size = cache_size(cache_dir)
    if size <= limit:
        return

    print(f"Cache size {size / 1024 / 1024:.1f} MiB exceeds limit {max_size_mb} MiB, cleaning old entries...")
    files = sorted((path.stat().st_mtime_ns, path) for path in cache_dir.rglob("*") if path.is_file())
    removed = 0
    for _, path in files:
        path.unlink()
        removed += 1
        parent = path.parent
        while parent != cache_dir:
            try:
                parent.rmdir()
            except OSError:
                break
            parent = parent.parent
        # Outputs vary greatly in size, and stats files count too. Check bytes,
        # not a fixed percentage of files, including the remaining directories.
        size = cache_size(cache_dir)
        if size <= limit:
            break

    print(f"Removed {removed} oldest cache files; cache size after cleanup: {size / 1024 / 1024:.1f} MiB")
    if size > limit:
        raise RuntimeError("Could not trim ctcache to its size limit")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("cache_dir", type=Path)
    parser.add_argument("max_size_mb", type=int)
    args = parser.parse_args()
    if args.max_size_mb <= 0:
        parser.error("max_size_mb must be positive")
    cleanup(args.cache_dir.resolve(), args.max_size_mb)


if __name__ == "__main__":
    main()
