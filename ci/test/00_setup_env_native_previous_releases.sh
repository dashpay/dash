#!/usr/bin/env bash
#
# Copyright (c) 2019-2022 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C.UTF-8

export CONTAINER_NAME=ci_native_previous_releases
export HOST=x86_64-pc-linux-gnu
export PACKAGES="python3-zmq"
# CI builds linux64 against the linux64_platform_gui depends: set its depends
# options in 00_setup_env_native_platform_gui.sh, not here.
export DEP_OPTS=""
export TEST_RUNNER_EXTRA="--previous-releases --coverage --extended --exclude feature_pruning,feature_dbcrash"  # Run extended tests so that coverage does not fail, but exclude the very slow dbcrash
export RUN_UNIT_TESTS_SEQUENTIAL="true"
export RUN_UNIT_TESTS="false"
export GOAL="install"
export DOWNLOAD_PREVIOUS_RELEASES="true"
export BITCOIN_CONFIG="\
 -DWITH_ZMQ=ON -DBUILD_CONSENSUS_LIB=OFF -DREDUCE_EXPORTS=ON \
 -DCMAKE_C_COMPILER=gcc-14 \
 -DCMAKE_CXX_COMPILER=g++-14 \
 -DAPPEND_CPPFLAGS='-DBOOST_MULTI_INDEX_ENABLE_SAFE_MODE -DDEBUG_LOCKORDER -DDEBUG_LOCKCONTENTION' \
 -DAPPEND_LDFLAGS='-static-libstdc++' \
"
export RUN_STDLIB_PATH_CHECK=true
export RUN_CHECK_NO_RUST="true"
