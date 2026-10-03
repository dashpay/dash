// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WALLET_PLATFORMTYPES_H
#define BITCOIN_WALLET_PLATFORMTYPES_H

#include <pubkey.h>
#include <script/standard.h>
#include <support/allocators/secure.h>
#include <uint256.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <variant>

namespace wallet {

enum class PlatformKeyStatus : uint8_t {
    SUCCESS,
    NOT_SUPPORTED,
    WALLET_LOCKED,
    AMBIGUOUS_SOURCE,
    INVALID_SOURCE,
    INVALID_ARGUMENT,
    IMPORT_ERROR,
    DERIVATION_ERROR,
};

template <typename T>
struct PlatformKeyResult {
    PlatformKeyStatus status{PlatformKeyStatus::DERIVATION_ERROR};
    T value{};

    explicit operator bool() const { return status == PlatformKeyStatus::SUCCESS; }
};

struct IdentityAuthKey {
    uint32_t identity_index{0};
    uint32_t key_index{0};
};

struct RegistrationFundingKey {
    uint32_t identity_index{0};
};

struct TopupFundingKey {
    uint32_t funding_index{0};
};

struct InvitationFundingKey {
    uint32_t invitation_index{0};
};

using PlatformKeyRequest = std::variant<IdentityAuthKey, RegistrationFundingKey, TopupFundingKey, InvitationFundingKey>;

struct FriendshipKeychainRequest {
    uint32_t account{0};
    uint256 local_id;
    uint256 remote_id;
    int64_t birth_time{0}; //!< 0 means unknown/genesis.
};

struct FriendshipXpub {
    CPubKey pubkey;
    ChainCode chaincode;
    //! BIP32 fingerprint (Hash160 prefix) of the key one level up, i.e. of
    //! m/9'/coin'/15'/account'/<user_a> for the receiving chain.
    std::array<uint8_t, 4> parent_fingerprint{};
};

//! DIP-15 compact extended public key: parentFingerprint(4) || chainCode(32)
//! || pubKey(33). This is the contactRequest encryptedPublicKey plaintext and
//! the accountReference MAC input; unlike a BIP32/DIP-14 serialization it
//! carries no version, depth or child number.
inline constexpr size_t COMPACT_XPUB_SIZE{4 + 32 + 33};
using CompactXpub = std::array<uint8_t, COMPACT_XPUB_SIZE>;

//! Serialize a friendship xpub in the compact form. Fails for a xpub whose
//! key is not compressed, as externally supplied contact xpubs may be.
[[nodiscard]] bool CompactXpubBytes(const FriendshipXpub& xpub, CompactXpub& compact_out);

//! Derive one contact payment destination from a DIP-15 friendship xpub.
//! This is public-only key math and does not require a wallet instance.
bool DeriveFriendshipPaymentDestination(const FriendshipXpub& xpub, uint32_t index, CTxDestination& destination_out);

} // namespace wallet

#endif // BITCOIN_WALLET_PLATFORMTYPES_H
