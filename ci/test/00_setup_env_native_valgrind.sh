#!/usr/bin/env bash
#
# Copyright (c) 2019-2022 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C.UTF-8

export PACKAGES="valgrind clang llvm libclang-rt-dev python3-zmq libevent-dev libboost-dev libdb5.3++-dev libminiupnpc-dev libzmq3-dev"
export USE_VALGRIND=1
export NO_DEPENDS=1
export TEST_RUNNER_EXTRA="--exclude feature_init,rpc_bind,feature_bind_extra --timeout-factor=4"  # Excluded for now, see https://github.com/bitcoin/bitcoin/issues/17765#issuecomment-602068547
export GOAL="install"
# Temporarily pin dwarf 4, until valgrind can understand clang's dwarf 5
export BITCOIN_CONFIG="\
 -DWITH_ZMQ=ON -DWITH_BDB=ON -DWARN_INCOMPATIBLE_BDB=OFF -DBUILD_GUI=OFF \
 -DCMAKE_C_COMPILER=clang-19 \
 -DCMAKE_CXX_COMPILER=clang++-19 \
 -DCMAKE_C_FLAGS='-gdwarf-4' \
 -DCMAKE_CXX_FLAGS='-gdwarf-4' \
"  # TODO enable GUI
