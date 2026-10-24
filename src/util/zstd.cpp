// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <util/zstd.h>

#include <util/system.h>

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

int DefaultZstdLevel()
{
    return ZstdAvailable() ? DEFAULT_ZSTD_LEVEL : 0;
}

int ZstdLevel()
{
    if (!ZstdAvailable()) return 0;
    return gArgs.GetIntArg("-blockcompression", DEFAULT_ZSTD_LEVEL);
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
    const unsigned long long content_size{ZSTD_getFrameContentSize(in.data(), in.size())};
    if (content_size == ZSTD_CONTENTSIZE_UNKNOWN || content_size == ZSTD_CONTENTSIZE_ERROR || content_size > max_out) {
        return std::nullopt;
    }
    std::vector<std::byte> out(content_size);
    const size_t written{ZSTD_decompress(out.data(), out.size(), in.data(), in.size())};
    if (ZSTD_isError(written) || written != out.size()) return std::nullopt;
    return out;
}

#else

int ZstdMaxLevel() { return 0; }

std::vector<std::byte> ZstdCompress(Span<const std::byte>, int) { return {}; }

std::optional<std::vector<std::byte>> ZstdDecompress(Span<const std::byte>, size_t) { return std::nullopt; }

#endif // ENABLE_ZSTD

} // namespace util
