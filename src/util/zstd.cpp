// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <util/fs_helpers.h>
#include <util/zstd.h>

#include <streams.h>
#include <tinyformat.h>

#include <algorithm>
#include <cstdio>
#include <ios>

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
#ifdef ENABLE_ZSTD
    return DEFAULT_ZSTD_LEVEL;
#else
    return 0;
#endif
}

void ZstdFileReader::ReadPlain(Span<std::byte> dst)
{
    if (dst.size() > m_remaining) throw std::ios_base::failure("ZstdFileReader::read: end of data");
    m_file.read(dst);
    m_remaining -= dst.size();
}

void ZstdFileReader::ignore(size_t n)
{
    if (!m_compressed) {
        if (n > m_remaining) throw std::ios_base::failure("ZstdFileReader::ignore: end of data");
        if (FileSeek64(m_file.Get(), n, SEEK_CUR) != 0) throw std::ios_base::failure("ZstdFileReader::ignore: fseek failed");
        m_remaining -= n;
        return;
    }
    std::byte buf[1024];
    while (n > 0) {
        const size_t now{std::min(n, sizeof(buf))};
        read({buf, now});
        n -= now;
    }
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

ZstdFileReader::ZstdFileReader(AutoFile& file, size_t size, bool compressed, size_t max_out)
    : m_file{file},
      m_compressed{compressed},
      m_remaining{size},
      m_max_out{max_out},
      m_stream{compressed ? ZSTD_createDStream() : nullptr, [](ZSTD_DCtx_s* stream) { ZSTD_freeDStream(stream); }}
{
    if (!m_compressed) return;
    if (!m_stream) throw std::ios_base::failure("ZstdFileReader: ZSTD_createDStream failed");
    m_in.resize(ZSTD_DStreamInSize());
    m_out.resize(ZSTD_DStreamOutSize());
}

void ZstdFileReader::read(Span<std::byte> dst)
{
    if (!m_compressed) return ReadPlain(dst);
    while (!dst.empty()) {
        if (!m_unread.empty()) {
            const size_t n{std::min(dst.size(), m_unread.size())};
            std::copy_n(m_unread.begin(), n, dst.begin());
            m_unread = m_unread.subspan(n);
            dst = dst.subspan(n);
            continue;
        }
        if (m_frame_done) throw std::ios_base::failure("ZstdFileReader::read: end of data");
        if (m_pending.empty()) {
            if (m_remaining == 0) throw std::ios_base::failure("ZstdFileReader::read: truncated frame");
            const Span<std::byte> chunk{Span{m_in}.first(std::min(m_remaining, m_in.size()))};
            m_file.read(chunk);
            m_remaining -= chunk.size();
            m_pending = chunk;
        }
        ZSTD_inBuffer in{m_pending.data(), m_pending.size(), 0};
        ZSTD_outBuffer out{m_out.data(), m_out.size(), 0};
        const size_t ret{ZSTD_decompressStream(m_stream.get(), &out, &in)};
        if (ZSTD_isError(ret)) throw std::ios_base::failure(strprintf("ZstdFileReader::read: %s", ZSTD_getErrorName(ret)));
        if (out.pos > m_max_out - m_decompressed) throw std::ios_base::failure("ZstdFileReader::read: record decompresses to more than its limit");
        m_decompressed += out.pos;
        m_pending = m_pending.subspan(in.pos);
        m_unread = Span{m_out}.first(out.pos);
        m_frame_done = ret == 0;
    }
}

#else

int ZstdMaxLevel() { return 0; }

std::vector<std::byte> ZstdCompress(Span<const std::byte>, int) { return {}; }

std::optional<std::vector<std::byte>> ZstdDecompress(Span<const std::byte>, size_t) { return std::nullopt; }

ZstdFileReader::ZstdFileReader(AutoFile& file, size_t size, bool compressed, size_t max_out)
    : m_file{file}, m_compressed{compressed}, m_remaining{size}, m_max_out{max_out}, m_stream{nullptr, [](ZSTD_DCtx_s*) {}} {}

void ZstdFileReader::read(Span<std::byte> dst)
{
    if (m_compressed) throw std::ios_base::failure("ZstdFileReader::read: compressed record, but this build has no zstd support");
    ReadPlain(dst);
}

#endif // ENABLE_ZSTD

} // namespace util
