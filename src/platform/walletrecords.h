// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PLATFORM_WALLETRECORDS_H
#define BITCOIN_PLATFORM_WALLETRECORDS_H

#include <consensus/amount.h>
#include <platform/params.h>
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

//! Persistent snapshot of the identity/username registration flow, stored
//! in the wallet DB under the "identity/0" platform data key (single
//! identity per wallet in this version). Shared between the GUI flow
//! (IdentityFlow) and seed-only recovery, which synthesizes the same record
//! from proved Platform queries.
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
        FAILED = 255,
    };

    static constexpr uint8_t CURRENT_VERSION{1};
    uint8_t version{CURRENT_VERSION};
    State state{State::NONE};
    //! L1 funding
    uint256 funding_txid;
    uint32_t funding_vout{0};      //!< index of the OP_RETURN burn output
    uint32_t funding_key_index{0}; //!< registration funding derivation index
    CAmount funding_amount{0};
    //! Identity
    Identifier identity_id{};
    //! Username
    std::string label;             //!< as typed, e.g. "Alice"
    std::string normalized_label;  //!< homograph-safe
    std::array<uint8_t, 32> preorder_salt{};
    bool contested{false};
    //! Diagnostics
    std::string last_error;
    int64_t started_at{0};

    //! True for an identity seed-only recovery proved exists but found no
    //! username for. The flow parks here until the user picks a name, which
    //! is then registered on its own (no asset lock: the identity's existing
    //! credits pay for it).
    bool AwaitsUsername() const { return state == State::IDENTITY_CONFIRMED && label.empty(); }
};

std::vector<unsigned char> SerializeIdentityRecord(const IdentityRecord& record);
bool DeserializeIdentityRecord(const std::vector<unsigned char>& data, IdentityRecord& record);

//! "contact/pay-index/<id>" records: the next unused DIP-15 payment index
//! for a contact, as a 4-byte little-endian value. Decoding anything but a
//! well-formed record yields 0 (start of the chain).
std::vector<unsigned char> EncodePaymentCursor(uint32_t next_index);
uint32_t DecodePaymentCursor(const std::vector<unsigned char>& data);

//! Rebuild a contact's outbound payment cursor from wallet history: one past
//! the highest index in [0, window) whose derived payment script appears in
//! wallet_output_scripts, or 0 when none match. derive returns the payment
//! script for an index; nullopt ends the scan early.
uint32_t ComputePaymentCursor(uint32_t window,
                              const std::function<std::optional<CScript>(uint32_t)>& derive,
                              const std::set<CScript>& wallet_output_scripts);

} // namespace platform

#endif // BITCOIN_PLATFORM_WALLETRECORDS_H
