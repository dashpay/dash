#!/usr/bin/env python3

# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

def get_cxx_version(makefile_path: Path) -> str:
    content = makefile_path.read_text()
    match = re.search(r"\$\(package\)_version:=(.+)", content)
    if not match:
        raise RuntimeError("Could not find cxx version in makefile")
    return match.group(1).strip()


def main() -> int:
    script_dir = Path(__file__).resolve().parent
    repo_root = script_dir / "../.."
    repo_root = repo_root.resolve()

    makefile_path = repo_root / "depends/packages/native_cxxbridge.mk"
    if not makefile_path.exists():
        print(f"Error: {makefile_path} not found", file=sys.stderr)
        return 1

    version = get_cxx_version(makefile_path)
    print(f"cxx version: {version}")

    tarball_path = repo_root / f"depends/sources/native_cxxbridge-{version}.tar.gz"
    if not tarball_path.exists():
        print(f"Error: {tarball_path} not found", file=sys.stderr)
        print("Run 'make -C depends download-one PKG=native_cxxbridge' first", file=sys.stderr)
        return 1

    toolchain_path = repo_root / "rust-toolchain.toml"
    if not toolchain_path.exists():
        print(f"Error: {toolchain_path} not found", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory() as tmp_dir:
        tmp_path = Path(tmp_dir)
        print(f"Working in {tmp_path}")

        # Copy rust-toolchain.toml
        shutil.copy(toolchain_path, tmp_path / "rust-toolchain.toml")

        # Extract tarball
        print(f"Extracting {tarball_path}")
        with tarfile.open(tarball_path, "r:gz") as tar:
            tar.extractall(tmp_path)

        cxx_dir = tmp_path / f"cxx-{version}"
        if not cxx_dir.exists():
            print(f"Error: Expected directory {cxx_dir} not found after extraction", file=sys.stderr)
            return 1

        # Copy rust-toolchain.toml into cxx directory
        shutil.copy(toolchain_path, cxx_dir / "rust-toolchain.toml")

        # Run cargo check
        print("Running cargo check --release --package=cxxbridge-cmd --bin=cxxbridge")
        result = subprocess.run(
            ["cargo", "check", "--release", "--package=cxxbridge-cmd", "--bin=cxxbridge"],
            cwd=cxx_dir,
        )
        if result.returncode != 0:
            print("Error: cargo check failed", file=sys.stderr)
            return 1

        # Copy Cargo.lock to patches directory
        cargo_lock_src = cxx_dir / "Cargo.lock"
        cargo_lock_dst = repo_root / "depends/patches/native_cxxbridge/Cargo.lock"
        if not cargo_lock_src.exists():
            print(f"Error: {cargo_lock_src} not found after cargo check", file=sys.stderr)
            return 1

        cargo_lock_dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(cargo_lock_src, cargo_lock_dst)
        print(f"Copied Cargo.lock to {cargo_lock_dst}")

    print("\nDone!")
    return 0


if __name__ == "__main__":
    sys.exit(main())
