#!/usr/bin/env bash
#
# Copyright (c) 2019-2022 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C.UTF-8

export CONTAINER_NAME=ci_native_fuzz_valgrind
export PACKAGES="clang llvm libclang-rt-dev libevent-dev libboost-dev valgrind"
export NO_DEPENDS=1
export RUN_UNIT_TESTS=false
export RUN_FUNCTIONAL_TESTS=false
export RUN_FUZZ_TESTS=true
export FUZZ_TESTS_CONFIG="--valgrind"
export GOAL="install"
# Temporarily pin dwarf 4, until valgrind can understand clang's dwarf 5
export BITCOIN_CONFIG="\
 -DBUILD_FOR_FUZZING=ON \
 -DSANITIZERS=fuzzer \
 -DCMAKE_C_COMPILER=clang-19 \
 -DCMAKE_CXX_COMPILER=clang++-19 \
 -DCMAKE_C_FLAGS='-gdwarf-4' \
 -DCMAKE_CXX_FLAGS='-gdwarf-4' \
"
export CCACHE_MAXSIZE=200M
