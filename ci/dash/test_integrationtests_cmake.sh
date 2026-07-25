#!/usr/bin/env bash
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
#
# This script is executed inside the builder image
#
# A functional smoke test for the CMake build. The Autotools jobs remain
# responsible for the full functional suite; this only proves that the binaries
# CMake produced actually start, mine and talk RPC. The mining tests matter in
# particular, because a missing ENABLE_MINER definition turns every mining RPC
# into a stub that unit tests would never notice.

export LC_ALL=C.UTF-8

set -e

source ./ci/dash/matrix.sh

if [ "$RUN_FUNCTIONAL_TESTS" != "true" ]; then
  echo "Skipping integration tests"
  exit 0
fi

SMOKE_TESTS=(
  mining_basic.py
  rpc_blockchain.py
  wallet_basic.py
)

export LD_LIBRARY_PATH="$DEPENDS_DIR/$HOST/lib"

cd "${BASE_ROOT_DIR}/build-cmake"

# --combinedlogslen dumps the failing node's logs into the job output, which is
# what stands in for the log bundle the Autotools jobs upload.
./test/functional/test_runner.py \
  --ci --ansi --combinedlogslen=4000 --failfast --attempts=3 \
  --timeout-factor="${TEST_RUNNER_TIMEOUT_FACTOR}" \
  --tmpdir="$(pwd)/testdatadirs" \
  "${SMOKE_TESTS[@]}"
