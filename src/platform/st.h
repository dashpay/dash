// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PLATFORM_ST_H
#define BITCOIN_PLATFORM_ST_H

#include <platform/signer.h>
#include <platform/types.h>
#include <util/result.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace platform_ffi {
struct PlatformClient;
} // namespace platform_ffi

//! Thin adapters over the SDK's state-transition builders. Every builder
//! takes a SigningOperation, so it can only run inside a user-initiated,
//! unlocked, kind-scoped operation, and signs through the WalletSigner the
//! operation binds. The builders run to completion on the calling thread
//! with no network access; the SDK assembles, serializes and signs the
//! transition under the protocol version it has verified the network runs.
namespace platform::st {

//! Registers keys funded by the asset lock; every key signs its own
//! possession proof and the funding key signs the asset lock once.
util::Result<Built> BuildIdentityCreate(const platform_ffi::PlatformClient& sdk, const SigningOperation& op,
                                        const AssetLockProof& proof, const std::vector<NewIdentityKey>& keys);

//! DPNS preorder document for label under the given salt, which the caller
//! persists before broadcasting so the domain step can reuse it.
util::Result<Built> BuildDpnsPreorder(const platform_ffi::PlatformClient& sdk, const SigningOperation& op,
                                      const Identifier& owner, uint64_t nonce, const std::string& label,
                                      const std::array<uint8_t, 32>& salt);

//! DPNS domain document for label with the salt of its preorder.
util::Result<Built> BuildDpnsDomain(const platform_ffi::PlatformClient& sdk, const SigningOperation& op,
                                    const Identifier& owner, uint64_t nonce, const std::string& label,
                                    const std::array<uint8_t, 32>& salt);

//! Creates the profile when existing.document_id is all zero, otherwise
//! replaces existing at its revision + 1, keeping every field input does
//! not edit.
util::Result<Built> BuildProfile(const platform_ffi::PlatformClient& sdk, const SigningOperation& op,
                                 const Identifier& owner, uint64_t nonce, const Profile& existing,
                                 const ProfileInput& input);

//! DashPay contactRequest from sender to recipient; the SDK encrypts the
//! compact xpub with the ECDH secret the wallet derived.
util::Result<Built> BuildContactRequest(const platform_ffi::PlatformClient& sdk, const SigningOperation& op,
                                        const Identity& sender, const Identity& recipient, uint64_t nonce,
                                        const ContactRequestInput& input);

} // namespace platform::st

#endif // BITCOIN_PLATFORM_ST_H
