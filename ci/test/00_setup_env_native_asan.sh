#!/usr/bin/env bash
#
# Copyright (c) 2019-2022 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C.UTF-8

export CONTAINER_NAME=ci_native_asan
export PACKAGES="clang-19 llvm-19 libclang-rt-19-dev python3-zmq qtbase5-dev qttools5-dev-tools libevent-dev bsdmainutils libboost-dev libminiupnpc-dev libzmq3-dev libqrencode-dev"
# Reuses the depends built for the linux64 target, which uses the defaults.
export DEP_OPTS=""
export TEST_RUNNER_EXTRA="--timeout-factor=4 -j4"  # Increase timeout because sanitizers slow down
export CI_LIMIT_STACK_SIZE=1
export GOAL="install"
export BITCOIN_CONFIG="--enable-zmq --enable-crash-hooks --with-gui=qt5 --without-bdb --with-sqlite \
--with-sanitizers=address,float-divide-by-zero,integer,undefined \
CPPFLAGS='-DARENA_DEBUG -DDEBUG_LOCKORDER' \
CC='clang-19 -ftrivial-auto-var-init=pattern' CXX='clang++-19 -ftrivial-auto-var-init=pattern'"
export PYZMQ=true
