// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PLATFORM_WALLETRECORDS_H
#define BITCOIN_PLATFORM_WALLETRECORDS_H

#include <consensus/amount.h>
#include <platform/types.h>
#include <script/script.h>
#include <uint256.h>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace platform {

//! Wallet platform-data record keys. Every record is tagged by the chain it
//! was written for through the "platform/chain-id" record; a record whose
//! chain id or version no longer matches is wiped as a whole (see
//! IsRecordSetCurrent), never migrated.
namespace records {
inline constexpr char ENABLED[]{"platform/enabled"};
inline constexpr char CHAIN_ID[]{"platform/chain-id"};
inline constexpr char VERSION[]{"platform/version"};
//! Present while seed-only recovery still owes the wallet its contacts:
//! the identity record exists but the contact phase did not finish.
inline constexpr char RECOVERY_PENDING[]{"platform/recovery-pending"};
inline constexpr char IDENTITY[]{"identity/0"};
inline constexpr char CONTACT_KEY_PREFIX[]{"contact/key/"};
inline constexpr char CONTACT_IN_PREFIX[]{"contact/in/"};
inline constexpr char CONTACT_OUT_PREFIX[]{"contact/out/"};
inline constexpr char CONTACT_PAY_INDEX_PREFIX[]{"contact/pay-index/"};
inline constexpr char CONTACT_USERNAME_PREFIX[]{"contact/username/"};
inline constexpr char CONTACT_DISPLAY_NAME_PREFIX[]{"contact/display-name/"};
//! Present ({1}) while the user ignores this identity's request or hides
//! this contact; kept on this wallet only.
inline constexpr char CONTACT_HIDDEN_PREFIX[]{"contact/hidden/"};
//! The record layout every "identity/*" and "contact/*" record follows. A
//! wallet carrying another version has every Platform record wiped and
//! recovery rerun.
inline constexpr uint8_t CURRENT_VERSION{2};
} // namespace records

//! Persistent snapshot of the identity/username registration flow, stored
//! under records::IDENTITY (single identity per wallet in this version).
//! Shared between the GUI flow (IdentityFlow) and seed-only recovery, which
//! synthesizes the same record from proved Platform queries.
struct IdentityRecord {
    enum class State : uint8_t {
        NONE = 0,
        FUNDING_SENT = 1,
        FUNDING_LOCKED = 2,
        IDENTITY_BROADCAST = 3,
        IDENTITY_CONFIRMED = 4,
        PREORDER_BROADCAST = 5,
        PREORDER_WAIT = 6,
        DOMAIN_BROADCAST = 7,
        REGISTERED = 8,
        CONTESTED_PENDING = 9,
        //! A signing step is waiting for the user to unlock the wallet; the
        //! flow retries on the next user action, not every tick.
        NEEDS_UNLOCK = 10,
        FAILED = 255,
    };

    State state{State::NONE};
    //! L1 funding
    uint256 funding_txid;
    uint32_t funding_key_index{0}; //!< registration funding derivation index
    CAmount funding_amount{0};
    //! Identity
    Identifier identity_id{};
    //! Ids (and wallet derivation indexes) of the identity keys this wallet
    //! signs documents with (AUTHENTICATION, HIGH or CRITICAL) and runs the
    //! contact-request ECDH with (ENCRYPTION, DECRYPTION). A registration
    //! sets 1/2/3; recovery takes them from the proved identity after
    //! checking each key is the one the wallet derives at that index.
    uint32_t auth_key_id{0};
    uint32_t encryption_key_id{0};
    uint32_t decryption_key_id{0};
    //! Username
    std::string label;            //!< as typed, e.g. "Alice"
    std::string normalized_label; //!< homograph-safe
    std::array<uint8_t, 32> preorder_salt{};
    bool contested{false};
    //! The step NEEDS_UNLOCK parks (resumed there once the wallet unlocks)
    //! or FAILED left, which decides what "try again" keeps.
    State resume_state{State::NONE};
    //! Diagnostics
    std::string last_error;
    int64_t started_at{0};

    //! A state transition signed ahead of the step that broadcasts it, the
    //! identity contract nonce it was signed with, and the protocol version
    //! it was built under (0 when no verified read had shown one): a
    //! transition built before Platform upgraded may no longer apply.
    struct SignedTransition {
        std::vector<uint8_t> bytes;
        uint64_t nonce{0};
        uint32_t protocol_version{0};
        bool empty() const { return bytes.empty(); }
    };
    //! Transitions a registration signed under its one wallet unlock, so the
    //! later steps broadcast them without asking for the passphrase again.
    //! Each is cleared once its step is confirmed or it can no longer apply;
    //! a record without them signs each step when it gets there.
    std::vector<uint8_t> signed_identity_create;
    SignedTransition signed_preorder;
    SignedTransition signed_domain;
    //! The DashPay profile chosen during registration, published once the
    //! username is registered. The display name stays once it is published
    //! and is cleared when the flow gives the profile up.
    SignedTransition signed_profile;
    std::string profile_display_name;
    //! What last_error is about, so its text is worded when it is shown,
    //! not when it was stored, and "Show details" survives a restart.
    struct Failure {
        std::string operation;
        int64_t time{0};
        //! The failed Platform call, when a call failed.
        std::optional<Status> status;
    };
    std::optional<Failure> last_failure;

    //! True for an identity seed-only recovery proved exists but found no
    //! username for. The flow parks here until the user picks a name, which
    //! is then registered on its own (no asset lock: the identity's existing
    //! credits pay for it).
    bool AwaitsUsername() const { return state == State::IDENTITY_CONFIRMED && label.empty(); }
};

std::vector<unsigned char> SerializeIdentityRecord(const IdentityRecord& record);
bool DeserializeIdentityRecord(const std::vector<unsigned char>& data, IdentityRecord& record);

//! Per-contact outbound request record ("contact/out/<id>"): when our own
//! request was confirmed on Platform, in seconds. Doubles as the rescan
//! birth time of the friendship keychain (never the counterparty's
//! document time, which the sender controls).
std::vector<unsigned char> EncodeContactOutRecord(int64_t created_at);
std::optional<int64_t> DecodeContactOutRecord(const std::vector<unsigned char>& data);

//! "contact/pay-index/<id>" records: the next unused DIP-15 payment index
//! for a contact, as a 4-byte little-endian value. Decoding anything but a
//! well-formed record yields 0 (start of the chain).
std::vector<unsigned char> EncodePaymentCursor(uint32_t next_index);
uint32_t DecodePaymentCursor(const std::vector<unsigned char>& data);

//! Rebuild a contact's outbound payment cursor from wallet history: one past
//! the highest index in [0, window) whose derived payment script appears in
//! wallet_output_scripts, or 0 when none match. derive returns the payment
//! script for an index; nullopt ends the scan early.
uint32_t ComputePaymentCursor(uint32_t window, const std::function<std::optional<CScript>(uint32_t)>& derive,
                              const std::set<CScript>& wallet_output_scripts);

//! Whether the "platform/version" record of a wallet matches the layout this
//! build writes. An absent record on a wallet without Platform records is
//! current (nothing to wipe); anything else that differs is not.
bool IsRecordSetCurrent(const std::vector<unsigned char>& version_record, bool have_platform_records);
std::vector<unsigned char> EncodeRecordVersion();

} // namespace platform

#endif // BITCOIN_PLATFORM_WALLETRECORDS_H
