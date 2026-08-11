#!/usr/bin/env bash
#
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C.UTF-8

export CONTAINER_NAME=ci_native_rust
export HOST=x86_64-pc-linux-gnu
export DEP_OPTS="RUST=1 NO_QT=1 NO_QR=1"
export RUN_FUNCTIONAL_TESTS="false"
export GOAL="install"
export BITCOIN_CONFIG="--enable-rust --with-gui=no --enable-reduce-exports"
