#!/usr/bin/env bash

# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

LIBDIR="$1"
shift

if ! command -v patchelf >/dev/null 2>&1; then
    echo "patchelf not found, skipping ELF fix"
    exit 0
fi

# Get the interpreter from a known working binary (ls)
LS_PATH=$(command -v ls)
GUIX_INTERP=$(patchelf --print-interpreter "$LS_PATH" 2>/dev/null)

if [ -z "$GUIX_INTERP" ]; then
    echo "Could not detect interpreter, skipping"
    exit 0
fi

echo "Detected interpreter: $GUIX_INTERP"

# Find and copy libgcc_s.so.1 into our lib directory
LIBGCC_SRC=""

# Method 1: Use gcc to find it
if command -v gcc >/dev/null 2>&1; then
    GCC_LIBDIR=$(dirname "$(gcc -print-libgcc-file-name)" 2>/dev/null)
    if [ -f "$GCC_LIBDIR/libgcc_s.so.1" ]; then
        LIBGCC_SRC="$GCC_LIBDIR/libgcc_s.so.1"
    else
        GCC_PATH=$(command -v gcc)
        GCC_PREFIX=$(dirname "$(dirname "$GCC_PATH")")
        if [ -f "$GCC_PREFIX/lib/libgcc_s.so.1" ]; then
            LIBGCC_SRC="$GCC_PREFIX/lib/libgcc_s.so.1"
        fi
    fi
fi

# Method 2: Search LIBRARY_PATH
if [ -z "$LIBGCC_SRC" ] && [ -n "$LIBRARY_PATH" ]; then
    IFS=':' read -ra LIB_PATHS <<< "$LIBRARY_PATH"
    for libpath in "${LIB_PATHS[@]}"; do
        if [ -f "$libpath/libgcc_s.so.1" ]; then
            LIBGCC_SRC="$libpath/libgcc_s.so.1"
            break
        fi
    done
fi

if [ -n "$LIBGCC_SRC" ]; then
    # Resolve symlinks and copy the actual file
    LIBGCC_REAL=$(readlink -f "$LIBGCC_SRC")
    echo "Copying libgcc_s.so.1 from: $LIBGCC_REAL"
    cp "$LIBGCC_REAL" "$LIBDIR/libgcc_s.so.1"
else
    echo "WARNING: Could not find libgcc_s.so.1 to copy"
fi

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

echo "Verifying first binary:"
patchelf --print-interpreter "$1"
patchelf --print-rpath "$1"
