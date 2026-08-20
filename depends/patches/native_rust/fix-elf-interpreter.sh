#!/usr/bin/env bash
export LC_ALL=C

# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

LIBDIR="$1"
shift

if ! command -v patchelf >/dev/null 2>&1; then
    # Inside a Guix environment the prebuilt binaries cannot run without
    # having their interpreter patched, so a missing patchelf is fatal there.
    case "$(command -v ls)" in
        /gnu/store/*)
            echo "ERROR: patchelf is required inside the Guix environment but was not found" >&2
            exit 1
            ;;
    esac
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

# Find and copy runtime libraries the prebuilt binaries need into our lib
# directory so the $ORIGIN-based RPATH can resolve them.
for libname in libgcc_s.so.1 libz.so.1; do
    LIB_SRC=""

    # Method 1: Use gcc to find it
    if command -v gcc >/dev/null 2>&1; then
        CANDIDATE=$(gcc -print-file-name="$libname" 2>/dev/null)
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
    if [ -z "$LIB_SRC" ] && [ -n "$LIBRARY_PATH" ]; then
        IFS=':' read -ra LIB_PATHS <<< "$LIBRARY_PATH"
        for libpath in "${LIB_PATHS[@]}"; do
            if [ -f "$libpath/$libname" ]; then
                LIB_SRC="$libpath/$libname"
                break
            fi
        done
    fi

    if [ -n "$LIB_SRC" ]; then
        # Resolve symlinks and copy the actual file
        LIB_REAL=$(readlink -f "$LIB_SRC")
        echo "Copying $libname from: $LIB_REAL"
        cp "$LIB_REAL" "$LIBDIR/$libname"
    else
        echo "WARNING: Could not find $libname to copy"
    fi
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

if [ -n "$1" ]; then
    echo "Verifying first binary:"
    patchelf --print-interpreter "$1"
    patchelf --print-rpath "$1"
fi
