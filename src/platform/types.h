// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PLATFORM_TYPES_H
#define BITCOIN_PLATFORM_TYPES_H

#include <platform/params.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace platform {

using Bytes = std::vector<uint8_t>;

//! Identity public key record (DPP IdentityPublicKey, subset the GUI needs).
struct IdentityPublicKey {
    enum class Type : uint8_t { ECDSA_SECP256K1 = 0, BLS12_381 = 1, ECDSA_HASH160 = 2, BIP13_SCRIPT_HASH = 3, EDDSA_25519_HASH160 = 4 };
    enum class Purpose : uint8_t { AUTHENTICATION = 0, ENCRYPTION = 1, DECRYPTION = 2, TRANSFER = 3, VOTING = 5 };
    enum class SecurityLevel : uint8_t { MASTER = 0, CRITICAL = 1, HIGH = 2, MEDIUM = 3 };

    uint32_t id{0};
    Purpose purpose{Purpose::AUTHENTICATION};
    SecurityLevel security_level{SecurityLevel::MASTER};
    Type type{Type::ECDSA_SECP256K1};
    bool read_only{false};
    std::vector<uint8_t> data; //!< serialized public key
    std::optional<uint64_t> disabled_at;
};

//! A Platform identity as the GUI sees it.
struct Identity {
    Identifier id{};
    uint64_t balance{0}; //!< platform credits
    uint64_t revision{0};
    std::vector<IdentityPublicKey> public_keys;
};

//! A resolved DPNS name (domain document subset).
struct DpnsName {
    std::string label;            //!< as registered, e.g. "Alice"
    std::string normalized_label; //!< homograph-safe lower-case, e.g. "al1ce"
    std::string parent_domain;    //!< normalized parent, e.g. "dash"
    Identifier identity{};        //!< records.identity
    Identifier document_id{};
};

//! DashPay profile document (all fields optional per schema).
struct Profile {
    Identifier document_id{};
    Identifier owner_id{};
    std::string display_name;
    std::string public_message;
    std::string avatar_url;
    std::vector<uint8_t> avatar_hash;        //!< SHA256 of avatar (32B) if set
    std::vector<uint8_t> avatar_fingerprint; //!< dHash (8B) if set
    uint64_t created_at{0};                  //!< ms since epoch
    uint64_t updated_at{0};
    uint64_t revision{0};
};

//! DashPay contactRequest document.
struct ContactRequest {
    Identifier owner_id{};   //!< sender identity
    Identifier to_user_id{}; //!< recipient identity
    std::vector<uint8_t> encrypted_public_key; //!< 96B: IV(16) || AES-CBC(xpub)
    uint32_t sender_key_index{0};
    uint32_t recipient_key_index{0};
    uint32_t account_reference{0};
    std::vector<uint8_t> encrypted_account_label; //!< optional, 48-80B
    uint32_t core_height_created_at{0};
    uint64_t created_at{0}; //!< ms since epoch
    Identifier document_id{};
};

//! Contested-resource (premium username) vote state. Status is
//! contest-global: WON means the contest finished with `winner` awarded the
//! name (callers compare against their own identity); UNKNOWN means no
//! contest exists for the label (proven absent).
struct ContestedNameState {
    enum class Status { UNKNOWN, CONTEST_IN_PROGRESS, WON, LOST, LOCKED };
    Status status{Status::UNKNOWN};
    std::string normalized_label;
    std::vector<std::pair<Identifier, uint32_t>> contenders; //!< identity -> votes
    uint32_t abstain_votes{0};
    uint32_t lock_votes{0};
    std::optional<Identifier> winner; //!< set when status == WON
    std::optional<uint64_t> ends_at;  //!< ms since epoch (finish time once decided)
};

//! Result of broadcasting a state transition.
struct BroadcastResult {
    bool accepted{false};
    //! When not accepted: a platform consensus error description. Note this
    //! error channel is informational (not proof-backed); success is
    //! confirmed separately through a proved re-query of the created object.
    std::string error;
    uint32_t error_code{0};
};

//! Metadata every proved response is verified against. All fields are
//! covered by the Tenderdash quorum signature (they enter the StateId /
//! CanonicalVote sign bytes), so they are trustworthy once verification
//! succeeded.
struct ResponseMetadata {
    uint64_t height{0};              //!< platform block height
    uint32_t core_chain_locked_height{0};
    uint64_t time_ms{0};
    uint32_t protocol_version{0};
    std::string chain_id;            //!< tenderdash chain id
};

} // namespace platform

#endif // BITCOIN_PLATFORM_TYPES_H
