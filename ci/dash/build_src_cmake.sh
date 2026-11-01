#!/usr/bin/env bash
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
#
# This script is executed inside the builder image
#
# Counterpart to build_src.sh for the CMake build system. It consumes the same
# depends prefix, by way of the toolchain file that `make -C depends` generates.

export LC_ALL=C.UTF-8

set -e

source ./ci/dash/matrix.sh

unset CC CXX DISPLAY;

ccache --zero-stats

CMAKE_BUILD_DIR="${BASE_ROOT_DIR}/build-cmake"
TOOLCHAIN_FILE="${DEPENDS_DIR}/${HOST}/toolchain.cmake"

if [ ! -f "${TOOLCHAIN_FILE}" ]; then
  echo "No toolchain file at ${TOOLCHAIN_FILE}; run 'make -C depends' first." >&2
  exit 1
fi

BITCOIN_CONFIG_ALL="-DENABLE_EXTERNAL_SIGNER=ON -DCMAKE_INSTALL_PREFIX=${BASE_OUTDIR}"
if [ -z "$NO_WERROR" ]; then
  BITCOIN_CONFIG_ALL="${BITCOIN_CONFIG_ALL} -DWERROR=ON"
fi

rm -rf "${CMAKE_BUILD_DIR}"

bash -c "cmake -B ${CMAKE_BUILD_DIR} -S ${BASE_ROOT_DIR} \
  --toolchain ${TOOLCHAIN_FILE} \
  ${BITCOIN_CONFIG_ALL} ${BITCOIN_CONFIG}"

bash -c "cmake --build ${CMAKE_BUILD_DIR} ${MAKEJOBS}" || \
  ( echo "Build failure. Verbose build follows." && \
    cmake --build "${CMAKE_BUILD_DIR}" --verbose ; false )

if [ "${GOAL}" = "install" ]; then
  cmake --install "${CMAKE_BUILD_DIR}"
fi

ccache --version | head -n 1 && ccache --show-stats
