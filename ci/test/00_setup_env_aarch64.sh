#!/usr/bin/env bash
#
# Copyright (c) 2019-2021 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C.UTF-8

export HOST=aarch64-linux-gnu
export DPKG_ADD_ARCH="arm64"
export PACKAGES="python3-zmq g++-11-aarch64-linux-gnu busybox libc6:arm64 libstdc++6:arm64 libfontconfig1:arm64 libxcb1:arm64"
export CONTAINER_NAME=ci_aarch64_linux
export CHECK_DOC=0
export USE_BUSY_BOX=true
export RUN_UNIT_TESTS=false
export RUN_FUNCTIONAL_TESTS=false
export GOAL="install"
export DEP_OPTS="CC=aarch64-linux-gnu-gcc-11 CXX=aarch64-linux-gnu-g++-11"
export BITCOIN_CONFIG="-DREDUCE_EXPORTS=ON"
