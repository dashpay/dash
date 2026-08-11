#!/usr/bin/env python3

# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

import hashlib
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path


def get_cxx_version(makefile_path: Path) -> str:
    content = makefile_path.read_text()
    match = re.search(r"\$\(package\)_version:=(.+)", content)
    if not match:
        raise RuntimeError("Could not find cxx version in makefile")
    return match.group(1).strip()


def download_and_hash(url: str, dest: Path) -> str:
    hasher = hashlib.sha256()
    dest.parent.mkdir(parents=True, exist_ok=True)
    temporary_path = None
    try:
        with tempfile.NamedTemporaryFile(dir=dest.parent, delete=False) as output:
            temporary_path = Path(output.name)
            with urllib.request.urlopen(url) as response:
                while chunk := response.read(8192):
                    hasher.update(chunk)
                    output.write(chunk)
        temporary_path.replace(dest)
    except BaseException:
        if temporary_path is not None:
            temporary_path.unlink(missing_ok=True)
        raise
    return hasher.hexdigest()


def write_stamp(stamps_dir: Path, version: str, sha256: str, file_name: str) -> None:
    stamps_dir.mkdir(parents=True, exist_ok=True)
    stamp_path = stamps_dir / f".stamp_fetched-native_cxxbridge-{version}-{sha256}.hash"
    stamp_path.write_text(f"{sha256}  {file_name}\n")


def update_value_in_file(path: Path, pattern: str, value: str) -> None:
    content = path.read_text()
    regex = re.compile(pattern, re.MULTILINE)
    new_content, replacements = regex.subn(
        lambda match: f"{match.group(1)}{value}{match.group(2) if match.lastindex == 2 else ''}", content
    )
    if replacements != 1:
        raise RuntimeError(f"Expected one matching value in {path}, found {replacements}")
    path.write_text(new_content)


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

    sources_dir = repo_root / "depends/sources"
    file_name = f"native_cxxbridge-{version}.tar.gz"
    tarball_path = sources_dir / file_name
    url = f"https://github.com/dtolnay/cxx/archive/refs/tags/{version}.tar.gz"
    print(f"Downloading {url}")
    hash_value = download_and_hash(url, tarball_path)
    print(f"sha256: {hash_value}")

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
            # getattr keeps this compatible with the older tarfile type stubs
            # used by the Python 3.10 lint environment. Supported Python 3.10
            # releases include the security filter at runtime.
            getattr(tar, "extractall")(tmp_path, filter="data")

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

        print("Updating the workspace cxx crates")
        result = subprocess.run(["cargo", "update", "-p", "cxx", "--precise", version], cwd=repo_root)
        if result.returncode != 0:
            print("Error: workspace cargo update failed", file=sys.stderr)
            return 1

        cargo_lock_dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy(cargo_lock_src, cargo_lock_dst)
        print(f"Copied Cargo.lock to {cargo_lock_dst}")

    update_value_in_file(makefile_path, r"^(\$\(package\)_sha256_hash:=).*$", hash_value)
    configure_path = repo_root / "configure.ac"
    update_value_in_file(configure_path, r'^(CXXBRIDGE_REQUIRED_VERSION=")[^"]*(")$', version)
    write_stamp(sources_dir / "download-stamps", version, hash_value, file_name)

    print("\nDone!")
    return 0


if __name__ == "__main__":
    sys.exit(main())
