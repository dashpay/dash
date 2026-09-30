// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PLATFORM_MARSHAL_H
#define BITCOIN_PLATFORM_MARSHAL_H

#include <platform/types.h>

#include <dash/platform/ffi.h>

#include <cstdint>
#include <optional>

//! Conversions between the bridge structs of dash-platform-cxx and the
//! platform:: types the rest of Core uses. Only the client library includes
//! this header; nothing above it sees the bridge.
namespace platform::marshal {

Status FromFfi(const platform_ffi::Status& status);
ResponseMetadata FromFfi(const platform_ffi::Meta& meta);
ContractBounds FromFfi(const platform_ffi::ContractBounds& bounds);
IdentityPublicKey FromFfi(const platform_ffi::IdentityKey& key);
Identity FromFfi(const platform_ffi::Identity& identity);
DpnsName FromFfi(const platform_ffi::DpnsName& name);
Profile FromFfi(const platform_ffi::Profile& profile);
ContactRequest FromFfi(const platform_ffi::ContactRequest& request);
ContestedNameState FromFfi(const platform_ffi::ContestedState& state);
Built FromFfi(const platform_ffi::Built& built);

//! A verified read: the value is filled under OK and
//! UNSUPPORTED_PROTOCOL_VERSION, the metadata always.
Result<Identity> FromFfi(const platform_ffi::VerifiedIdentity& verified);
Result<uint64_t> FromFfi(const platform_ffi::VerifiedU64& verified);
Result<DpnsName> FromFfi(const platform_ffi::VerifiedDpnsName& verified);
Result<Paged<DpnsName>> FromFfi(const platform_ffi::VerifiedDpnsNames& verified);
Result<Profile> FromFfi(const platform_ffi::VerifiedProfile& verified);
Result<Paged<ContactRequest>> FromFfi(const platform_ffi::VerifiedContactRequests& verified);
Result<ContestedNameState> FromFfi(const platform_ffi::VerifiedContested& verified);

platform_ffi::ContractBounds ToFfi(const ContractBounds& bounds);
platform_ffi::IdentityKey ToFfi(const IdentityPublicKey& key);
platform_ffi::Identity ToFfi(const Identity& identity);
platform_ffi::Profile ToFfi(const Profile& profile);
platform_ffi::NewIdentityKey ToFfi(const NewIdentityKey& key);
platform_ffi::AssetLockProofInput ToFfi(const AssetLockProof& proof);
//! nullopt is kind 0 (connect directly).
platform_ffi::Proxy ToFfi(const std::optional<ProxyConfig>& proxy);

} // namespace platform::marshal

#endif // BITCOIN_PLATFORM_MARSHAL_H
