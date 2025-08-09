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
__m128i ALWAYS_INLINE Xor(const __m128i& x, const __m128i& y) { return _mm_xor_si128(x, y); }
} // anonymous namespace

namespace sapphire {
namespace shavite_aesni {
void CompressElement(uint32_t& l0, uint32_t& l1, uint32_t& l2, uint32_t& l3,
                     uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3,
                     const uint32_t* rk)
{
    /* Load state + XOR with round key 1 */
    __m128i state = _mm_set_epi32(r3, r2, r1, r0);
    state = Xor(state, _mm_loadu_si128((const __m128i*)&rk[0]));
    /* AES round + XOR with round key 2 */
    Round128(state, _mm_setzero_si128(), state);
    state = Xor(state, _mm_loadu_si128((const __m128i*)&rk[4]));
    /* AES round + XOR with round key 3 */
    Round128(state, _mm_setzero_si128(), state);
    state = Xor(state, _mm_loadu_si128((const __m128i*)&rk[8]));
    /* AES Round + XOR with round key 4 */
    Round128(state, _mm_setzero_si128(), state);
    state = Xor(state, _mm_loadu_si128((const __m128i*)&rk[12]));
    /* AES round */
    Round128(state, _mm_setzero_si128(), state);
    /* Unpack + XOR with l values */
    l0 ^= _mm_extract_epi32(state, 0);
    l1 ^= _mm_extract_epi32(state, 1);
    l2 ^= _mm_extract_epi32(state, 2);
    l3 ^= _mm_extract_epi32(state, 3);
}
} // namespace shavite_aesni
} // namespace sapphire

#endif // ENABLE_SSE41 && ENABLE_X86_AESNI
