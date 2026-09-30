#!/usr/bin/env bash
#
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C.UTF-8

export CONTAINER_NAME=ci_native_platform_gui
export HOST=x86_64-pc-linux-gnu
export DEP_OPTS="PLATFORM_GUI=1"
export RUN_FUNCTIONAL_TESTS="false"
export RUN_CHECK_NO_RUST="true"
export GOAL="install"
export BITCOIN_CONFIG="--enable-zmq --with-libs=no --enable-reduce-exports LDFLAGS=-static-libstdc++"
