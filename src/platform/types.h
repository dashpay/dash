// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PLATFORM_TYPES_H
#define BITCOIN_PLATFORM_TYPES_H

#include <netaddress.h>
#include <primitives/transaction.h>

#include <array>
#include <cstdint>
#include <string>
#include <variant>
#include <vector>

//! The values PlatformClient reads and writes, as plain Core types. They
//! mirror dash-sdk-cxx's bridge structs, so nothing above src/platform
//! includes the binding.
namespace platform {

using Identifier = std::array<uint8_t, 32>;

//! How a call ended, as the binding classifies it. The message is for logs.
enum class StatusKind : uint8_t {
    OK,
    //! Proved absence: the identity or name does not exist. For resolveName
    //! this means "does not resolve", not "available".
    NOT_FOUND,
    //! Transport, proxy, timeout, no endpoint, or the client is shut down.
    UNAVAILABLE,
    //! A proof or quorum check refused the response.
    REJECTED,
    //! Platform refused the state transition; consensus_code says why.
    CONSENSUS,
    //! Bad input, a refused callback, or an unclassified SDK error.
    INTERNAL,
};

struct Status {
    StatusKind kind{StatusKind::INTERNAL};
    uint32_t consensus_code{0};
    std::string message;
};

//! The value is meaningful only under OK.
template <typename T>
struct Result {
    Status status;
    T value{};

    bool ok() const { return status.kind == StatusKind::OK; }
};

struct IdentityPublicKey {
    enum class Type : uint8_t {
        ECDSA_SECP256K1 = 0,
        BLS12_381 = 1,
        ECDSA_HASH160 = 2,
        BIP13_SCRIPT_HASH = 3,
        EDDSA_25519_HASH160 = 4,
    };
    enum class Purpose : uint8_t {
        AUTHENTICATION = 0,
        ENCRYPTION = 1,
        DECRYPTION = 2,
        TRANSFER = 3,
        SYSTEM = 4,
        VOTING = 5,
        OWNER = 6,
    };
    enum class SecurityLevel : uint8_t {
        MASTER = 0,
        CRITICAL = 1,
        HIGH = 2,
        MEDIUM = 3,
    };

    uint32_t id{0};
    Purpose purpose{Purpose::AUTHENTICATION};
    SecurityLevel security_level{SecurityLevel::MASTER};
    Type type{Type::ECDSA_SECP256K1};
    bool read_only{false};
    std::vector<uint8_t> data; //!< the 33-byte public key for ECDSA_SECP256K1
    uint64_t disabled_at{0};   //!< 0: enabled
};

struct Identity {
    Identifier id{};
    uint64_t balance{0}; //!< credits
    uint64_t revision{0};
    std::vector<IdentityPublicKey> public_keys;
};

struct DpnsName {
    std::string label;
    std::string normalized_label;
    Identifier owner{};
    Identifier identity{}; //!< records.identity, all zero if none
};

struct Names {
    std::vector<DpnsName> names;
    //! false: the page bound was reached, so the list is a lower bound.
    bool complete{false};
};

//! A contested name the identity is a contender for, still being voted on.
struct Contest {
    std::string normalized_label;
    uint64_t ends_at_ms{0};
};

enum class ContactDirection : uint8_t {
    SENT,
    RECEIVED,
};

//! A DashPay contactRequest document.
struct ContactRequest {
    Identifier document_id{};
    Identifier owner{};
    Identifier to_user_id{};
    std::vector<uint8_t> encrypted_public_key;
    uint32_t sender_key_index{0};
    uint32_t recipient_key_index{0};
    uint32_t account_reference{0};
    std::vector<uint8_t> encrypted_account_label; //!< empty: absent
    std::vector<uint8_t> auto_accept_proof;       //!< empty: absent
    uint64_t created_at_ms{0};
    uint32_t created_at_core_height{0};
};

//! An InstantSend-locked asset lock: the transaction, its serialized islock
//! and the index of the credit output.
struct InstantAssetLockProof {
    CTransactionRef transaction;
    std::vector<uint8_t> instant_lock;
    uint32_t output_index{0};
};

//! A ChainLocked asset lock: its credit output and a ChainLocked height at
//! or above the block that mined it.
struct ChainAssetLockProof {
    uint32_t core_chain_locked_height{0};
    COutPoint out_point;
};

using AssetLockProof = std::variant<InstantAssetLockProof, ChainAssetLockProof>;

//! A DIP-15 contact request to send. account_reference is the finished
//! value from interfaces::Wallet::platformAccountReference.
struct ContactRequestInput {
    Identifier sender{};
    Identifier recipient{};
    uint32_t sender_key_index{0}; //!< our ENCRYPTION key
    uint32_t recipient_key_index{0};
    uint32_t signing_key_id{0};
    uint32_t account_reference{0};
    std::string account_label; //!< empty: none
};

//! The SOCKS5 proxy every connection of a client goes through.
struct ProxyConfig {
    //! A numeric address, or the path of a Unix socket (without "unix:").
    std::variant<CService, std::string> address;
    //! -proxyrandomize: fresh credentials per connection.
    bool randomize_credentials{true};
};

} // namespace platform

#endif // BITCOIN_PLATFORM_TYPES_H
