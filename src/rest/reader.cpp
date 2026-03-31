// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <rest/reader.h>

#include <flatfile.h>
#include <fs.h>
#include <node/blockstorage.h>

#include <cstdint>
#include <cstdio>
#include <memory>

namespace rest {
std::optional<std::string> ReadRawBlockFromDisk(const FlatFilePos& pos)
{
    if (pos.IsNull()) {
        return std::nullopt;
    }

    // The size field is stored in the 4 bytes preceding pos.nPos.
    if (pos.nPos < sizeof(uint32_t)) {
        return std::nullopt;
    }

    const auto path{node::GetBlockPosFilename(pos)};
    std::unique_ptr<FILE, decltype(&std::fclose)> file{fsbridge::fopen(path, "rb"), &std::fclose};
    if (!file) {
        return std::nullopt;
    }

    // Read the 4-byte block size just before pos.nPos.
    if (std::fseek(file.get(), pos.nPos - sizeof(uint32_t), SEEK_SET) != 0) {
        return std::nullopt;
    }

    uint32_t block_size{0};
    if (std::fread(&block_size, sizeof(block_size), 1, file.get()) != 1) {
        return std::nullopt;
    }
    if (block_size == 0 || block_size > node::MAX_BLOCKFILE_SIZE) {
        return std::nullopt;
    }

    // Read block data directly into a std::string (file cursor is already at pos.nPos).
    std::string data(block_size, '\0');
    if (std::fread(data.data(), 1, block_size, file.get()) != block_size) {
        return std::nullopt;
    }

    return data;
}
} // namespace rest
