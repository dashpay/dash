// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PLATFORM_STATETRANSITIONS_H
#define BITCOIN_PLATFORM_STATETRANSITIONS_H

#include <platform/types.h>
#include <uint256.h>

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

/**
 * Construction and signing of DPP state transitions (bincode
 * platform-serialization, pinned to a single Platform protocol version).
 * Mirrors dashpay/platform packages/rs-dpp state_transition types; every
 * builder returns the exact bytes to hand to
 * PlatformClient::broadcastStateTransition.
 *
 * Signing is delegated through callbacks so private keys stay in the wallet
 * (interfaces::Wallet::signPlatformDigest): the callback receives the
 * double-SHA256 digest of the transition's signable bytes and must return a
 * compact/recoverable ECDSA signature (65 bytes).
 */
namespace platform::st {

//! Signs a 32-byte digest, returning a 65-byte compact recoverable ECDSA
//! signature. Returns false on failure (e.g. locked wallet).
using Signer = std::function<bool(const uint256& digest, std::vector<uint8_t>& sig_out)>;

template <typename T>
struct Result {
    std::optional<T> value;
    std::string error;
    bool ok() const { return value.has_value(); }
};

//! Asset lock proof for identity create/top-up.
struct InstantAssetLockProof {
    std::vector<uint8_t> transaction;   //!< serialized asset lock tx
    std::vector<uint8_t> instant_lock;  //!< serialized islock message
    uint32_t output_index{0};           //!< index of the OP_RETURN output in tx.vout
};
struct ChainAssetLockProof {
    uint32_t core_chain_locked_height{0};
    std::array<uint8_t, 36> out_point{}; //!< txid (big-endian) || index (LE u32)
};

//! Identity public key to register with a new identity.
struct NewIdentityKey {
    uint32_t id{0};
    IdentityPublicKey::Purpose purpose{IdentityPublicKey::Purpose::AUTHENTICATION};
    IdentityPublicKey::SecurityLevel security_level{IdentityPublicKey::SecurityLevel::MASTER};
    std::vector<uint8_t> pubkey; //!< compressed secp256k1 public key (33B)
    //! Signs with the corresponding private key (each registered key must
    //! prove ownership by signing the transition's signable bytes).
    Signer signer;
};

struct BuiltTransition {
    std::vector<uint8_t> bytes;   //!< serialized signed state transition
    uint256 hash;                 //!< sha256(bytes) — wait handle for waitForStateTransitionResult
};

//! Compute the identity id for an asset lock outpoint
//! (DSHA256 of the 36-byte outpoint), as InstantAssetLockProof::createIdentifier.
Identifier IdentityIdFromOutpoint(const std::array<uint8_t, 36>& out_point);

//! IdentityCreateTransition: registers public keys funded by the asset lock;
//! signed by the asset-lock one-time private key (asset_lock_signer).
Result<BuiltTransition> BuildIdentityCreate(
    const std::variant<InstantAssetLockProof, ChainAssetLockProof>& proof,
    const std::vector<NewIdentityKey>& keys,
    const Signer& asset_lock_signer);

//! DPNS preorder document create (documents batch transition), signed by the
//! identity HIGH-level authentication key. The salted domain hash
//! (DSHA256(salt || normalized_label || ".dash")) is derived on the Rust
//! side by the upstream DPNS document builder.
Result<BuiltTransition> BuildDpnsPreorder(
    const Identifier& identity,
    uint64_t identity_contract_nonce,
    const std::string& label,
    const std::array<uint8_t, 32>& preorder_salt,
    uint32_t signature_public_key_id,
    const Signer& signer);

//! DPNS domain document create. entropy is the 32-byte document entropy used
//! for the document id. preorder_salt must match the earlier preorder:
//! salted_domain_hash = DSHA256(salt || normalized_label || "." || parent).
Result<BuiltTransition> BuildDpnsDomain(
    const Identifier& identity,
    uint64_t identity_contract_nonce,
    const std::string& label,
    const std::string& normalized_label,
    const std::string& parent_domain,   //!< "dash"
    const std::array<uint8_t, 32>& preorder_salt,
    uint32_t signature_public_key_id,
    const Signer& signer);

//! DashPay profile create (revision 1) or replace (revision > 1).
Result<BuiltTransition> BuildProfile(
    const Identifier& identity,
    uint64_t identity_contract_nonce,
    const Profile& profile,
    uint64_t revision,
    const std::optional<Identifier>& existing_document_id, //!< set for replace
    uint32_t signature_public_key_id,
    const Signer& signer);

//! DashPay contactRequest create.
Result<BuiltTransition> BuildContactRequest(
    const Identifier& identity,
    uint64_t identity_contract_nonce,
    const ContactRequest& request,
    uint32_t signature_public_key_id,
    const Signer& signer);

//! Compute the salted domain hash for a DPNS (pre)order:
//! DSHA256(salt || <serialized normalized full name>) per
//! packages/rs-dpp DPNS logic (verify the exact concatenation against the
//! Rust implementation when implementing).
std::array<uint8_t, 32> SaltedDomainHash(
    const std::array<uint8_t, 32>& salt,
    const std::string& normalized_label,
    const std::string& parent_domain);

//! Homograph-safe normalization for DPNS labels (o/O->0, i/I/l/L->1,
//! lower-case) per platform convertToHomographSafeChars.
std::string NormalizeLabel(const std::string& label);

//! True if the normalized label is contested (masternode vote) per the DPNS
//! v1 contract: length 3..19 and matches [a-hj-km-np-z0-1-]+ style rules.
bool IsContestedLabel(const std::string& normalized_label);

} // namespace platform::st

#endif // BITCOIN_PLATFORM_STATETRANSITIONS_H
