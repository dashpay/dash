// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PLATFORM_TYPES_H
#define BITCOIN_PLATFORM_TYPES_H

#include <netaddress.h>
#include <pubkey.h>
#include <uint256.h>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace platform {

using Identifier = std::array<uint8_t, 32>;

//! Outcome class of a Platform client call, as the SDK shell classifies it.
//! Every failure is typed by kind; the message is for logs only.
enum class StatusKind : uint8_t {
    OK = 0,
    //! The proof shows the queried object does not exist. The only path to
    //! "available", "no contacts" and "not registered".
    PROVEN_ABSENT = 1,
    //! Broadcast: a node already holds this state transition.
    ALREADY_EXISTS = 2,
    //! Broadcast: a node rejected the state transition with a consensus
    //! error carried in Status::consensus_code.
    CONSENSUS = 3,
    //! No endpoints, no local ChainLock anchor, the client is shut down, a
    //! transport failure or timeout, or every address is banned.
    UNAVAILABLE = 4,
    //! A proof, signature, quorum or freshness check refused the response,
    //! or a node definitively refused the request.
    REJECTED = 5,
    //! A verified response signed for another Tenderdash chain.
    CHAIN_ID_MISMATCH = 6,
    //! A verified response from a protocol version this build does not
    //! know: the value is still returned, writes must stop until an update.
    UNSUPPORTED_PROTOCOL_VERSION = 7,
    //! A bug: bad input, a contained panic, or an unclassified SDK error.
    INTERNAL = 8,
};

struct Status {
    StatusKind kind{StatusKind::INTERNAL};
    uint32_t consensus_code{0};
    std::string message;
};

//! Metadata every proved response is verified against. All fields are
//! covered by the Tenderdash quorum signature.
struct ResponseMetadata {
    uint64_t height{0}; //!< platform block height
    uint32_t core_chain_locked_height{0};
    uint64_t time_ms{0};
    uint32_t protocol_version{0};
    std::string chain_id; //!< tenderdash chain id
};

//! Outcome of a proved read. The value is present under OK and, so the GUI
//! can still show it, under UNSUPPORTED_PROTOCOL_VERSION; absent otherwise.
template <typename T>
struct Result {
    Status status;
    ResponseMetadata metadata;
    std::optional<T> value;

    bool ok() const { return status.kind == StatusKind::OK; }
    bool provenAbsent() const { return status.kind == StatusKind::PROVEN_ABSENT; }
};

//! Cursor of a paged read: pass next_start_after to the next call while
//! has_more. One client call is one page.
template <typename T>
struct Paged {
    std::vector<T> items;
    Identifier next_start_after{};
    bool has_more{false};
};

//! Contract bounds of an identity key.
struct ContractBounds {
    enum class Kind : uint8_t {
        NONE = 0,
        SINGLE_CONTRACT = 1,
        SINGLE_CONTRACT_DOCUMENT_TYPE = 2,
        CONTRACT_GROUP = 3
    };
    Kind kind{Kind::NONE};
    Identifier contract_id{};
    std::string document_type;
};

//! Identity public key record (DPP IdentityPublicKey, subset the GUI needs).
struct IdentityPublicKey {
    enum class Type : uint8_t {
        ECDSA_SECP256K1 = 0,
        BLS12_381 = 1,
        ECDSA_HASH160 = 2,
        BIP13_SCRIPT_HASH = 3,
        EDDSA_25519_HASH160 = 4
    };
    enum class Purpose : uint8_t {
        AUTHENTICATION = 0,
        ENCRYPTION = 1,
        DECRYPTION = 2,
        TRANSFER = 3,
        VOTING = 5
    };
    enum class SecurityLevel : uint8_t {
        MASTER = 0,
        CRITICAL = 1,
        HIGH = 2,
        MEDIUM = 3
    };

    uint32_t id{0};
    Purpose purpose{Purpose::AUTHENTICATION};
    SecurityLevel security_level{SecurityLevel::MASTER};
    Type type{Type::ECDSA_SECP256K1};
    bool read_only{false};
    std::vector<uint8_t> data; //!< serialized public key
    std::optional<uint64_t> disabled_at;
    ContractBounds contract_bounds;
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
    std::string normalized_label; //!< homograph-safe lower-case, e.g. "a11ce"
    std::string parent_domain;    //!< normalized parent, e.g. "dash"
    Identifier identity{};        //!< records.identity
    Identifier document_id{};
    Identifier owner_id{};
};

//! DashPay profile document. The GUI edits and renders the display name and
//! public message; the remaining fields are carried so a replace keeps what
//! another wallet set.
struct Profile {
    Identifier document_id{};
    Identifier owner_id{};
    uint64_t revision{0};
    std::string display_name;
    std::string public_message;
    std::string avatar_url;
    std::vector<uint8_t> avatar_hash;
    std::vector<uint8_t> avatar_fingerprint;
    std::vector<uint8_t> core_payment_address;
    std::vector<uint8_t> platform_payment_address;
    std::vector<uint8_t> shielded_address;
    uint64_t created_at{0}; //!< ms since epoch
    uint64_t updated_at{0};
};

//! DashPay contactRequest document.
struct ContactRequest {
    Identifier document_id{};
    Identifier owner_id{};                     //!< sender identity
    Identifier to_user_id{};                   //!< recipient identity
    std::vector<uint8_t> encrypted_public_key; //!< 96B: IV(16) || AES-CBC(DIP-15 compact xpub)
    uint32_t sender_key_index{0};
    uint32_t recipient_key_index{0};
    uint32_t account_reference{0};
    std::vector<uint8_t> encrypted_account_label; //!< optional
    std::vector<uint8_t> auto_accept_proof;       //!< optional
    uint64_t created_at{0};                       //!< ms since epoch, sender-authored
    uint32_t core_height_created_at{0};
};

//! Contested-resource (premium username) vote state. A proven absence of
//! the contest is reported through Result::provenAbsent().
struct ContestedNameState {
    enum class Outcome : uint8_t {
        OPEN = 0,
        WON = 1,
        LOCKED = 2
    };
    struct Contender {
        Identifier identity{};
        uint32_t votes{0};
    };
    Outcome outcome{Outcome::OPEN};
    std::vector<Contender> contenders;
    uint32_t abstain_votes{0};
    uint32_t lock_votes{0};
    Identifier winner{}; //!< set when outcome == WON
    uint64_t ends_at{0}; //!< ms since epoch of the finalizing block, 0 while open
};

//! A signed state transition. hash is its transaction id; object_id is the
//! identity id or document id it creates or replaces.
struct Built {
    std::vector<uint8_t> bytes;
    uint256 hash;
    Identifier object_id{};
};

//! Funding of an identity registration: an InstantSend-locked asset lock
//! (consensus-encoded transaction and islock, output_index of the credit
//! output) or a ChainLocked outpoint (txid || vout).
struct AssetLockProof {
    bool is_instant{false};
    std::vector<uint8_t> transaction;
    std::vector<uint8_t> instant_lock;
    uint32_t output_index{0};
    uint32_t core_chain_locked_height{0};
    std::array<uint8_t, 36> out_point{};
};

//! A key to register with a new identity; always ECDSA secp256k1.
struct NewIdentityKey {
    uint32_t id{0};
    IdentityPublicKey::Purpose purpose{IdentityPublicKey::Purpose::AUTHENTICATION};
    IdentityPublicKey::SecurityLevel security_level{IdentityPublicKey::SecurityLevel::MASTER};
    CPubKey pubkey; //!< compressed
    ContractBounds contract_bounds;
};

//! The profile fields the GUI edits; an empty string omits the field.
struct ProfileInput {
    std::string display_name;
    std::string public_message;
};

//! A contact request to mint. shared_secret is the ECDH secret between our
//! sender_key_index key and the recipient's key at recipient_key_index;
//! account_reference is already masked.
struct ContactRequestInput {
    Identifier to_user_id{};
    uint32_t sender_key_index{0};
    uint32_t recipient_key_index{0};
    CPubKey recipient_pubkey;
    uint32_t account_reference{0};
    std::array<uint8_t, 69> compact_xpub{}; //!< parentFingerprint || chainCode || pubKey
    std::array<uint8_t, 32> shared_secret{};
    std::string account_label;
};

//! The SOCKS5 proxy every connection of a client is tunnelled through.
struct ProxyConfig {
    //! A numeric address, or the path of a Unix socket (without "unix:").
    std::variant<CService, std::string> address;
    //! -proxyrandomize: fresh credentials per connection, so Tor gives each
    //! its own circuit.
    bool randomize_credentials{true};

    bool operator==(const ProxyConfig&) const = default;
};

} // namespace platform

#endif // BITCOIN_PLATFORM_TYPES_H
