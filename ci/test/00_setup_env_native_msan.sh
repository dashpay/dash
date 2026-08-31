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
export DEP_OPTS="DEBUG=1 NO_BDB=1 NO_QT=1 NO_UPNP=1 NO_NATPMP=1 CC=clang-19 CXX=clang++-19 CFLAGS='${MSAN_FLAGS}' CXXFLAGS='${MSAN_AND_LIBCXX_FLAGS}'"
export GOAL="install"
# CC/CXX/CFLAGS/CXXFLAGS are repeated here rather than left to depends'
# config.site. Upstream dropped them in bitcoin#29800 because its
# ci/test/03_test_script.sh sets CONFIG_SITE explicitly (bitcoin#26683);
# ci/dash/build_src.sh still uses the older --prefix form, and this is the only
# target whose compiler differs from the system default, so nothing else in
# Dash's CI exercises that hand-off.
# _FORTIFY_SOURCE is not compatible with MSAN.
# --with-asm=no and --with-backend=easy keep secp256k1 and relic off
# hand-written assembly, which MSan cannot see through.
# --disable-mimalloc selects the plain malloc secure allocator; mimalloc seeds
# itself with a raw getrandom syscall before main(), which MSan cannot track.
export BITCOIN_CONFIG="--with-sanitizers=memory --with-gui=no --without-bdb --with-sqlite \
--with-asm=no --with-backend=easy --disable-mimalloc \
CC=clang-19 CXX=clang++-19 CFLAGS='${MSAN_FLAGS}' CXXFLAGS='${MSAN_AND_LIBCXX_FLAGS}' \
CPPFLAGS='-U_FORTIFY_SOURCE'"
export USE_MEMORY_SANITIZER="true"
export TEST_RUNNER_TIMEOUT_FACTOR=40
