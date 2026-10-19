// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_UTIL_ZSTD_H
#define BITCOIN_UTIL_ZSTD_H

#include <span.h>

#include <cstddef>
#include <optional>
#include <vector>

namespace util {

/** Whether this build links libzstd. Without it nothing is compressed and compressed data cannot be read. */
bool ZstdAvailable();

/** Highest compression level accepted by the linked libzstd, 0 without zstd. */
int ZstdMaxLevel();

/** Compress `in` into a single zstd frame. Returns an empty vector on library error. */
std::vector<std::byte> ZstdCompress(Span<const std::byte> in, int level);

/**
 * Decompress the zstd frame at the start of `in`, whether or not its header records the content size.
 * Returns nullopt for a malformed or truncated frame, or one that decompresses to more than max_out bytes.
 */
std::optional<std::vector<std::byte>> ZstdDecompress(Span<const std::byte> in, size_t max_out);

} // namespace util

#endif // BITCOIN_UTIL_ZSTD_H
