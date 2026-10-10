// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_INTERFACES_MASTERNODE_OPERATOR_H
#define BITCOIN_INTERFACES_MASTERNODE_OPERATOR_H

#include <support/allocators/secure.h>

#include <cstdint>
#include <string>
#include <vector>

namespace interfaces {

//! Result of a deterministic masternode operator-key operation.
enum class MasternodeOperatorKeyStatus : uint8_t {
    SUCCESS,
    NOT_SUPPORTED,
    WALLET_LOCKED,
    EXHAUSTED,
    INVALID_KEY,
    NOT_FOUND,
    DATABASE_ERROR,
    DERIVATION_ERROR,
};

//! A deterministic masternode operator key returned by the wallet. The public
//! key uses the canonical basic-scheme serialization.
struct MasternodeOperatorKey {
    SecureVector secret_key;
    std::vector<unsigned char> public_key;
    std::string path;
};

struct MasternodeOperatorKeyResult {
    MasternodeOperatorKeyStatus status{MasternodeOperatorKeyStatus::DERIVATION_ERROR};
    MasternodeOperatorKey key;
};

} // namespace interfaces

#endif // BITCOIN_INTERFACES_MASTERNODE_OPERATOR_H
