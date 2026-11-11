#!/usr/bin/env bash
#
# Copyright (c) 2020-present The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C.UTF-8

export CONTAINER_NAME="ci_native_msan"
export HOST=x86_64-pc-linux-gnu
# Built into the CI image by contrib/containers/ci/ci-msan.Dockerfile so that it
# lands in a cached layer instead of being rebuilt on every run. Upstream does
# the equivalent in ci/test/01_base_install.sh, which Dash does not have.
LIBCXX_DIR="/cxx_build/"
export MSAN_FLAGS="-fsanitize=memory -fsanitize-memory-track-origins=2 -fno-omit-frame-pointer -g -O1 -fno-optimize-sibling-calls"
LIBCXX_FLAGS="-nostdinc++ -nostdlib++ -isystem ${LIBCXX_DIR}include/c++/v1 -L${LIBCXX_DIR}lib -Wl,-rpath,${LIBCXX_DIR}lib -lc++ -lc++abi -lpthread -Wno-unused-command-line-argument"
export MSAN_AND_LIBCXX_FLAGS="${MSAN_FLAGS} ${LIBCXX_FLAGS}"

# BDB generates false-positives and will be removed in future
# __BLST_NO_ASM__ makes blst use its C implementation instead of hand-written
# assembly, which MSan cannot see through.
export DEP_OPTS="DEBUG=1 NO_BDB=1 NO_QT=1 NO_UPNP=1 NO_NATPMP=1 CC=clang-19 CXX=clang++-19 CFLAGS='${MSAN_FLAGS}' CXXFLAGS='${MSAN_AND_LIBCXX_FLAGS}' CPPFLAGS='-D__BLST_NO_ASM__'"
export GOAL="install"
# The compilers and flags come from DEP_OPTS through depends' toolchain file.
# _FORTIFY_SOURCE is not compatible with MSAN.
# depends' toolchain file applies its DEBUG=1 defines to Debug builds only, so
# repeat the libc++ debug hardening mode here to keep it on in this build.
# SECP256K1_ASM=OFF keeps secp256k1 off hand-written assembly, which MSan
# cannot see through.
# ENABLE_MIMALLOC=OFF selects the plain malloc secure allocator; mimalloc seeds
# itself with a raw getrandom syscall before main(), which MSan cannot track.
export BITCOIN_CONFIG="\
 -DSANITIZERS=memory -DBUILD_GUI=OFF -DWITH_BDB=OFF -DWITH_SQLITE=ON \
 -DSECP256K1_ASM=OFF -DENABLE_MIMALLOC=OFF \
 -DAPPEND_CPPFLAGS='-U_FORTIFY_SOURCE -D_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_DEBUG' \
"
export USE_MEMORY_SANITIZER="true"
export TEST_RUNNER_TIMEOUT_FACTOR=40
