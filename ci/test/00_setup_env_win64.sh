#!/usr/bin/env bash
#
# Copyright (c) 2019-2021 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C.UTF-8

export CONTAINER_NAME=ci_win64
export HOST=x86_64-w64-mingw32
export DPKG_ADD_ARCH="i386"
export PACKAGES="python3 nsis g++-mingw-w64-x86-64-posix wine-binfmt wine64 wine32 file"
export RUN_FUNCTIONAL_TESTS=false
export RUN_SECURITY_TESTS="false"
export GOAL="deploy"
# Prior to 11.0.0, the mingw-w64 headers were missing noreturn attributes, causing warnings when
# cross-compiling for Windows. https://sourceforge.net/p/mingw-w64/bugs/306/
# https://github.com/mingw-w64/mingw-w64/commit/1690994f515910a31b9fb7c7bd3a52d4ba987abe
export BITCOIN_CONFIG="--enable-gui --enable-reduce-exports --disable-miner --without-boost-process CXXFLAGS='-Wno-return-type -Wno-error=maybe-uninitialized -Wno-error=array-bounds'"
export DIRECT_WINE_EXEC_TESTS=true
