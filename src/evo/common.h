// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_EVO_COMMON_H
#define BITCOIN_EVO_COMMON_H

#include <cstdint>

namespace ProTxVersion {
enum : uint16_t {
    LegacyBLS = 1,
    BasicBLS  = 2,
    ExtAddr   = 3,
};
} // namespace ProTxVersion

#endif // BITCOIN_EVO_COMMON_H
