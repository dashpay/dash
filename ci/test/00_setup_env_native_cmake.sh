#!/usr/bin/env bash
#
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C.UTF-8

# Builds the tree with CMake instead of Autotools, on top of the very same
# depends prefix the linux64 job uses. HOST and DEP_OPTS must therefore stay in
# sync with 00_setup_env_native_qt5.sh, otherwise the depends cache cannot be
# shared and this job would have to build depends from scratch.
export CONTAINER_NAME=ci_native_cmake
export HOST=x86_64-pc-linux-gnu
export PACKAGES="python3-zmq qtbase5-dev qttools5-dev-tools libdbus-1-dev libharfbuzz-dev"
export DEP_OPTS=""
export RUN_UNIT_TESTS="true"
# Only the smoke list in ci/dash/test_integrationtests_cmake.sh; the Autotools
# jobs still run the full functional suite.
export RUN_FUNCTIONAL_TESTS="true"
export GOAL="install"
# The remaining feature flags (GUI, wallet, ZMQ, ...) come from
# depends/toolchain.cmake, which reflects what was actually built in depends.
export BITCOIN_CONFIG="-DBUILD_BENCH=ON"
