#!/usr/bin/env bash
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
#
# This script is executed inside the builder image

export LC_ALL=C.UTF-8

set -e

source ./ci/dash/matrix.sh

if [ "$RUN_UNIT_TESTS" != "true" ]; then
  echo "Skipping unit tests"
  exit 0
fi

export BOOST_TEST_RANDOM="${BOOST_TEST_RANDOM:-1}"
export LD_LIBRARY_PATH="$DEPENDS_DIR/$HOST/lib"
export BOOST_TEST_LOG_LEVEL=test_suite
# The Qt tests need a display; the minimal platform stands in for one.
export QT_QPA_PLATFORM=minimal

ctest --test-dir "${BASE_ROOT_DIR}/build-cmake" "${MAKEJOBS}" --output-on-failure
