#!/usr/bin/env bash
#
# Copyright (c) 2019-2022 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C.UTF-8

export CONTAINER_NAME=ci_native_fuzz
export PACKAGES="clang llvm libevent-dev libboost-dev"
export DEP_OPTS="NO_UPNP=1 DEBUG=1"
export PYZMQ=true
export RUN_UNIT_TESTS=false
export RUN_FUNCTIONAL_TESTS=false
export RUN_FUZZ_TESTS=true
export GOAL="install"
# depends' toolchain file applies its DEBUG=1 defines to Debug builds only, so
# repeat the libstdc++ debug mode here to keep it on in this build.
export BITCOIN_CONFIG="\
 -DBUILD_FOR_FUZZING=ON -DWITH_ZMQ=ON \
 -DSANITIZERS=fuzzer,address,undefined,float-divide-by-zero,integer \
 -DCMAKE_C_COMPILER=clang-19 \
 -DCMAKE_CXX_COMPILER=clang++-19 \
 -DAPPEND_CFLAGS='-ftrivial-auto-var-init=pattern' \
 -DAPPEND_CXXFLAGS='-ftrivial-auto-var-init=pattern' \
 -DAPPEND_CPPFLAGS='-DDEBUG_LOCKORDER -DARENA_DEBUG -D_GLIBCXX_DEBUG -D_GLIBCXX_DEBUG_PEDANTIC' \
"
