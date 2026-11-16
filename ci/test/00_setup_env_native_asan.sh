#!/usr/bin/env bash
#
# Copyright (c) 2019-2022 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C.UTF-8

export CONTAINER_NAME=ci_native_asan
export PACKAGES="clang-19 llvm-19 libclang-rt-19-dev python3-zmq qtbase5-dev qttools5-dev-tools libevent-dev libboost-dev libminiupnpc-dev libzmq3-dev libqrencode-dev"
export NO_DEPENDS=1
export TEST_RUNNER_EXTRA="--timeout-factor=4 -j4"  # Increase timeout because sanitizers slow down
export CI_LIMIT_STACK_SIZE=1
export GOAL="install"
export BITCOIN_CONFIG="\
 -DWITH_ZMQ=ON -DENABLE_CRASH_HOOKS=ON -DBUILD_GUI=ON -DWITH_BDB=OFF -DWITH_SQLITE=ON \
 -DWITH_MINIUPNPC=ON -DWITH_NATPMP=ON -DWITH_ZSTD=ON -DWITH_USDT=ON \
 -DSANITIZERS=address,float-divide-by-zero,integer,undefined \
 -DCMAKE_C_COMPILER=clang-19 \
 -DCMAKE_CXX_COMPILER=clang++-19 \
 -DAPPEND_CFLAGS='-ftrivial-auto-var-init=pattern' \
 -DAPPEND_CXXFLAGS='-ftrivial-auto-var-init=pattern' \
 -DAPPEND_CPPFLAGS='-DARENA_DEBUG' \
"
export PYZMQ=true
