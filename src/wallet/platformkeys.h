// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WALLET_PLATFORMKEYS_H
#define BITCOIN_WALLET_PLATFORMKEYS_H

#include <key.h>
#include <pubkey.h>
#include <span.h>
#include <support/allocators/secure.h>
#include <wallet/platformtypes.h>

#include <array>
#include <cstdint>
#include <variant>
#include <vector>

/**
 * Dash Platform (DIP-9/13/14/15) key derivation for the wallet.
 *
 * This is pure BIP32/DIP-14 key math: it knows nothing about Platform
 * documents, contracts or the network. It is only used through the
 * interfaces::Wallet platform key provider methods.
 *
 * References:
 *  - DIP-9  feature-purpose derivation (m/9'/...)
 *  - DIP-13 identity keys (m/9'/coin'/5'/...)
 *  - DIP-14 256-bit child indexes (CKey::Derive256)
 *  - DIP-15 DashPay friendship keychains (m/9'/coin'/15'/account'/idA/idB)
 *  - dashj DerivationPathFactory.java / FriendKeyChain.java (reference impl)
 */
namespace wallet::platformkeys {

//! One component of a derivation path: a 31-bit BIP32 index or a DIP-14
//! 256-bit index (big-endian bytes), plus a hardened flag.
struct PathElement {
    std::variant<uint32_t, std::array<uint8_t, 32>> index;
    bool hardened{false};

    static PathElement Hardened(uint32_t i) { return {i, true}; }
    static PathElement Normal(uint32_t i) { return {i, false}; }
    static PathElement Normal256(const std::array<uint8_t, 32>& i) { return {i, false}; }
};
using Path = std::vector<PathElement>;

// DIP-9 feature purpose, and DIP-13/DIP-15 feature types under it.
inline constexpr uint32_t FEATURE_PURPOSE{9};
inline constexpr uint32_t FEATURE_IDENTITIES{5}; // DIP-13
inline constexpr uint32_t FEATURE_DASHPAY{15};   // DIP-15

// DIP-13 sub-features: m/9'/coin'/5'/<subfeature>'
inline constexpr uint32_t IDENTITY_AUTHENTICATION{0};
inline constexpr uint32_t IDENTITY_REGISTRATION_FUNDING{1};
inline constexpr uint32_t IDENTITY_TOPUP_FUNDING{2};
inline constexpr uint32_t IDENTITY_INVITATION_FUNDING{3};

// DIP-13 authentication key types: m/9'/coin'/5'/0'/<keytype>'
inline constexpr uint32_t AUTH_KEY_TYPE_ECDSA{0};
inline constexpr uint32_t AUTH_KEY_TYPE_BLS{1};

//! An extended key produced by walking a (possibly 256-bit) derivation path.
//! Unlike CExtKey this does not carry BIP32 serialization metadata beyond the
//! parent fingerprint; DIP-14 extended-key serialization tracks the path
//! separately.
struct ExtKey256 {
    CKey key;
    ChainCode chaincode;
    //! BIP32 fingerprint of the key the last path step was derived from
    //! (Hash160 prefix of its public key); the DIP-15 compact xpub carries it.
    std::array<uint8_t, 4> parent_fingerprint{};

    ExtKey256() = default;
};

struct ExtPubKey256 {
    CPubKey pubkey;
    ChainCode chaincode;
};

//! m/9'/coin'/5'/0'/0'/<identity>'/<key>' — ECDSA identity authentication key
//! (all components hardened; key 0 = MASTER, key 1 = HIGH security level).
Path IdentityAuthKeyPath(uint32_t coin_type, uint32_t identity_index, uint32_t key_index);

//! Convert a typed Platform key request to its DIP-13 derivation path.
Path PlatformKeyPath(uint32_t coin_type, const PlatformKeyRequest& request);

//! m/9'/coin'/15'/<account>'/<identity_a>/<identity_b> — DIP-15 friendship keychain
//! root. The two 256-bit identity ids are NOT hardened (this is what allows
//! watch-only derivation from an exported xpub); the receiving chain is
//! (userA = my id, userB = their id), the sending chain is the reverse.
Path FriendshipPath(uint32_t coin_type, uint32_t account, Span<const uint8_t> user_a_id, Span<const uint8_t> user_b_id);

//! Derive the extended key at `path` from a BIP32 extended private key.
//! Returns false if the parent key or any derivation step is invalid.
[[nodiscard]] bool DeriveExtKey(const CExtKey& parent, const Path& path, ExtKey256& out);

//! Derive a (non-hardened) child extended pubkey. Fails on hardened steps.
[[nodiscard]] bool DerivePubKey(const ExtPubKey256& parent, const PathElement& element, ExtPubKey256& out);

//! ECDH shared secret between `key` and `counterparty`, using the libsecp256k1
//! ECDH KDF (SHA256 of the compressed shared point). This matches dashj's
//! KeyCrypterECDH / Secp256k1ECDHAgreement, used for DashPay contact request
//! encryption. Returns a 32-byte secret.
[[nodiscard]] bool ComputeECDHSecret(const CKey& key, const CPubKey& counterparty, SecureVector& secret_out);

//! DIP-15 accountReference MAC: HMAC-SHA256 keyed by the sender's ENCRYPTION
//! private key over the compact xpub sent in the contact request. Matches
//! rs-platform-encryption's calculate_account_reference, which then masks
//! the account index with the low bits of this digest; the masking itself
//! is not done here.
[[nodiscard]] bool ComputeAccountReferenceMac(const CKey& key, const CompactXpub& compact_xpub, uint256& mac_out);

} // namespace wallet::platformkeys

#endif // BITCOIN_WALLET_PLATFORMKEYS_H
