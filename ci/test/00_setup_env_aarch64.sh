#!/usr/bin/env bash
#
# Copyright (c) 2019-2021 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C.UTF-8

export HOST=aarch64-linux-gnu
# The host arch is unknown, so we run the tests through qemu.
# If the host is arm64 and wants to run the tests natively, it can set QEMU_USER_CMD to the empty string.
if [ -z ${QEMU_USER_CMD+x} ]; then export QEMU_USER_CMD="${QEMU_USER_CMD:-"qemu-aarch64 -L /usr/aarch64-linux-gnu/"}"; fi
export DPKG_ADD_ARCH="arm64"
export PACKAGES="python3-zmq g++-aarch64-linux-gnu busybox libc6:arm64 libstdc++6:arm64 libfontconfig1:arm64 libxcb1:arm64"
if [ -n "$QEMU_USER_CMD" ]; then
  # Likely cross-compiling, so install the needed gcc and qemu-user
  export PACKAGES="$PACKAGES qemu-user"
fi
export CONTAINER_NAME=ci_aarch64_linux
# Use debian to avoid 404 apt errors when cross compiling
export CHECK_DOC=0
export USE_BUSY_BOX=true
export RUN_UNIT_TESTS=false
export RUN_FUNCTIONAL_TESTS=false
export GOAL="install"
export BITCOIN_CONFIG="--enable-reduce-exports"
