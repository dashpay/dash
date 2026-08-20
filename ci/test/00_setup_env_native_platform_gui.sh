#!/usr/bin/env bash
#
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C.UTF-8

# Builds dash-qt with the optional Dash Platform GUI (--enable-platform-gui)
# turned on and runs the platform_* unit-test suites. PLATFORM_GUI=1 makes
# depends build mbedtls and the Platform-owned CXX binding archive; proof
# verification, DPP decoding and state-transition construction come from that
# archive. Functional tests are skipped: the feature is exercised by the gated
# C++ unit tests and there is no dashd-only surface to drive.
export CONTAINER_NAME=ci_native_platform_gui
export HOST=x86_64-pc-linux-gnu
export PACKAGES="python3-zmq qtbase5-dev qttools5-dev-tools libdbus-1-dev libharfbuzz-dev"
export DEP_OPTS="PLATFORM_GUI=1"
export RUN_UNIT_TESTS="true"
export RUN_UNIT_TESTS_SEQUENTIAL="false"
export RUN_FUNCTIONAL_TESTS="false"
export GOAL="install"
export BITCOIN_CONFIG="--enable-platform-gui --with-gui=qt5 --enable-zmq --with-libs=no --enable-reduce-exports LDFLAGS=-static-libstdc++"
