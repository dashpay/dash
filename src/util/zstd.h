// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_UTIL_ZSTD_H
#define BITCOIN_UTIL_ZSTD_H

#include <serialize.h>
#include <span.h>

#include <cstddef>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

class AutoFile;
struct ZSTD_DCtx_s;

namespace util {

static constexpr int DEFAULT_ZSTD_LEVEL{0};

/** Whether this build links libzstd. Without it only level 0 is accepted and compressed data cannot be read. */
bool ZstdAvailable();

/** DEFAULT_ZSTD_LEVEL, or 0 in builds without zstd. */
int DefaultZstdLevel();

/** Highest compression level accepted by the linked libzstd, 0 without zstd. */
int ZstdMaxLevel();

/** Compress `in` into a single zstd frame. Returns an empty vector on library error. */
std::vector<std::byte> ZstdCompress(Span<const std::byte> in, int level);

/**
 * Decompress the zstd frame at the start of `in`, whether or not its header records the content size.
 * Returns nullopt for a malformed or truncated frame, or one that decompresses to more than max_out bytes.
 */
std::optional<std::vector<std::byte>> ZstdDecompress(Span<const std::byte> in, size_t max_out);

/**
 * Reads a record of size bytes from the current position of a file. A compressed record is one
 * zstd frame, decompressed chunk by chunk while the caller reads, so neither the frame nor its
 * contents are ever held in memory as a whole; a plain record is passed through. Throws
 * std::ios_base::failure on a truncated or corrupt frame, when reading past its end, when a
 * compressed record decompresses to more than max_out bytes, and for a compressed record in builds
 * without zstd support.
 */
class ZstdFileReader
{
    AutoFile& m_file;
    const bool m_compressed;
    size_t m_remaining; //!< bytes of the record not read from the file yet
    [[maybe_unused]] const size_t m_max_out; //!< most bytes a compressed record may decompress to
    [[maybe_unused]] size_t m_decompressed{0};
    std::unique_ptr<ZSTD_DCtx_s, void (*)(ZSTD_DCtx_s*)> m_stream;
    std::vector<std::byte> m_in;
    std::vector<std::byte> m_out;
    Span<const std::byte> m_pending; //!< compressed bytes read from the file, not consumed by zstd yet
    Span<const std::byte> m_unread;  //!< decompressed bytes not returned to the caller yet
    [[maybe_unused]] bool m_frame_done{false};

    void ReadPlain(Span<std::byte> dst);

public:
    ZstdFileReader(AutoFile& file, size_t size, bool compressed, size_t max_out = std::numeric_limits<size_t>::max());
    void read(Span<std::byte> dst);
    void ignore(size_t n);

    template <typename T>
    ZstdFileReader& operator>>(T&& obj)
    {
        ::Unserialize(*this, obj);
        return *this;
    }
};

} // namespace util

#endif // BITCOIN_UTIL_ZSTD_H
