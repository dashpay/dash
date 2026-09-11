#!/usr/bin/env bash
#
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C.UTF-8

# Builds depends with PLATFORM_GUI=1 so mbedtls and the Platform-owned CXX
# binding archive (libdash_platform_cxx.a, built offline from vendored crates)
# are produced, hash-verified and installed into the depends prefix, then
# builds dash-qt against that prefix. The --enable-platform-gui configure flag
# and the platform_* unit-test suites arrive with the Platform client library
# and are added to BITCOIN_CONFIG there; until then this lane proves the
# depends knob end to end and that the enriched prefix stays link-compatible.
# Functional tests are skipped: there is no dashd-only surface to drive.
export CONTAINER_NAME=ci_native_platform_gui
export HOST=x86_64-pc-linux-gnu
export PACKAGES="python3-zmq qtbase5-dev qttools5-dev-tools libdbus-1-dev libharfbuzz-dev"
export DEP_OPTS="PLATFORM_GUI=1"
export RUN_UNIT_TESTS="true"
export RUN_UNIT_TESTS_SEQUENTIAL="false"
export RUN_FUNCTIONAL_TESTS="false"
export GOAL="install"
export BITCOIN_CONFIG="--with-gui=qt5 --enable-zmq --with-libs=no --enable-reduce-exports LDFLAGS=-static-libstdc++"
