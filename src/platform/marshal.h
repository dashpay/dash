// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PLATFORM_MARSHAL_H
#define BITCOIN_PLATFORM_MARSHAL_H

#include <platform/types.h>

#include <dash/sdk/ffi.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

//! Conversions between dash-sdk-cxx's bridge structs and platform:: types.
namespace platform::marshal {

Result<Identity> FromFfi(const dash_sdk::IdentityResult& result);
Result<Identifier> FromFfi(const dash_sdk::IdResult& result);
Result<uint64_t> FromFfi(const dash_sdk::BalanceResult& result);
Result<Names> FromFfi(const dash_sdk::NamesResult& result);
Result<std::vector<Contest>> FromFfi(const dash_sdk::ContestsResult& result);

//! Every contact request created after since_ms, read a page per `read`
//! call until a page is complete.
Result<std::vector<ContactRequest>>
ReadContactRequests(const std::function<dash_sdk::ContactRequestsResult(uint64_t since_ms)>& read, uint64_t since_ms);

std::vector<dash_sdk::IdentityKey> ToFfi(const std::vector<IdentityPublicKey>& keys);
dash_sdk::AssetLockProof ToFfi(const AssetLockProof& proof);
dash_sdk::ContactRequestInput ToFfi(const ContactRequestInput& input);
dash_sdk::Proxy ToFfi(const std::optional<ProxyConfig>& proxy);

} // namespace platform::marshal

#endif // BITCOIN_PLATFORM_MARSHAL_H
