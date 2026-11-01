// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PLATFORM_SIGNER_H
#define BITCOIN_PLATFORM_SIGNER_H

#include <platform/types.h>
#include <wallet/platformtypes.h>

#include <dash/sdk/callbacks.h>

#include <cstdint>
#include <memory>
#include <optional>

namespace interfaces {
class LLMQ;
class Wallet;
} // namespace interfaces

//! The C++ objects dash-sdk-cxx calls back into. They run on SDK worker
//! threads and touch only thread-safe node and wallet interfaces.
namespace platform {

//! Signs with the wallet's Platform keys: identity key `key_id` as
//! IdentityAuthKey{identity_index, key_id}, and the asset lock with
//! funding_key (none: refused).
std::unique_ptr<dash_sdk::WalletSigner> MakeWalletSigner(interfaces::Wallet& wallet, uint32_t identity_index,
                                                         std::optional<wallet::AssetLockFundingKey> funding_key);

//! The wallet's half of a contact request from sender: the ECDH secret of
//! IdentityAuthKey{identity_index, sender_key_id}, and the compact xpub of
//! our DIP-15 receiving chain towards the recipient, imported as it is
//! published.
std::unique_ptr<dash_sdk::ContactKeys> MakeContactKeys(interfaces::Wallet& wallet, uint32_t identity_index,
                                                       const Identifier& sender);

//! Answers quorum keys from the quorums the node retains, matching both
//! type and hash. There is no Platform activation height source: the SDK
//! needs it only for a query the binding does not expose.
std::unique_ptr<dash_sdk::CoreContext> MakeCoreContext(interfaces::LLMQ& llmq, uint8_t platform_llmq_type);

} // namespace platform

#endif // BITCOIN_PLATFORM_SIGNER_H
