#!/usr/bin/env bash
# Copyright (c) 2025 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

export LC_ALL=C.UTF-8

set -eo pipefail

# Warning: This script does not generate the compilation database these linters rely
#          only on nor do they set the requisite build parameters. Make sure you do
#          that *before* running this script.

# Verify clang-tidy is available
CLANG_TIDY_BIN=$(command -v clang-tidy)
if [ -z "${CLANG_TIDY_BIN}" ]; then
  echo "Error: clang-tidy not found in PATH"
  exit 1
fi

# Setup ctcache for clang-tidy result caching
export CTCACHE_SAVE_OUTPUT=1
# The cache key is the preprocessed source: keep comments so that a NOLINT edit invalidates it
export CTCACHE_KEEP_COMMENTS=1
export CTCACHE_CLANG_TIDY="${CLANG_TIDY_BIN}"
mkdir -p "${CTCACHE_DIR}"

CLANG_TIDY_CACHE=$(command -v clang-tidy-cache)

# Verify ctcache is installed
if [ -z "${CLANG_TIDY_CACHE}" ]; then
  echo "Error: clang-tidy-cache not found in PATH"
  exit 1
fi

# Derive Python script path from wrapper location (matches wrapper's own logic)
CLANG_TIDY_CACHE_PY="$(dirname "${CLANG_TIDY_CACHE}")/src/ctcache/clang_tidy_cache.py"
if [ ! -f "${CLANG_TIDY_CACHE_PY}" ]; then
  echo "Error: ctcache Python script not found at ${CLANG_TIDY_CACHE_PY}"
  exit 1
fi

# Zero stats before run to get accurate statistics for this run only
python3 "${CLANG_TIDY_CACHE_PY}" --zero-stats 2>&1 || true

TIDY_BUILD_DIR="${BASE_ROOT_DIR}/build-ci/tidy-build"
cmake -B "${TIDY_BUILD_DIR}" -DLLVM_DIR="$(llvm-config --cmakedir)" -DCMAKE_BUILD_TYPE=Release -S "${BASE_ROOT_DIR}/contrib/devtools/bitcoin-tidy"
cmake --build "${TIDY_BUILD_DIR}" "${MAKEJOBS}"
cmake --build "${TIDY_BUILD_DIR}" --target bitcoin-tidy-tests "${MAKEJOBS}"

cd "${BASE_ROOT_DIR}/build-ci/dashcore-${BUILD_TARGET}/src"

# Filter out:
# * generated Qt sources (moc, qrc and uic output under the *_autogen directories)
# * subtrees and other vendored code
jq 'map(select(.file | test("_autogen/|src/(secp256k1|crc32c|crypto/x11|dashbls|gsl|immer|leveldb|minisketch|univalue)/|src/bench/nanobench\\.cpp$") | not))' ../compile_commands.json > tmp.json
mv tmp.json ../compile_commands.json

CAST_LINT_DB="${PWD}/../cstyle-cast-compile-db"
python3 "${BASE_ROOT_DIR}/ci/dash/lint-cstyle-casts.py" prepare \
  --input "${PWD}/../compile_commands.json" \
  --output-dir "${CAST_LINT_DB}" \
  --source-root "${BASE_ROOT_DIR}"

if ! ( run-clang-tidy \
  -checks=clang-diagnostic-old-style-cast,google-readability-casting \
  -warnings-as-errors=-clang-diagnostic-old-style-cast,-google-readability-casting \
  -clang-tidy-binary="${CLANG_TIDY_CACHE}" \
  -load="${TIDY_BUILD_DIR}/libbitcoin-tidy.so" \
  -p "${CAST_LINT_DB}" \
  -quiet "${MAKEJOBS}" | \
  python3 "${BASE_ROOT_DIR}/ci/dash/lint-cstyle-casts.py" filter --source-root "${BASE_ROOT_DIR}" | \
  tee tmp.tidy-out.txt ); then
  grep -E -C5 "error: |warning: use of old-style cast|google-readability-casting" tmp.tidy-out.txt
  echo "^^^ ⚠️ Failure generated from clang-tidy"
  false
fi

# Show ctcache statistics and manage cache size
echo "=== ctcache statistics ==="
du -sh "${CTCACHE_DIR}" 2>/dev/null || echo "Cache directory not found"
python3 "${CLANG_TIDY_CACHE_PY}" --show-stats 2>&1 || true

# Limit cache size (ctcache has no built-in size management)
python3 "${BASE_ROOT_DIR}/ci/dash/cleanup-ctcache.py" "${CTCACHE_DIR}" "${CTCACHE_MAXSIZE_MB}"
echo "=========================="

cd "${BASE_ROOT_DIR}/build-ci/dashcore-${BUILD_TARGET}"
if ! iwyu_tool.py \
  -p . "${MAKEJOBS}" \
  -- -Xiwyu --cxx17ns -Xiwyu --mapping_file="${BASE_ROOT_DIR}/contrib/devtools/iwyu/bitcoin.core.imp" \
  -Xiwyu --max_line_length=160 \
  > "/tmp/iwyu_ci.out" 2>&1; then
  grep -E -C5 "error: |Traceback" /tmp/iwyu_ci.out || tail -n 50 /tmp/iwyu_ci.out
  echo "^^^ ⚠️ Failure generated from IWYU"
  false
fi

# Fix the tracked sources rather than the untracked build copy, so that git diff shows the result.
# The generated capnp files exist only in the build directory.
cd "${BASE_ROOT_DIR}/src"
fix_includes.py --nosafe_headers --ignore_re='\.capnp\.' < /tmp/iwyu_ci.out
git --no-pager diff
