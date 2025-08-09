// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(ENABLE_SSE41) && defined(ENABLE_X86_AESNI)
#include <attributes.h>

#include <cstdint>

#include <immintrin.h>
#include <wmmintrin.h>

namespace {
void ALWAYS_INLINE Round128(const __m128i& x, const __m128i& k, __m128i& y) { y = _mm_aesenc_si128(x, k); }
void ALWAYS_INLINE Unpack(const __m128i& i, uint32_t& w0, uint32_t& w1, uint32_t& w2, uint32_t& w3)
{
    w0 = _mm_extract_epi32(i, 0);
    w1 = _mm_extract_epi32(i, 1);
    w2 = _mm_extract_epi32(i, 2);
    w3 = _mm_extract_epi32(i, 3);
}
} // anonymous namespace

namespace sapphire {
namespace aes_aesni {
void Round(uint32_t x0, uint32_t x1, uint32_t x2, uint32_t x3,
           uint32_t k0, uint32_t k1, uint32_t k2, uint32_t k3,
           uint32_t& y0, uint32_t& y1, uint32_t& y2, uint32_t& y3)
{
    __m128i state{_mm_setzero_si128()};
    Round128(_mm_set_epi32(x3, x2, x1, x0), _mm_set_epi32(k3, k2, k1, k0), state);
    Unpack(state, y0, y1, y2, y3);
}

void RoundKeyless(uint32_t x0, uint32_t x1, uint32_t x2, uint32_t x3,
                  uint32_t& y0, uint32_t& y1, uint32_t& y2, uint32_t& y3)
{
    __m128i state{_mm_setzero_si128()};
    Round128(_mm_set_epi32(x3, x2, x1, x0), _mm_setzero_si128(), state);
    Unpack(state, y0, y1, y2, y3);
}
} // namespace aes_aesni
} // namespace sapphire

#endif // ENABLE_SSE41 && ENABLE_X86_AESNI
