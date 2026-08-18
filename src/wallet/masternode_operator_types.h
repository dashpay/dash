// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WALLET_MASTERNODE_OPERATOR_TYPES_H
#define BITCOIN_WALLET_MASTERNODE_OPERATOR_TYPES_H

#include <interfaces/masternode_operator.h>
#include <serialize.h>
#include <tinyformat.h>
#include <wallet/hdchain.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace wallet {

/**
 * Number of derivation indexes kept ahead of the consumption watermark. It is
 * both the size of the persisted recognition lookahead (so provider
 * transactions seen during sync can be matched without the seed) and the
 * BIP44-style gap limit at issuance: candidates are scanned against the
 * current masternode list until this many consecutive indexes are not in use,
 * and the key issued is the first index above every in-use hit.
 */
inline constexpr uint32_t MASTERNODE_OPERATOR_GAP_LIMIT{50};
/** Sanity bound on derivation depth; issuance and recognition never walk past it. */
inline constexpr uint32_t MASTERNODE_OPERATOR_MAX_INDEX{1000000};
inline constexpr uint32_t MASTERNODE_OPERATOR_PURPOSE{BIP32_PURPOSE_FEATURE};
inline constexpr uint32_t MASTERNODE_OPERATOR_PROVIDER_FEATURE{3};
inline constexpr uint32_t MASTERNODE_OPERATOR_SUBFEATURE{3};

using MasternodeOperatorKeyStatus = interfaces::MasternodeOperatorKeyStatus;

//! DashSync-compatible operator-key path m/9'/coin'/3'/3'/index. The first
//! four levels are hardened; the leaf is not.
inline std::array<uint32_t, 5> MasternodeOperatorDerivationPath(uint32_t coin_type, uint32_t index)
{
    constexpr uint32_t hardened{0x80000000};
    assert(index < hardened);
    return {
        hardened | MASTERNODE_OPERATOR_PURPOSE,
        hardened | coin_type,
        hardened | MASTERNODE_OPERATOR_PROVIDER_FEATURE,
        hardened | MASTERNODE_OPERATOR_SUBFEATURE,
        index,
    };
}

inline std::string MasternodeOperatorKeyPath(uint32_t coin_type, uint32_t index)
{
    const auto path{MasternodeOperatorDerivationPath(coin_type, index)};
    constexpr uint32_t hardened{0x80000000};
    return strprintf("m/%d'/%d'/%d'/%d'/%d", path[0] & ~hardened, path[1] & ~hardened, path[2] & ~hardened,
                     path[3] & ~hardened, path[4]);
}

/** Advisory consumption watermark: every derivation index below next_index is
 *  permanently consumed (issuance is strictly lowest-index-first, so the
 *  consumed set is always a prefix). Tagged with the seed-source identifier
 *  so a record left behind by a different seed is ignored rather than
 *  trusted. Holds no secret. */
struct MasternodeOperatorWatermark {
    std::vector<unsigned char> source_id;
    uint32_t next_index{0};

    SERIALIZE_METHODS(MasternodeOperatorWatermark, obj) { READWRITE(obj.source_id, obj.next_index); }
};

/** Advisory recognition lookahead: the basic-scheme public keys of the
 *  derivation indexes starting at base_index (normally the watermark), kept
 *  so the transaction-sync path can match provider transactions against
 *  upcoming keys without the seed - including while the wallet is locked.
 *  Rebuilt from the seed whenever it goes stale; holds no secret. */
struct MasternodeOperatorLookahead {
    std::vector<unsigned char> source_id;
    uint32_t base_index{0};
    std::vector<std::vector<unsigned char>> public_keys;

    SERIALIZE_METHODS(MasternodeOperatorLookahead, obj)
    {
        READWRITE(obj.source_id, obj.base_index, obj.public_keys);
    }
};

} // namespace wallet

#endif // BITCOIN_WALLET_MASTERNODE_OPERATOR_TYPES_H
