// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PLATFORM_HELPERS_H
#define BITCOIN_PLATFORM_HELPERS_H

#include <platform/types.h>
#include <util/result.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

//! Pure helpers over the SDK shell: the DPNS label rules, protocol
//! constants and the DIP-15 pieces that need nothing beyond the 32-byte
//! outputs the wallet hands over. No protocol byte is computed in C++.
namespace platform::helpers {

enum class SystemContract : uint8_t {
    DPNS = 0,
    DASHPAY = 1
};

//! Homograph-safe normalization of a DPNS label (o->0, i/l->1, lower case).
std::string NormalizeLabel(const std::string& label);
bool IsValidUsername(const std::string& label);
//! Whether a label is contested (masternode vote) under the DPNS contract.
bool IsContestedUsername(const std::string& label);

Identifier SystemContractId(SystemContract which);
uint64_t CreditsPerDuff();

//! Decrypts a contact request's encryptedPublicKey with the ECDH secret the
//! wallet derived, into the 69-byte DIP-15 compact xpub.
util::Result<std::array<uint8_t, 69>> Dip15DecryptXpub(const std::array<uint8_t, 32>& shared_secret,
                                                       const std::vector<uint8_t>& ciphertext);

//! DIP-15 accountReference masking over the MAC the wallet computed.
uint32_t Dip15AccountReferenceFromMac(const std::array<uint8_t, 32>& mac, uint32_t account_index, uint32_t version);
struct AccountReference {
    uint32_t version{0};
    uint32_t account_index{0};
};
AccountReference Dip15UnmaskAccountReference(const std::array<uint8_t, 32>& mac, uint32_t reference);

//! The recipient key a contact request to identity should reference, per
//! the SDK's mint-side purpose policy; nullopt when it has none.
std::optional<uint32_t> Dip15SelectRecipientKey(const Identity& identity);

//! Whether an inbound contact request's key references are acceptable for
//! the ECDH that unwraps its encryptedPublicKey: the SDK's receive-side
//! purpose policy plus "never ECDH with the MASTER key".
bool Dip15ReceiveKeysAcceptable(IdentityPublicKey::Purpose sender_purpose, IdentityPublicKey::Purpose recipient_purpose,
                                uint32_t recipient_key_id);

} // namespace platform::helpers

#endif // BITCOIN_PLATFORM_HELPERS_H
