#!/usr/bin/env python3

# Copyright (c) 2021-2022 The Zcash developers
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

import hashlib
import re
import sys
import urllib.request
from pathlib import Path

# Corresponds to 'hosts/*.mk'
CROSS_TARGETS = [
    # FreeBSD
    "x86_64-unknown-freebsd",
    # Linux
    "aarch64-unknown-linux-musl",
    "armv7-unknown-linux-musleabihf",
    "powerpc64le-unknown-linux-musl",
    "riscv64gc-unknown-linux-musl",
    "x86_64-unknown-linux-musl",
    # Windows
    "x86_64-pc-windows-gnu",
    # macOS
    "aarch64-apple-darwin",
    "x86_64-apple-darwin",
]

# Corresponds to 'builders/*.mk'
NATIVE_TARGETS = [
    # FreeBSD
    ("x86_64-unknown-freebsd", "x86_64_freebsd"),
    # Linux
    ("aarch64-unknown-linux-gnu", "aarch64_linux"),
    ("x86_64-unknown-linux-gnu", "x86_64_linux"),
    # macOS
    ("aarch64-apple-darwin", "aarch64_darwin"),
    ("x86_64-apple-darwin", "x86_64_darwin"),
]

def get_rust_version(makefile_path: Path) -> str:
    content = makefile_path.read_text()
    match = re.search(r"\$\(package\)_version:=(.+)", content)
    if not match:
        raise RuntimeError("Could not find Rust version in makefile")
    return match.group(1).strip()


def compute_sha256(url: str) -> str:
    hasher = hashlib.sha256()
    with urllib.request.urlopen(url) as response:
        while chunk := response.read(8192):
            hasher.update(chunk)
    return hasher.hexdigest()


def update_hash_in_file(makefile_path: Path, pattern: str, new_hash: str) -> None:
    content = makefile_path.read_text()
    regex = re.compile(rf"^(\$\(package\)_{pattern}:=).*$", re.MULTILINE)
    if not regex.search(content):
        raise RuntimeError(f"Could not find pattern {pattern} in makefile")
    new_content = regex.sub(rf"\g<1>{new_hash}", content)
    makefile_path.write_text(new_content)


def update_rust_hash(makefile_path: Path, rust_version: str, rust_target: str, makefile_id: str) -> None:
    url = f"https://static.rust-lang.org/dist/rust-{rust_version}-{rust_target}.tar.gz"
    hash_value = compute_sha256(url)
    update_hash_in_file(makefile_path, f"sha256_hash_{makefile_id}", hash_value)
    print(f"  Updated sha256_hash_{makefile_id}")


def update_stdlib_hash(makefile_path: Path, rust_version: str, rust_target: str) -> None:
    url = f"https://static.rust-lang.org/dist/rust-std-{rust_version}-{rust_target}.tar.gz"
    hash_value = compute_sha256(url)
    update_hash_in_file(makefile_path, f"sha256_hash_{rust_target}", hash_value)
    print(f"  Updated sha256_hash_{rust_target}")


def main() -> int:
    script_dir = Path(__file__).resolve().parent
    native_rust_path = script_dir / "../../depends/packages/native_rust.mk"
    native_rust_path = native_rust_path.resolve()
    rust_stdlib_path = script_dir / "../../depends/packages/rust_stdlib.mk"
    rust_stdlib_path = rust_stdlib_path.resolve()

    if not native_rust_path.exists():
        print(f"Error: {native_rust_path} not found", file=sys.stderr)
        return 1

    if not rust_stdlib_path.exists():
        print(f"Error: {rust_stdlib_path} not found", file=sys.stderr)
        return 1

    rust_version = get_rust_version(native_rust_path)

    print(f"Rust version: {rust_version}\n")
    print("Updating native compiler hashes:")

    for rust_target, makefile_id in NATIVE_TARGETS:
        update_rust_hash(native_rust_path, rust_version, rust_target, makefile_id)

    print("\nUpdating stdlib hashes:")
    for rust_target in CROSS_TARGETS:
        update_stdlib_hash(rust_stdlib_path, rust_version, rust_target)

    print("\nDone!")
    return 0


if __name__ == "__main__":
    sys.exit(main())
