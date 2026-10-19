// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <util/zstd.h>

#ifdef ENABLE_ZSTD
#include <zstd.h>
#endif

namespace util {

bool ZstdAvailable()
{
#ifdef ENABLE_ZSTD
    return true;
#else
    return false;
#endif
}

#ifdef ENABLE_ZSTD
int ZstdMaxLevel()
{
    return ZSTD_maxCLevel();
}

std::vector<std::byte> ZstdCompress(Span<const std::byte> in, int level)
{
    std::vector<std::byte> out(ZSTD_compressBound(in.size()));
    const size_t written{ZSTD_compress(out.data(), out.size(), in.data(), in.size(), level)};
    if (ZSTD_isError(written)) return {};
    out.resize(written);
    return out;
}

std::optional<std::vector<std::byte>> ZstdDecompress(Span<const std::byte> in, size_t max_out)
{
    const size_t frame_size{ZSTD_findFrameCompressedSize(in.data(), in.size())};
    if (ZSTD_isError(frame_size)) return std::nullopt;
    // zstd.h spells these sentinels as 0ULL - 1 and 0ULL - 2, which the unsigned-overflow sanitizer flags unless they are constant-folded
    constexpr unsigned long long content_size_unknown{ZSTD_CONTENTSIZE_UNKNOWN};
    constexpr unsigned long long content_size_error{ZSTD_CONTENTSIZE_ERROR};
    const unsigned long long content_size{ZSTD_getFrameContentSize(in.data(), frame_size)};
    if (content_size == content_size_error || (content_size != content_size_unknown && content_size > max_out)) {
        return std::nullopt;
    }
    // one-shot decompression keeps its history in the output buffer, so memory stays bounded by max_out whatever window the frame declares
    std::vector<std::byte> out(content_size == content_size_unknown ? max_out : content_size);
    const size_t written{ZSTD_decompress(out.data(), out.size(), in.data(), frame_size)};
    if (ZSTD_isError(written)) return std::nullopt;
    out.resize(written);
    return out;
}

#else

int ZstdMaxLevel() { return 0; }

std::vector<std::byte> ZstdCompress(Span<const std::byte>, int) { return {}; }

std::optional<std::vector<std::byte>> ZstdDecompress(Span<const std::byte>, size_t) { return std::nullopt; }

#endif // ENABLE_ZSTD

} // namespace util
