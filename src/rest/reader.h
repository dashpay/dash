// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_REST_READER_H
#define BITCOIN_REST_READER_H

#include <flatfile.h>

#include <optional>
#include <string>

namespace rest {
/**
 * Read raw serialized block bytes directly from disk without CBlock
 * deserialization. The returned bytes are the network-serialized block
 * suitable for sending as application/octet-stream.
 *
 * @param[in]  pos  The on-disk position of the block (from CBlockIndex::GetBlockPos()).
 * @return     The raw block bytes as a std::string, or std::nullopt on read failure.
 */
std::optional<std::string> ReadRawBlockFromDisk(const FlatFilePos& pos);
} // namespace rest

#endif // BITCOIN_REST_READER_H
