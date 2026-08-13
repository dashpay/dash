// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_INTERFACES_MASTERNODE_OPERATOR_H
#define BITCOIN_INTERFACES_MASTERNODE_OPERATOR_H

#include <uint256.h>

#include <cstdint>
#include <vector>

namespace interfaces {

/** Whether the node could provide a complete active-chain operator-key history. */
enum class MasternodeOperatorKeyHistoryStatus : uint8_t {
    SUCCESS,
    HISTORY_UNAVAILABLE,
};

/** Operator public keys ever assigned by ProRegTx or ProUpRegTx on the active chain. */
struct MasternodeOperatorKeyHistory {
    MasternodeOperatorKeyHistoryStatus status{MasternodeOperatorKeyHistoryStatus::HISTORY_UNAVAILABLE};
    std::vector<std::vector<unsigned char>> public_keys;
    uint256 tip_hash;
    int tip_height{-1};
};

} // namespace interfaces

#endif // BITCOIN_INTERFACES_MASTERNODE_OPERATOR_H
