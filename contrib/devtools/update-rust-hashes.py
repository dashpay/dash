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

# Rust standard libraries provisioned in rust_stdlib.mk. Confined to the
# hosts we validate (the Guix release set); see rust_stdlib.mk.
CROSS_TARGETS = [
    # Linux
    "aarch64-unknown-linux-musl",
    "riscv64gc-unknown-linux-musl",
    "x86_64-unknown-linux-musl",
    # Windows
    "x86_64-pc-windows-gnu",
    # macOS
    "aarch64-apple-darwin",
    "x86_64-apple-darwin",
]

# Native compilers provisioned in native_rust.mk (build hosts for depends)
NATIVE_TARGETS = [
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


def update_version_in_file(path: Path, pattern: str, version: str) -> None:
    content = path.read_text()
    regex = re.compile(pattern, re.MULTILINE)
    new_content, replacements = regex.subn(
        lambda match: f"{match.group(1)}{version}{match.group(2) if match.lastindex == 2 else ''}", content
    )
    if replacements != 1:
        raise RuntimeError(f"Expected one version pin in {path}, found {replacements}")
    path.write_text(new_content)


def compute_rust_hash(rust_version: str, rust_target: str) -> str:
    url = f"https://static.rust-lang.org/dist/rust-{rust_version}-{rust_target}.tar.gz"
    return compute_sha256(url)


def compute_stdlib_hash(rust_version: str, rust_target: str) -> str:
    url = f"https://static.rust-lang.org/dist/rust-std-{rust_version}-{rust_target}.tar.gz"
    return compute_sha256(url)


def main() -> int:
    script_dir = Path(__file__).resolve().parent
    native_rust_path = script_dir / "../../depends/packages/native_rust.mk"
    native_rust_path = native_rust_path.resolve()
    rust_stdlib_path = script_dir / "../../depends/packages/rust_stdlib.mk"
    rust_stdlib_path = rust_stdlib_path.resolve()
    toolchain_path = (script_dir / "../../rust-toolchain.toml").resolve()
    configure_path = (script_dir / "../../configure.ac").resolve()

    for path in (native_rust_path, rust_stdlib_path, toolchain_path, configure_path):
        if not path.exists():
            print(f"Error: {path} not found", file=sys.stderr)
            return 1

    rust_version = get_rust_version(native_rust_path)

    print(f"Rust version: {rust_version}\n")
    print("Downloading native compiler hashes:")

    native_hashes = {}
    for rust_target, makefile_id in NATIVE_TARGETS:
        native_hashes[makefile_id] = compute_rust_hash(rust_version, rust_target)
        print(f"  Downloaded sha256_hash_{makefile_id}")

    print("\nDownloading stdlib hashes:")
    stdlib_hashes = {}
    for rust_target in CROSS_TARGETS:
        stdlib_hashes[rust_target] = compute_stdlib_hash(rust_version, rust_target)
        print(f"  Downloaded sha256_hash_{rust_target}")

    for makefile_id, hash_value in native_hashes.items():
        update_hash_in_file(native_rust_path, f"sha256_hash_{makefile_id}", hash_value)
    for rust_target, hash_value in stdlib_hashes.items():
        update_hash_in_file(rust_stdlib_path, f"sha256_hash_{rust_target}", hash_value)

    update_version_in_file(rust_stdlib_path, r"^(\$\(package\)_version:=).*$", rust_version)
    update_version_in_file(toolchain_path, r'^(channel = ")[^"]*(")$', rust_version)
    update_version_in_file(configure_path, r'^(RUSTC_REQUIRED_VERSION=")[^"]*(")$', rust_version)
    print("\nSynchronized rust_stdlib.mk, rust-toolchain.toml, and configure.ac")

    print("\nDone!")
    return 0


if __name__ == "__main__":
    sys.exit(main())
