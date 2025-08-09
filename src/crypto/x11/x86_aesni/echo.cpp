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
namespace echo_aesni {
void FullStateRound(uint64_t W[16][2], uint32_t& pK0, uint32_t& pK1, uint32_t& pK2, uint32_t& pK3)
{
    __m128i key{_mm_set_epi32(pK3, pK2, pK1, pK0)};
    __m128i zero{_mm_setzero_si128()};

    for (int n = 0; n < 16; n++) {
        __m128i block{_mm_loadu_si128((const __m128i*)&W[n][0])};
        Round128(block, key, block);
        Round128(block, zero, block);
        _mm_storeu_si128((__m128i*)&W[n][0], block);

        uint32_t K0, K1, K2, K3;
        Unpack(key, K0, K1, K2, K3);

        if ((K0 = (K0 + 1)) == 0) {
            if ((K1 = (K1 + 1)) == 0) {
                if ((K2 = (K2 + 1)) == 0) {
                    K3 = (K3 + 1);
                }
            }
        }

        key = _mm_set_epi32(K3, K2, K1, K0);
    }

    Unpack(key, pK0, pK1, pK2, pK3);
}
} // namespace echo_aesni
} // namespace sapphire

#endif // ENABLE_SSE41 && ENABLE_X86_AESNI
