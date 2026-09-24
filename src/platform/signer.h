// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PLATFORM_SIGNER_H
#define BITCOIN_PLATFORM_SIGNER_H

#include <platform/types.h>
#include <span.h>
#include <wallet/platformtypes.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

class PlatformService;

namespace interfaces {
class Wallet;
} // namespace interfaces

namespace platform {

//! Keeps the wallet unlocked for as long as a SigningOperation lives. The Qt
//! layer wraps WalletModel::UnlockContext; this library never sees Qt.
class UnlockScope
{
public:
    virtual ~UnlockScope() = default;
};

enum class OperationKind : uint8_t {
    IDENTITY_CREATE,
    DPNS_PREORDER,
    DPNS_DOMAIN,
    PROFILE,
    CONTACT_REQUEST,
};

//! The bincode variant index a serialized StateTransition starts with, by
//! declaration order of rs-dpp's StateTransition enum (not the
//! StateTransitionType numbering). Pinned by dash-platform-cxx's
//! test_data/state_transition_first_byte.json.
inline constexpr uint8_t STATE_TRANSITION_VARIANT_BATCH{2};
inline constexpr uint8_t STATE_TRANSITION_VARIANT_IDENTITY_CREATE{3};

//! A user-initiated, unlocked, kind-scoped signing operation: the only thing
//! the state-transition builders accept. It is move-only and can only be
//! minted by PlatformService, so a builder cannot run outside an operation
//! the user started, and the wallet is relocked when the operation ends,
//! which the flows do before any network wait.
class SigningOperation
{
public:
    SigningOperation(SigningOperation&& other) noexcept;
    SigningOperation(const SigningOperation&) = delete;
    SigningOperation& operator=(const SigningOperation&) = delete;
    SigningOperation& operator=(SigningOperation&&) = delete;
    ~SigningOperation() = default;

    OperationKind kind() const { return m_kind; }
    interfaces::Wallet& wallet() const { return m_wallet; }
    //! The identity keys the operation may sign with.
    const std::vector<uint32_t>& keyIds() const { return m_key_ids; }
    bool allowsKey(uint32_t key_id) const;
    //! The identity key document transitions are signed with; unset for an
    //! identity registration, which signs with every key it registers.
    const std::optional<IdentityPublicKey>& documentKey() const { return m_document_key; }
    //! The asset-lock funding key of an identity registration, accepted for
    //! exactly one signature; unset for every other kind.
    const std::optional<wallet::RegistrationFundingKey>& fundingKey() const { return m_funding_key; }
    //! Claims the single asset-lock signature; false once it was claimed.
    bool claimAssetLockSignature() const;

private:
    friend class ::PlatformService;
    //! Test access to the constructor, as ConnmanTestMsg for CConnman.
    friend struct SigningOperationTestAccess;

    SigningOperation(interfaces::Wallet& wallet, OperationKind kind, std::vector<uint32_t> key_ids,
                     std::optional<IdentityPublicKey> document_key,
                     std::optional<wallet::RegistrationFundingKey> funding_key, std::unique_ptr<UnlockScope> unlock);

    interfaces::Wallet& m_wallet;
    OperationKind m_kind;
    std::vector<uint32_t> m_key_ids;
    std::optional<IdentityPublicKey> m_document_key;
    std::optional<wallet::RegistrationFundingKey> m_funding_key;
    mutable std::atomic_bool m_asset_lock_signed{false};
    std::unique_ptr<UnlockScope> m_unlock;
};

//! The wallet's answer to the SDK builders, bound to one operation. Signable
//! bytes come in and 65-byte compact recoverable signatures go out; the
//! double SHA256 is computed here, the operation kind is checked against the
//! first byte of the preimage, and keys outside the operation are refused.
//! Private keys never leave the wallet. Callable from any thread: the wallet
//! seams take cs_wallet, and the builders call it on the calling thread.
class WalletSigner
{
public:
    explicit WalletSigner(const SigningOperation& operation) :
        m_operation(operation)
    {
    }

    //! Signs the full signable preimage of a state transition with identity
    //! key key_id. Refuses a key outside the operation, a preimage whose
    //! variant byte does not match the operation's kind, or a locked wallet.
    bool signForKey(uint32_t key_id, Span<const uint8_t> signable, std::vector<uint8_t>& signature_out) const;

    //! Signs the asset-lock sighash with the operation's funding key, once.
    bool signAssetLockSighash(const std::array<uint8_t, 32>& sighash, std::vector<uint8_t>& signature_out) const;

private:
    const SigningOperation& m_operation;
};

} // namespace platform

#endif // BITCOIN_PLATFORM_SIGNER_H
