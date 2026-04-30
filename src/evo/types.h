// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_EVO_TYPES_H
#define BITCOIN_EVO_TYPES_H

#include <cstdint>
#include <memory>

class CDeterministicMN;

using CDeterministicMNCPtr = std::shared_ptr<const CDeterministicMN>;

namespace ProTxVersion {
enum : uint16_t {
    LegacyBLS = 1,
    BasicBLS = 2,
    ExtAddr = 3,
};

/** Get highest permissible ProTx version based on flags set. */
[[nodiscard]] constexpr uint16_t GetMax(const bool is_basic_scheme_active, const bool is_extended_addr)
{
    if (is_basic_scheme_active) {
        if (is_extended_addr) {
            // Requires *both* forks to be active to use extended addresses. is_basic_scheme_active could
            // be set to false due to RPC specialization, so we must evaluate is_extended_addr *last* to
            // avoid accidentally upgrading a legacy BLS node to basic BLS due to v24 activation.
            return ProTxVersion::ExtAddr;
        }
        return ProTxVersion::BasicBLS;
    }
    return ProTxVersion::LegacyBLS;
}

} // namespace ProTxVersion

#endif // BITCOIN_EVO_TYPES_H
