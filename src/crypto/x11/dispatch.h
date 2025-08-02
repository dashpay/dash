// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_CRYPTO_X11_DISPATCH
#define BITCOIN_CRYPTO_X11_DISPATCH

#include <cstdint>

namespace sapphire {
namespace dispatch {
// aes_helper
typedef void (*AESRoundFn)(uint32_t, uint32_t, uint32_t, uint32_t,
                           uint32_t, uint32_t, uint32_t, uint32_t,
                           uint32_t&, uint32_t&, uint32_t&, uint32_t&);
typedef void (*AESRoundFnNk)(uint32_t, uint32_t, uint32_t, uint32_t,
                             uint32_t&, uint32_t&, uint32_t&, uint32_t&);

// echo
typedef void (*EchoRoundFn)(uint64_t[16][2], uint32_t&, uint32_t&, uint32_t&, uint32_t&);

// shavite
typedef void (*ShaviteCompressFn)(uint32_t&, uint32_t&, uint32_t&, uint32_t&,
                                  uint32_t, uint32_t, uint32_t, uint32_t,
                                  const uint32_t*);
} // namespace dispatch
} // namespace sapphire

void SapphireAutoDetect();

#endif // BITCOIN_CRYPTO_X11_DISPATCH
