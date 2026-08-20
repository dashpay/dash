// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <platform/statetransitions.h>

#include <dash/platform/ffi.h>
#include <dash/platform/signer.h>

#include <crypto/common.h>
#include <hash.h>
#include <tinyformat.h>
#include <uint256.h>

#include <algorithm>
#include <utility>

/**
 * DPP state transition construction and signing, delegated to the
 * Platform-owned CXX bindings, which build and serialize with the real
 * rs-dpp. Signing crosses the FFI as a digest
 * callback (platform_ffi::WalletSigner) so private keys never leave the
 * wallet: Rust hands back the key id being signed plus the double-SHA256
 * digest of the transition's signable bytes and expects a 65-byte compact
 * recoverable ECDSA signature.
 *
 * The DPNS helpers (label normalization, salted domain hashes, the contested
 * rule) and the deterministic document entropy remain implemented here; the
 * builders validate/reproduce them against rs-dpp on the Rust side.
 */
namespace platform::st {

namespace {

uint256 DoubleSha(Span<const uint8_t> data)
{
    uint256 out;
    CHash256().Write(data).Finalize(out);
    return out;
}

std::array<uint8_t, 32> ToArray(const uint256& hash)
{
    std::array<uint8_t, 32> out;
    std::copy(hash.begin(), hash.end(), out.begin());
    return out;
}

//! Deterministic document entropy for DashPay documents:
//! DSHA256(owner_id || document_type_name || identity_contract_nonce LE64).
//! rs-dpp leaves entropy to the caller (the Rust SDK draws it at random);
//! deriving it from the (identity, contract nonce) pair keeps rebuilds of
//! the same transition byte-identical, so a GUI retry cannot register a
//! second document. DPNS documents use flow-specific entropy instead
//! (preorder: salted domain hash; domain: preorder salt).
std::array<uint8_t, 32> DeriveEntropy(const Identifier& owner,
                                      const std::string& document_type_name,
                                      uint64_t identity_contract_nonce)
{
    CHash256 hasher;
    hasher.Write(owner);
    hasher.Write(MakeUCharSpan(document_type_name));
    uint8_t nonce_le[8];
    WriteLE64(nonce_le, identity_contract_nonce);
    hasher.Write(nonce_le);
    uint256 hash;
    hasher.Finalize(hash);
    return ToArray(hash);
}

rust::Slice<const uint8_t> ToSlice(Span<const uint8_t> data)
{
    return {data.data(), data.size()};
}

//! Wraps a single st::Signer as the bridge's keyed signer; the key id is
//! already pinned on the C++ side (signature_public_key_id), so it is
//! ignored here.
platform_ffi::WalletSigner SingleKeySigner(const Signer& signer)
{
    return platform_ffi::WalletSigner{
        [&signer](uint32_t /*key_id*/, const std::array<uint8_t, 32>& digest,
                  std::vector<uint8_t>& sig_out) {
            return signer(uint256{digest}, sig_out);
        }};
}

//! The identity key the GUI signs document transitions with: its HIGH-level
//! ECDSA authentication key. Only the metadata reaches the bridge (rs-dpp
//! checks purpose and security level and records the key id in the
//! transition); the public key itself stays in the wallet with its private
//! counterpart.
platform_ffi::FfiIdentityKey HighAuthKey(uint32_t signature_public_key_id)
{
    platform_ffi::FfiIdentityKey key{};
    key.id = signature_public_key_id;
    key.purpose = static_cast<uint8_t>(IdentityPublicKey::Purpose::AUTHENTICATION);
    key.security_level = static_cast<uint8_t>(IdentityPublicKey::SecurityLevel::HIGH);
    key.key_type = static_cast<uint8_t>(IdentityPublicKey::Type::ECDSA_SECP256K1);
    key.read_only = false;
    key.has_disabled_at = false;
    key.disabled_at = 0;
    return key;
}

Result<BuiltTransition> FromFfi(const platform_ffi::FfiBuiltTransition& built)
{
    BuiltTransition out;
    out.bytes.assign(built.bytes.begin(), built.bytes.end());
    if (built.hash.size() != out.hash.size()) {
        return {std::nullopt, "bridge returned a malformed transition hash"};
    }
    std::copy(built.hash.begin(), built.hash.end(), out.hash.begin());
    return {std::move(out), ""};
}

} // namespace

Identifier IdentityIdFromOutpoint(const std::array<uint8_t, 36>& out_point)
{
    return ToArray(DoubleSha(out_point));
}

Result<BuiltTransition> BuildIdentityCreate(
    const std::variant<InstantAssetLockProof, ChainAssetLockProof>& proof,
    const std::vector<NewIdentityKey>& keys,
    const Signer& asset_lock_signer)
{
    if (keys.empty()) return {std::nullopt, "no identity keys provided"};
    if (!asset_lock_signer) return {std::nullopt, "no asset lock signer provided"};

    rust::Vec<platform_ffi::FfiNewIdentityKey> ffi_keys;
    ffi_keys.reserve(keys.size());
    for (const NewIdentityKey& key : keys) {
        if (!key.signer) return {std::nullopt, strprintf("identity key %d: no signer", key.id)};
        platform_ffi::FfiNewIdentityKey ffi_key{};
        ffi_key.id = key.id;
        ffi_key.purpose = static_cast<uint8_t>(key.purpose);
        ffi_key.security_level = static_cast<uint8_t>(key.security_level);
        ffi_key.pubkey.reserve(key.pubkey.size());
        for (const uint8_t byte : key.pubkey) ffi_key.pubkey.push_back(byte);
        ffi_keys.push_back(std::move(ffi_key));
    }

    // Route each per-key signature request to the matching registered key's
    // signer; the sentinel ASSET_LOCK_KEY_ID selects the one-time asset-lock
    // key. Every key signs the same signable-bytes digest.
    const platform_ffi::WalletSigner signer{
        [&keys, &asset_lock_signer](uint32_t key_id, const std::array<uint8_t, 32>& digest,
                                    std::vector<uint8_t>& sig_out) {
            const uint256 digest256{digest};
            if (key_id == platform_ffi::WalletSigner::ASSET_LOCK_KEY_ID) {
                return asset_lock_signer(digest256, sig_out);
            }
            for (const NewIdentityKey& key : keys) {
                if (key.id == key_id) return key.signer(digest256, sig_out);
            }
            return false;
        }};

    const bool is_instant{std::holds_alternative<InstantAssetLockProof>(proof)};
    Span<const uint8_t> transaction, instant_lock;
    uint32_t output_index{0}, core_chain_locked_height{0};
    Span<const uint8_t> out_point;
    if (is_instant) {
        const auto& instant{std::get<InstantAssetLockProof>(proof)};
        transaction = instant.transaction;
        instant_lock = instant.instant_lock;
        output_index = instant.output_index;
    } else {
        const auto& chain{std::get<ChainAssetLockProof>(proof)};
        core_chain_locked_height = chain.core_chain_locked_height;
        out_point = chain.out_point;
    }

    try {
        return FromFfi(platform_ffi::st_build_identity_create(
            is_instant, ToSlice(transaction), ToSlice(instant_lock), output_index,
            core_chain_locked_height, ToSlice(out_point), std::move(ffi_keys), signer));
    } catch (const rust::Error& e) {
        return {std::nullopt, e.what()};
    }
}

Result<BuiltTransition> BuildDpnsPreorder(
    const Identifier& identity,
    uint64_t identity_contract_nonce,
    const std::string& label,
    const std::array<uint8_t, 32>& preorder_salt,
    uint32_t signature_public_key_id,
    const Signer& signer)
{
    if (!signer) return {std::nullopt, "no signer provided"};
    const platform_ffi::WalletSigner ffi_signer{SingleKeySigner(signer)};
    try {
        // The preorder document has no natural id source; the bridge derives
        // the salted domain hash — already blinded and unique per
        // (name, salt) — and doubles it as the document entropy.
        return FromFfi(platform_ffi::st_build_dpns_preorder(
            ToSlice(identity), identity_contract_nonce, label, ToSlice(preorder_salt),
            signature_public_key_id, HighAuthKey(signature_public_key_id), ffi_signer));
    } catch (const rust::Error& e) {
        return {std::nullopt, e.what()};
    }
}

Result<BuiltTransition> BuildDpnsDomain(
    const Identifier& identity,
    uint64_t identity_contract_nonce,
    const std::string& label,
    const std::string& normalized_label,
    const std::string& parent_domain,
    const std::array<uint8_t, 32>& preorder_salt,
    uint32_t signature_public_key_id,
    const Signer& signer)
{
    if (!signer) return {std::nullopt, "no signer provided"};
    const platform_ffi::WalletSigner ffi_signer{SingleKeySigner(signer)};
    try {
        // The preorder salt is drawn fresh per registration attempt; the
        // bridge doubles it as the deterministic document entropy for the
        // paired domain create.
        return FromFfi(platform_ffi::st_build_dpns_domain(
            ToSlice(identity), identity_contract_nonce, label, normalized_label, parent_domain,
            ToSlice(preorder_salt), signature_public_key_id,
            HighAuthKey(signature_public_key_id), ffi_signer));
    } catch (const rust::Error& e) {
        return {std::nullopt, e.what()};
    }
}

Result<BuiltTransition> BuildProfile(
    const Identifier& identity,
    uint64_t identity_contract_nonce,
    const Profile& profile,
    uint64_t revision,
    const std::optional<Identifier>& existing_document_id,
    uint32_t signature_public_key_id,
    const Signer& signer)
{
    if (!signer) return {std::nullopt, "no signer provided"};
    const platform_ffi::WalletSigner ffi_signer{SingleKeySigner(signer)};
    const std::array<uint8_t, 32> entropy{DeriveEntropy(identity, "profile", identity_contract_nonce)};
    const Identifier existing{existing_document_id.value_or(Identifier{})};
    try {
        return FromFfi(platform_ffi::st_build_profile(
            ToSlice(identity), identity_contract_nonce, profile.display_name,
            profile.public_message, profile.avatar_url, ToSlice(profile.avatar_hash),
            ToSlice(profile.avatar_fingerprint), revision, existing_document_id.has_value(),
            ToSlice(existing), ToSlice(entropy), signature_public_key_id,
            HighAuthKey(signature_public_key_id), ffi_signer));
    } catch (const rust::Error& e) {
        return {std::nullopt, e.what()};
    }
}

Result<BuiltTransition> BuildContactRequest(
    const Identifier& identity,
    uint64_t identity_contract_nonce,
    const ContactRequest& request,
    uint32_t signature_public_key_id,
    const Signer& signer)
{
    if (!signer) return {std::nullopt, "no signer provided"};
    const platform_ffi::WalletSigner ffi_signer{SingleKeySigner(signer)};
    const std::array<uint8_t, 32> entropy{
        DeriveEntropy(identity, "contactRequest", identity_contract_nonce)};
    try {
        // $createdAt and $createdAtCoreBlockHeight are chain-assigned system
        // fields, so ContactRequest::created_at and ::core_height_created_at
        // do not enter the transition.
        return FromFfi(platform_ffi::st_build_contact_request(
            ToSlice(identity), identity_contract_nonce, ToSlice(request.to_user_id),
            ToSlice(request.encrypted_public_key), request.sender_key_index,
            request.recipient_key_index, request.account_reference,
            ToSlice(request.encrypted_account_label), ToSlice(entropy), signature_public_key_id,
            HighAuthKey(signature_public_key_id), ffi_signer));
    } catch (const rust::Error& e) {
        return {std::nullopt, e.what()};
    }
}

std::array<uint8_t, 32> SaltedDomainHash(
    const std::array<uint8_t, 32>& salt,
    const std::string& normalized_label,
    const std::string& parent_domain)
{
    // DSHA256(salt || normalized_label || "." || parent_domain), per the
    // Rust SDK DPNS registration flow
    // (packages/rs-sdk/src/platform/dpns_usernames/mod.rs register_dpns_name).
    const std::string full_name{normalized_label + "." + parent_domain};
    CHash256 hasher;
    hasher.Write(salt);
    hasher.Write(MakeUCharSpan(full_name));
    uint256 hash;
    hasher.Finalize(hash);
    return ToArray(hash);
}

std::string NormalizeLabel(const std::string& label)
{
    // Port of rs-dpp/src/util/strings.rs convert_to_homograph_safe_chars:
    // lower-case, then o->0 and l/i->1. DPNS labels are ASCII-only by
    // contract pattern, so ASCII lower-casing matches Rust's to_lowercase().
    std::string normalized;
    normalized.reserve(label.size());
    for (char c : label) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        switch (c) {
        case 'o': normalized.push_back('0'); break;
        case 'l':
        case 'i': normalized.push_back('1'); break;
        default: normalized.push_back(c);
        }
    }
    return normalized;
}

bool IsContestedLabel(const std::string& normalized_label)
{
    // DPNS domain contested index rule: the parentNameAndLabel index is
    // contested when normalizedLabel matches ^[a-zA-Z01-]{3,19}$
    // (packages/dpns-contract/schema/v1/dpns-contract-documents.json,
    // indices[0].contested.fieldMatches). Note digits 2-9 make a name
    // non-contested and normalized labels are already lower-case.
    if (normalized_label.size() < 3 || normalized_label.size() > 19) return false;
    for (char c : normalized_label) {
        const bool allowed{(c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                           c == '0' || c == '1' || c == '-'};
        if (!allowed) return false;
    }
    return true;
}

} // namespace platform::st
