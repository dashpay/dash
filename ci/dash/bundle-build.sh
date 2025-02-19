#!/usr/bin/env bash
# Copyright (c) 2024-2025 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C.UTF-8

set -eo pipefail

SH_NAME="$(basename "${0}")"

if [ -z "${BUILD_TARGET}" ]; then
  echo "${SH_NAME}: BUILD_TARGET not defined, cannot continue!";
  exit 1;
elif [ -z "${ARTIFACT_PATH}" ]; then
  echo "${SH_NAME}: ARTIFACT_PATH not defined, cannot continue!";
  exit 1;
elif  [ ! "$(command -v zstd)" ]; then
  echo "${SH_NAME}: zstd not found, cannot continue!";
  exit 1;
fi

ARTIFACT_ARCHIVE="artifacts-${BUILD_TARGET}.tar.zst"
if [ -f "${ARTIFACT_ARCHIVE}" ]; then
  echo "${SH_NAME}: ${ARTIFACT_ARCHIVE} already exists, cannot continue!";
  exit 1;
fi

FILE_EXTENSION=""
if [[ "${BUILD_TARGET}" == "win"* ]]; then
  FILE_EXTENSION=".exe"
fi

# The extra comma is so that brace expansion works as expected, otherwise
# it's just interpreted as literal braces
ARTIFACT_DIRS=",bin"
if [[ "${BUILD_TARGET}" == "mac"* ]]; then
  ARTIFACT_DIRS="${ARTIFACT_DIRS},dist"
fi

# If there are new binaries, these need to be updated
BIN_NAMES="d,-cli,-tx"
if [[ ${BUILD_TARGET} != *"nowallet" ]]; then
  BIN_NAMES="${BIN_NAMES},-wallet"
fi
if [[ ${BUILD_TARGET} == *"multiprocess" ]]; then
  BIN_NAMES="${BIN_NAMES},-node"
fi

# Since brace expansion happens _before_ variable substitution, we need to
# do the variable substitution in this bash instance, then use a fresh bash
# instance to do the brace expansion, then take the word split output.
#
# This needs us to suppress SC2046.

ARTIFACT_BASE_PATH="${ARTIFACT_PATH}/${BUILD_TARGET}"
# shellcheck disable=SC2046
mkdir -p $(echo -n $(bash -c "echo ${ARTIFACT_BASE_PATH}/{${ARTIFACT_DIRS}}"));

# We aren't taking binaries from "release" as they're stripped and therefore,
# impede debugging.
BUILD_BASE_PATH="build-ci/dashcore-${BUILD_TARGET}/src"

if [ -f "${BUILD_BASE_PATH}/dashd${FILE_EXTENSION}" ]; then
  # shellcheck disable=SC2046
  cp $(echo -n $(bash -c "echo ${BUILD_BASE_PATH}/dash{${BIN_NAMES}}${FILE_EXTENSION}")) \
   "${BUILD_BASE_PATH}/bench/bench_dash${FILE_EXTENSION}" \
   "${BUILD_BASE_PATH}/test/test_dash${FILE_EXTENSION}" \
   "${ARTIFACT_BASE_PATH}/bin";
fi

if [ -f "${BUILD_BASE_PATH}/qt/dash-qt${FILE_EXTENSION}" ]; then
  cp "${BUILD_BASE_PATH}/qt/dash-qt${FILE_EXTENSION}" \
   "${BUILD_BASE_PATH}/qt/test/test_dash-qt${FILE_EXTENSION}" \
   "${ARTIFACT_BASE_PATH}/bin";
fi

if [ -f "${BUILD_BASE_PATH}/test/fuzz/fuzz${FILE_EXTENSION}" ]; then
  cp "${BUILD_BASE_PATH}/test/fuzz/fuzz${FILE_EXTENSION}" \
   "${ARTIFACT_BASE_PATH}/bin";
fi

if [[ "${ARTIFACT_DIRS}" == *"dist"* ]] && [[ "${BUILD_TARGET}" == "mac"* ]]; then
  cp "${BUILD_BASE_PATH}/../Dash-Core.zip" \
   "${ARTIFACT_BASE_PATH}/dist";
fi

# We have to cd into ARTIFACT_PATH so that the archive doesn't have the
# directory structure ARTIFACT_PATH/BUILD_TARGET when we want BUILD_TARGET
# as the root directory. `tar` offers no easy way to do this.
cd "${ARTIFACT_PATH}";
tar --use-compress-program="zstd -T0 -5" -cvf "${ARTIFACT_ARCHIVE}" "${BUILD_TARGET}";
sha256sum "${ARTIFACT_ARCHIVE}" > "${ARTIFACT_ARCHIVE}.sha256";
