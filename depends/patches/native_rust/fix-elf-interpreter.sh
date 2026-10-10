#!/usr/bin/env bash
export LC_ALL=C
set -euo pipefail

# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

# Usage: fix-elf-interpreter.sh <lib dir> <binary>...
#
# The prebuilt Rust binaries expect a conventional /lib64/ld-linux-* loader
# and system library directories. Inside a Guix environment neither exists, so
# the binaries get the environment's loader, an $ORIGIN-relative RPATH, and
# copies of the runtime libraries they need. Anywhere else they run as they
# are, and this script does nothing.

LIBDIR="$1"
shift

case "$(readlink -f "$(command -v ls)")" in
    /gnu/store/*) ;;
    *) exit 0 ;;
esac

if ! command -v patchelf >/dev/null 2>&1; then
    echo "ERROR: patchelf is required inside the Guix environment but was not found" >&2
    exit 1
fi

# Get the interpreter from a known working binary (ls)
LS_PATH=$(command -v ls)
GUIX_INTERP=$(patchelf --print-interpreter "$LS_PATH")

echo "Detected interpreter: $GUIX_INTERP"

# Find and copy runtime libraries the prebuilt binaries need into our lib
# directory so the $ORIGIN-based RPATH can resolve them.
for libname in libgcc_s.so.1 libz.so.1; do
    LIB_SRC=""

    # Method 1: Use gcc to find it
    if command -v gcc >/dev/null 2>&1; then
        CANDIDATE=$(gcc -print-file-name="$libname" 2>/dev/null || true)
        if [ -f "$CANDIDATE" ]; then
            LIB_SRC="$CANDIDATE"
        else
            GCC_PATH=$(command -v gcc)
            GCC_PREFIX=$(dirname "$(dirname "$GCC_PATH")")
            if [ -f "$GCC_PREFIX/lib/$libname" ]; then
                LIB_SRC="$GCC_PREFIX/lib/$libname"
            fi
        fi
    fi

    # Method 2: Search LIBRARY_PATH
    if [ -z "$LIB_SRC" ] && [ -n "${LIBRARY_PATH:-}" ]; then
        IFS=':' read -ra LIB_PATHS <<< "$LIBRARY_PATH"
        for libpath in "${LIB_PATHS[@]}"; do
            if [ -f "$libpath/$libname" ]; then
                LIB_SRC="$libpath/$libname"
                break
            fi
        done
    fi

    # Method 3: the Guix profile. contrib/guix/libexec/build.sh narrows
    # LIBRARY_PATH to the gcc-toolchain outputs, so libraries provisioned by
    # contrib/guix/manifest.scm (zlib) are only reachable through the
    # profile union that guix shell exposes as GUIX_ENVIRONMENT.
    if [ -z "$LIB_SRC" ] && [ -n "${GUIX_ENVIRONMENT:-}" ] && [ -f "$GUIX_ENVIRONMENT/lib/$libname" ]; then
        LIB_SRC="$GUIX_ENVIRONMENT/lib/$libname"
    fi

    if [ -z "$LIB_SRC" ]; then
        # There are no default library search paths inside Guix, so a
        # toolchain missing one of these libraries is nonfunctional and must
        # not be staged and cached.
        echo "ERROR: $libname is required inside the Guix environment but was not found" >&2
        exit 1
    fi
    # Resolve symlinks and copy the actual file
    LIB_REAL=$(readlink -f "$LIB_SRC")
    echo "Copying $libname from: $LIB_REAL"
    cp "$LIB_REAL" "$LIBDIR/$libname"
done

# RPATH just needs $ORIGIN/../lib - everything is self-contained
GUIX_RPATH="\$ORIGIN/../lib"
echo "Using RPATH: $GUIX_RPATH"

for binary in "$@"; do
    if [ -f "$binary" ]; then
        echo "Patching: $binary"
        patchelf --set-interpreter "$GUIX_INTERP" "$binary"
        patchelf --set-rpath "$GUIX_RPATH" "$binary"
    fi
done

if [ $# -gt 0 ]; then
    echo "Verifying first binary:"
    patchelf --print-interpreter "$1"
    patchelf --print-rpath "$1"
fi
