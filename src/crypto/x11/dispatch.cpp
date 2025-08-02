// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <crypto/x11/dispatch.h>

#if !defined(DISABLE_OPTIMIZED_SHA256)
#include <compat/cpuid.h>
#endif // !DISABLE_OPTIMIZED_SHA256

#include <cstdint>

namespace sapphire {
#if !defined(DISABLE_OPTIMIZED_SHA256)
#if defined(ENABLE_SSE41) && defined(ENABLE_X86_AESNI)
namespace aes_aesni {
void Round(uint32_t x0, uint32_t x1, uint32_t x2, uint32_t x3,
           uint32_t k0, uint32_t k1, uint32_t k2, uint32_t k3,
           uint32_t& y0, uint32_t& y1, uint32_t& y2, uint32_t& y3);

void RoundKeyless(uint32_t x0, uint32_t x1, uint32_t x2, uint32_t x3,
                  uint32_t& y0, uint32_t& y1, uint32_t& y2, uint32_t& y3);
} // namespace aes_aesni
namespace echo_aesni {
void FullStateRound(uint64_t W[16][2], uint32_t& pK0, uint32_t& pK1, uint32_t& pK2, uint32_t& pK3);
} // namespace echo_aesni
#endif // ENABLE_SSE41 && ENABLE_X86_AESNI
#endif // !DISABLE_OPTIMIZED_SHA256
namespace aes_soft {
void Round(uint32_t x0, uint32_t x1, uint32_t x2, uint32_t x3,
           uint32_t k0, uint32_t k1, uint32_t k2, uint32_t k3,
           uint32_t& y0, uint32_t& y1, uint32_t& y2, uint32_t& y3);

void RoundKeyless(uint32_t x0, uint32_t x1, uint32_t x2, uint32_t x3,
                  uint32_t& y0, uint32_t& y1, uint32_t& y2, uint32_t& y3);
} // namespace aes_soft
namespace echo_soft {
void FullStateRound(uint64_t W[16][2], uint32_t& pK0, uint32_t& pK1, uint32_t& pK2, uint32_t& pK3);
} // namespace echo_soft
} // namespace sapphire

namespace {
[[maybe_unused]] bool use_aes_ni = []() {
#if !defined(DISABLE_OPTIMIZED_SHA256) && defined(HAVE_GETCPUID)
    uint32_t eax, ebx, ecx, edx;
    GetCPUID(1, 0, eax, ebx, ecx, edx);
    return (/*has_sse4_1=*/((ecx >> 19) & 1) &&
            /*has_aes_ni=*/((ecx >> 25) & 1));
#else
    return false;
#endif // !DISABLE_OPTIMIZED_SHA256 && HAVE_GETCPUID
}();
} // anonymous namespace

// aes_helper
extern sapphire::dispatch::AESRoundFn round_fn;
extern sapphire::dispatch::AESRoundFnNk round_keyless_fn;

// echo
extern sapphire::dispatch::EchoRoundFn aes_2rounds_all;

void SapphireAutoDetect()
{
    // aes_helper
    round_fn = sapphire::aes_soft::Round;
    round_keyless_fn = sapphire::aes_soft::RoundKeyless;

    // echo
    aes_2rounds_all = sapphire::echo_soft::FullStateRound;

#if !defined(DISABLE_OPTIMIZED_SHA256)
#if defined(ENABLE_SSE41) && defined(ENABLE_X86_AESNI)
    if (use_aes_ni) {
        // aes_helper
        round_fn = sapphire::aes_aesni::Round;
        round_keyless_fn = sapphire::aes_aesni::RoundKeyless;

        // echo
        aes_2rounds_all = sapphire::echo_aesni::FullStateRound;
    }
#endif // ENABLE_SSE41 && ENABLE_X86_AESNI
#endif // !DISABLE_OPTIMIZED_SHA256
}
