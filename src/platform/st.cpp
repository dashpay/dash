// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <platform/st.h>

#include <platform/marshal.h>
#include <support/cleanse.h>
#include <util/translation.h>

#include <dash/platform/ffi.h>
#include <dash/platform/signer.h>

#include <algorithm>

namespace platform::st {

namespace {

//! The bridge's signer over the operation's WalletSigner. The bridge calls
//! it synchronously on this thread; C++ exceptions are swallowed into a
//! refusal by the bridge header.
platform_ffi::WalletSigner BridgeSigner(const WalletSigner& signer)
{
    return platform_ffi::WalletSigner(
        [&signer](uint32_t key_id, std::span<const uint8_t> signable, std::vector<uint8_t>& sig_out) {
            return signer.signForKey(key_id, Span<const uint8_t>{signable.data(), signable.size()}, sig_out);
        },
        [&signer](const std::array<uint8_t, 32>& sighash, std::vector<uint8_t>& sig_out) {
            return signer.signAssetLockSighash(sighash, sig_out);
        });
}

template <typename Fn>
util::Result<Built> Build(const SigningOperation& op, OperationKind kind, const Fn& fn)
{
    if (op.kind() != kind) {
        return util::Error{Untranslated("state transition builder called under the wrong signing operation")};
    }
    const WalletSigner signer{op};
    const platform_ffi::WalletSigner bridge_signer{BridgeSigner(signer)};
    try {
        return marshal::FromFfi(fn(bridge_signer));
    } catch (const std::exception& e) {
        return util::Error{Untranslated(e.what())};
    }
}

//! The identity key document transitions are signed with; the operation
//! carries it from the proved identity read.
util::Result<platform_ffi::IdentityKey> DocumentKey(const SigningOperation& op)
{
    if (!op.documentKey()) return util::Error{Untranslated("signing operation carries no document key")};
    return marshal::ToFfi(*op.documentKey());
}

} // namespace

util::Result<Built> BuildIdentityCreate(const platform_ffi::PlatformClient& sdk, const SigningOperation& op,
                                        const AssetLockProof& proof, const std::vector<NewIdentityKey>& keys)
{
    std::vector<platform_ffi::NewIdentityKey> ffi_keys;
    ffi_keys.reserve(keys.size());
    for (const auto& key : keys) {
        if (!key.pubkey.IsCompressed()) {
            return util::Error{Untranslated("identity keys must be compressed public keys")};
        }
        ffi_keys.push_back(marshal::ToFfi(key));
    }
    const platform_ffi::AssetLockProofInput ffi_proof{marshal::ToFfi(proof)};
    return Build(op, OperationKind::IDENTITY_CREATE, [&](const platform_ffi::WalletSigner& signer) {
        return sdk.build_identity_create(ffi_proof,
                                         rust::Slice<const platform_ffi::NewIdentityKey>{ffi_keys.data(), ffi_keys.size()},
                                         signer);
    });
}

util::Result<Built> BuildDpnsPreorder(const platform_ffi::PlatformClient& sdk, const SigningOperation& op,
                                      const Identifier& owner, uint64_t nonce, const std::string& label,
                                      const std::array<uint8_t, 32>& salt)
{
    auto key{DocumentKey(op)};
    if (!key) return util::Error{util::ErrorString(key)};
    return Build(op, OperationKind::DPNS_PREORDER, [&](const platform_ffi::WalletSigner& signer) {
        return sdk.build_dpns_preorder(owner, nonce, label, salt, *key, signer);
    });
}

util::Result<Built> BuildDpnsDomain(const platform_ffi::PlatformClient& sdk, const SigningOperation& op,
                                    const Identifier& owner, uint64_t nonce, const std::string& label,
                                    const std::array<uint8_t, 32>& salt)
{
    auto key{DocumentKey(op)};
    if (!key) return util::Error{util::ErrorString(key)};
    return Build(op, OperationKind::DPNS_DOMAIN, [&](const platform_ffi::WalletSigner& signer) {
        return sdk.build_dpns_domain(owner, nonce, label, salt, *key, signer);
    });
}

util::Result<Built> BuildProfile(const platform_ffi::PlatformClient& sdk, const SigningOperation& op,
                                 const Identifier& owner, uint64_t nonce, const Profile& existing,
                                 const ProfileInput& input)
{
    auto key{DocumentKey(op)};
    if (!key) return util::Error{util::ErrorString(key)};
    const platform_ffi::Profile ffi_existing{marshal::ToFfi(existing)};
    platform_ffi::ProfileInput ffi_input;
    ffi_input.display_name = input.display_name;
    ffi_input.public_message = input.public_message;
    return Build(op, OperationKind::PROFILE, [&](const platform_ffi::WalletSigner& signer) {
        return sdk.build_profile(owner, nonce, ffi_existing, ffi_input, *key, signer);
    });
}

util::Result<Built> BuildContactRequest(const platform_ffi::PlatformClient& sdk, const SigningOperation& op,
                                        const Identity& sender, const Identity& recipient, uint64_t nonce,
                                        const ContactRequestInput& input)
{
    auto key{DocumentKey(op)};
    if (!key) return util::Error{util::ErrorString(key)};
    if (!input.recipient_pubkey.IsCompressed()) {
        return util::Error{Untranslated("the recipient key is not a compressed public key")};
    }
    const platform_ffi::Identity ffi_sender{marshal::ToFfi(sender)};
    const platform_ffi::Identity ffi_recipient{marshal::ToFfi(recipient)};
    platform_ffi::ContactRequestInput ffi_input;
    ffi_input.to_user_id = input.to_user_id;
    ffi_input.sender_key_index = input.sender_key_index;
    ffi_input.recipient_key_index = input.recipient_key_index;
    std::copy(input.recipient_pubkey.begin(), input.recipient_pubkey.end(), ffi_input.recipient_pubkey.begin());
    ffi_input.account_reference = input.account_reference;
    std::copy_n(input.compact_xpub.begin(), 4, ffi_input.compact_xpub.parent_fingerprint.begin());
    std::copy_n(input.compact_xpub.begin() + 4, 32, ffi_input.compact_xpub.chain_code.begin());
    std::copy_n(input.compact_xpub.begin() + 36, 33, ffi_input.compact_xpub.public_key.begin());
    ffi_input.shared_secret = input.shared_secret;
    ffi_input.account_label = input.account_label;
    auto built{Build(op, OperationKind::CONTACT_REQUEST, [&](const platform_ffi::WalletSigner& signer) {
        return sdk.build_contact_request(ffi_sender, ffi_recipient, nonce, ffi_input, *key, signer);
    })};
    // The bridge zeroizes its own copies of the secret; this one is ours.
    memory_cleanse(ffi_input.shared_secret.data(), ffi_input.shared_secret.size());
    return built;
}

} // namespace platform::st
