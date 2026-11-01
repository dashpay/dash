#!/usr/bin/env bash
#
# Copyright (c) 2019-2022 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C.UTF-8

# The root dir.
# The ci system copies this folder.
BASE_ROOT_DIR=$( cd "$( dirname "${BASH_SOURCE[0]}" )"/../../ >/dev/null 2>&1 && pwd )
export BASE_ROOT_DIR

echo "Setting specific values in env"
if [ -n "${FILE_ENV}" ]; then
  set -o errexit;
  # shellcheck disable=SC1090
  source "${FILE_ENV}"
fi

export BUILD_TARGET=${BUILD_TARGET:-linux64}
export PULL_REQUEST=${PULL_REQUEST:-false}
export JOB_NUMBER=${JOB_NUMBER:-1}

echo "Fallback to default values in env (if not yet set)"
# The number of parallel jobs to pass down to make and test_runner.py
export MAKEJOBS=${MAKEJOBS:--j$(nproc)}
# A folder for the ci system to put temporary files (build result, datadirs for tests, ...)
# This folder only exists on the ci guest.
export BASE_SCRATCH_DIR=${BASE_SCRATCH_DIR:-$BASE_ROOT_DIR/ci/scratch}
# A folder for the ci system to put executables.
# This folder only exists on the ci guest.
export BINS_SCRATCH_DIR="${BASE_SCRATCH_DIR}/bins/"
# What host to compile for. See also ./depends/README.md
# Tests that need cross-compilation export the appropriate HOST.
# Tests that run natively guess the host
export HOST=${HOST:-$("$BASE_ROOT_DIR/depends/config.guess")}
# Whether to prefer BusyBox over GNU utilities
export USE_BUSY_BOX=${USE_BUSY_BOX:-false}

export RUN_UNIT_TESTS=${RUN_UNIT_TESTS:-true}
export RUN_FUNCTIONAL_TESTS=${RUN_FUNCTIONAL_TESTS:-true}
export RUN_TIDY=${RUN_TIDY:-false}
# Whether to check the built executables for embedded standard library
# header paths. Off by default: sanitizer and fuzz builds record source
# locations for their own diagnostics, so they are expected to name
# standard library headers legitimately.
export RUN_STDLIB_PATH_CHECK=${RUN_STDLIB_PATH_CHECK:-false}
# By how much to scale the test_runner timeouts (option --timeout-factor).
# This is needed because some ci machines have slow CPU or disk, so sanitizers
# might be slow or a reindex might be waiting on disk IO.
export TEST_RUNNER_TIMEOUT_FACTOR=${TEST_RUNNER_TIMEOUT_FACTOR:-4}
export RUN_FUZZ_TESTS=${RUN_FUZZ_TESTS:-false}

export CONTAINER_NAME=${CONTAINER_NAME:-ci_unnamed}
# Randomize test order.
# See https://www.boost.org/doc/libs/1_71_0/libs/test/doc/html/boost_test/utf_reference/rt_param_reference/random.html
export BOOST_TEST_RANDOM=${BOOST_TEST_RANDOM:-1}
# See man 7 debconf
export DEBIAN_FRONTEND=noninteractive
export HOST_CACHE_DIR=${HOST_CACHE_DIR:-$BASE_ROOT_DIR/ci-cache-$BUILD_TARGET}
export CACHE_DIR=${CACHE_DIR:-$HOST_CACHE_DIR}
export CCACHE_MAXSIZE=${CCACHE_MAXSIZE:-100M}
export CCACHE_TEMPDIR=${CCACHE_TEMPDIR:-/tmp/.ccache-temp}
export CCACHE_COMPRESS=${CCACHE_COMPRESS:-1}
export CCACHE_COMPRESSLEVEL=${CCACHE_COMPRESSLEVEL:-9}
# The cache dir.
# This folder exists only on the ci guest, and on the ci host as a volume.
export CCACHE_DIR=${CCACHE_DIR:-$CACHE_DIR/ccache}
# ctcache (clang-tidy cache) configuration
export CTCACHE_DIR=${CTCACHE_DIR:-$CACHE_DIR/ctcache}
export CTCACHE_MAXSIZE_MB=${CTCACHE_MAXSIZE_MB:-50}
# The depends dir.
# This folder exists only on the ci guest, and on the ci host as a volume.
export DEPENDS_DIR=${DEPENDS_DIR:-$BASE_ROOT_DIR/depends}
# Folder where the build result is put (bin and lib).
export BASE_OUTDIR=${BASE_OUTDIR:-$BASE_SCRATCH_DIR/out/$HOST}
# Folder where the build is done (dist and out-of-tree build).
export BASE_BUILD_DIR=${BASE_BUILD_DIR:-$BASE_SCRATCH_DIR/build-ci}
# The folder for previous release binaries.
# This folder exists only on the ci guest, and on the ci host as a volume.
export PREVIOUS_RELEASES_DIR=${PREVIOUS_RELEASES_DIR:-$BASE_ROOT_DIR/releases/$HOST}
export DOCKER_PACKAGES=${DOCKER_PACKAGES:-build-essential libtool autotools-dev automake pkg-config bsdmainutils curl ca-certificates ccache python3 rsync git procps}
export GOAL=${GOAL:-install}
export DIR_QA_ASSETS=${DIR_QA_ASSETS:-${BASE_SCRATCH_DIR}/qa-assets}
export CI_RETRY_EXE=${CI_RETRY_EXE:-"retry --"}
# Dash's Docker-specifics
export BUILDER_IMAGE_NAME="dash-builder-$BUILD_TARGET-$JOB_NUMBER"
export DOCKER_RUN_VOLUME_ARGS="-v $BASE_ROOT_DIR:$BASE_ROOT_DIR -v $HOST_CACHE_DIR:$CACHE_DIR"
export DOCKER_RUN_ENV_ARGS="-e BASE_ROOT_DIR=$BASE_ROOT_DIR -e CACHE_DIR=$CACHE_DIR -e PULL_REQUEST=$PULL_REQUEST -e COMMIT_RANGE=$COMMIT_RANGE -e JOB_NUMBER=$JOB_NUMBER -e BUILD_TARGET=$BUILD_TARGET"
export DOCKER_RUN_ARGS="$DOCKER_RUN_VOLUME_ARGS $DOCKER_RUN_ENV_ARGS"
export DOCKER_RUN_IN_BUILDER="docker run -t --rm -w $BASE_ROOT_DIR $DOCKER_RUN_ARGS $BUILDER_IMAGE_NAME"
