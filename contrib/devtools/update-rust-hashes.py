#!/usr/bin/env python3
# Copyright (c) 2021-2022 The Zcash developers
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
'''
Refresh the sha256 pins of the prebuilt Rust toolchain (native_rust.mk) and
the per-host standard libraries (rust_stdlib.mk) for the version set in
native_rust.mk, or verify them with --check.

Rewriting downloads every archive from static.rust-lang.org, hashes it locally
and requires the result to match the .sha256 file published next to it.
--check only compares the pins with the published .sha256 files.
'''
import argparse
import hashlib
import http.client
import re
import sys
import urllib.request
from pathlib import Path

DIST_URL = "https://static.rust-lang.org/dist"
TIMEOUT = 60  # seconds without data before a request fails
PACKAGES_DIR = Path(__file__).resolve().parents[2] / "depends" / "packages"
NATIVE_RUST_MK = PACKAGES_DIR / "native_rust.mk"
RUST_STDLIB_MK = PACKAGES_DIR / "rust_stdlib.mk"


def pins(content: str, kind: str) -> dict:
    """Return {id: value} for every `$(package)_<kind>_<id>:=<value>` line."""
    return dict(re.findall(rf"^\$\(package\)_{kind}_(\w+):=(\S+)$", content, re.MULTILINE))


def published_sha256(url: str) -> str:
    """Return the hash in the `<hash>  <file>` line of `<url>.sha256`."""
    with urllib.request.urlopen(f"{url}.sha256", timeout=TIMEOUT) as response:
        return response.read().decode().split()[0]


def sha256_of(url: str, attempts: int = 3) -> str:
    for _ in range(attempts):
        hasher = hashlib.sha256()
        received = 0
        try:
            with urllib.request.urlopen(url, timeout=TIMEOUT) as response:
                length = response.headers["Content-Length"]
                while chunk := response.read(1 << 20):
                    hasher.update(chunk)
                    received += len(chunk)
        except (http.client.IncompleteRead, OSError) as error:
            print(f"warning: {url}: {error}", file=sys.stderr)
            continue
        if length is None or received == int(length):
            return hasher.hexdigest()
        print(f"warning: {url}: download ended after {received} of {length} bytes", file=sys.stderr)
    sys.exit(f"error: {url}: no complete download in {attempts} attempts")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true", help="verify the pinned hashes instead of rewriting them")
    args = parser.parse_args()

    native = NATIVE_RUST_MK.read_text(encoding="utf-8")
    stdlib = RUST_STDLIB_MK.read_text(encoding="utf-8")
    version_pin = re.search(r"^\$\(package\)_version:=(\S+)$", native, re.MULTILINE)
    if version_pin is None:
        sys.exit(f"error: no version pin in {NATIVE_RUST_MK}")
    version = version_pin.group(1)

    archives = []  # (makefile, pin id, archive name)
    build_targets = pins(native, "build_target")
    for build_id, file_name in pins(native, "file_name").items():
        file_name = file_name.replace("$($(package)_version)", version)
        if build_id not in build_targets:
            sys.exit(f"error: {NATIVE_RUST_MK} has no build_target_{build_id} pin")
        file_name = file_name.replace(f"$($(package)_build_target_{build_id})", build_targets[build_id])
        archives.append((NATIVE_RUST_MK, build_id, file_name))
    for host_id, target in pins(stdlib, "target").items():
        archives.append((RUST_STDLIB_MK, host_id, f"rust-std-{version}-{target}.tar.gz"))

    contents = {NATIVE_RUST_MK: native, RUST_STDLIB_MK: stdlib}
    stale = []
    for makefile, pin_id, archive in archives:
        pinned = pins(contents[makefile], "sha256_hash").get(pin_id)
        if pinned is None:
            sys.exit(f"error: {makefile} has no sha256_hash_{pin_id} pin")
        url = f"{DIST_URL}/{archive}"
        actual = published_sha256(url)
        if not args.check:
            downloaded = sha256_of(url)
            if downloaded != actual:
                sys.exit(f"error: {archive} hashes to {downloaded}, but its published .sha256 says {actual}")
        print(f"{actual}  {archive}")
        if actual != pinned:
            stale.append(archive)
            contents[makefile] = re.sub(rf"^(\$\(package\)_sha256_hash_{pin_id}:=)\S*$",
                                        rf"\g<1>{actual}", contents[makefile], flags=re.MULTILINE)

    if args.check:
        for archive in stale:
            print(f"error: pinned hash for {archive} does not match", file=sys.stderr)
        return 1 if stale else 0

    for makefile, content in contents.items():
        makefile.write_text(content, encoding="utf-8")
    print(f"Updated {len(stale)} of {len(archives)} pins for Rust {version}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
