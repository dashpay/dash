#!/usr/bin/env bash
# Copyright (c) 2021-2025 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
#
# This script is executed inside the builder image

export LC_ALL=C.UTF-8

set -e

source ./ci/dash/matrix.sh

unset CC CXX DISPLAY;

ccache --zero-stats

BUILD_DIR="build-ci/dashcore-${BUILD_TARGET}"

BITCOIN_CONFIG_ALL="-DBUILD_BENCH=ON -DBUILD_FUZZ_BINARY=ON -DENABLE_EXTERNAL_SIGNER=ON"
if [ -z "$NO_DEPENDS" ]; then
  BITCOIN_CONFIG_ALL="${BITCOIN_CONFIG_ALL} -DCMAKE_TOOLCHAIN_FILE=${DEPENDS_DIR}/${HOST}/toolchain.cmake"
fi
BITCOIN_CONFIG_ALL="${BITCOIN_CONFIG_ALL} -DCMAKE_INSTALL_PREFIX=${BASE_OUTDIR}"
if [ -z "$NO_WERROR" ]; then
  BITCOIN_CONFIG_ALL="${BITCOIN_CONFIG_ALL} -DWERROR=ON"
fi
if [ "${RUN_TIDY}" = "true" ]; then
  BITCOIN_CONFIG_ALL="${BITCOIN_CONFIG_ALL} -DCMAKE_EXPORT_COMPILE_COMMANDS=ON"
fi

rm -rf build-ci
mkdir -p "${BUILD_DIR}"

bash -c "cmake -S . -B ${BUILD_DIR} $BITCOIN_CONFIG_ALL $BITCOIN_CONFIG" || ( cat "${BUILD_DIR}/CMakeFiles/CMakeConfigureLog.yaml" && false )

bash -c "cmake --build ${BUILD_DIR} ${MAKEJOBS} --target all ${GOAL}" || ( echo "Build failure. Verbose build follows." && cmake --build "${BUILD_DIR}" --target all ${GOAL} --verbose ; false )

ccache --version | head -n 1 && ccache --show-stats

if [ "${RUN_STDLIB_PATH_CHECK}" = "true" ]; then
  cmake --build "${BUILD_DIR}" --target check-stdlib-paths
fi

if [ "${RUN_CHECK_NO_RUST}" = "true" ]; then
  ( cd "${BUILD_DIR}" && "${BASE_ROOT_DIR}/contrib/devtools/check-no-rust.py" src/dashd src/dash-cli src/dash-tx src/dash-util src/dash-wallet src/bench/bench_dash src/test/fuzz/fuzz )
fi

if [ -n "$USE_VALGRIND" ]; then
    echo "valgrind in USE!"
    "${BASE_ROOT_DIR}/ci/test/wrap-valgrind.sh"
fi

# GitHub Actions can segment a job into steps, linting is a separate step
# so Actions runners will perform this step separately.
if [ "${RUN_TIDY}" = "true" ] && [ "${GITHUB_ACTIONS}" != "true" ]; then
  "${BASE_ROOT_DIR}/ci/dash/lint-tidy.sh"
fi
