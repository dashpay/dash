// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_INVITATIONFLOW_H
#define BITCOIN_QT_PLATFORM_INVITATIONFLOW_H

#include <consensus/amount.h>
#include <platform/client.h>
#include <platform/types.h>
#include <primitives/transaction.h>
#include <qt/platform/identityflow.h>
#include <qt/walletmodel.h>
#include <support/allocators/secure.h>
#include <uint256.h>

#include <QObject>
#include <QString>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class PlatformService;
class UsernameFlow;

/**
 * DashPay invitations, as the mobile wallets make and claim them.
 *
 * Inviting: the wallet funds an asset lock paying a fresh invitation key
 * derived from its seed, the user confirms its amount and exact fee, and
 * once the node has the payment's InstantSend lock, or else its ChainLock,
 * the link is built and shown once. The link holds the invitation key in
 * plain text: it is a bearer voucher for the credits, so it is never stored
 * or logged. What is stored, before the payment is sent, is the invitation's
 * record (payment, amount and time), so a funded invitation whose link was
 * lost or never used can be reclaimed: its asset lock tops up the wallet's
 * own identity. The record ends once Platform reports the asset lock
 * consumed, by the reclaim or, as a reclaim finds, by the person invited.
 *
 * Claiming: a pasted link is read, its amount and whether it was used are
 * looked up, and once the user accepts, one SDK call creates the wallet's
 * identity from it, with the same keys a funded identity registers. Only a
 * wallet without an identity claims one.
 */
class InvitationFlow : public QObject
{
    Q_OBJECT

public:
    //! What an invitation is funded with, as DashPay iOS funded each one.
    static constexpr CAmount AMOUNT{1'000'000};
    //! The records of funded invitations, by invitation key index.
    static constexpr const char* RECORD_PREFIX{"dashpay/invitation/"};

    //! Show the link of a new invitation, once.
    using Show = std::function<void(const SecureString& link)>;
    //! Whether the user claims an invitation of `amount` duffs from `inviter`.
    using ConfirmClaim = std::function<bool(CAmount amount, const std::optional<platform::InviterInfo>& inviter)>;

    //! An invitation this wallet funded whose asset lock it has not reclaimed.
    struct Funded {
        uint32_t index{0};
        uint256 txid;
        CAmount amount{0};
        int64_t created{0}; //!< seconds since the epoch
    };

    enum class Step {
        IDLE,
        FUNDING,          //!< asking for the passphrase or the confirmation
        WAITING_FOR_LOCK, //!< committed; the node has no lock for it yet
        CHECKING,         //!< reading what a link would claim
        CLAIMING,
        RECLAIMING,
    };

    InvitationFlow(PlatformService& service, IdentityFlow& identity_flow, const UsernameFlow& username_flow,
                   IdentityFlow::Confirm confirm, Show show, ConfirmClaim confirm_claim);

    Step step() const { return m_step; }
    //! Why the last step failed; empty when it did not.
    const QString& error() const { return m_error; }
    //! Whether the last claim can be tried again with the same link.
    bool retryable() const { return m_step == Step::IDLE && m_invitation; }
    std::vector<Funded> funded() const;

    //! Fund an invitation from the wallet's identity and show its link.
    void create();
    //! Claim the invitation `link` into a new identity for the wallet.
    void claim(const SecureString& link);
    //! Claim the link of the last claim that can be tried again.
    void retry();
    //! Top up the wallet's identity with a funded invitation's asset lock.
    void reclaim(uint32_t index);

Q_SIGNALS:
    void changed();

private:
    void serviceChanged();
    void send();
    void check();
    void onChecked(platform::Result<platform::InvitationClaim> result);
    void onClaimed(platform::Result<platform::Identity> result);
    void onReclaimed(uint32_t index, platform::Result<uint64_t> result);
    //! The invitation's asset lock was consumed: say so, and look the
    //! identity up again.
    void claimedAlready();
    //! Ask for the passphrase; nullopt (and IDLE again) if the wallet stays
    //! locked, another flow started meanwhile or a gate closed.
    std::optional<WalletModel::UnlockContext> unlock();
    //! Back to IDLE; a retryable claim keeps its link for retry().
    void finish(const QString& error = {}, bool retryable = false);

    PlatformService& m_service;
    IdentityFlow& m_identity_flow;
    const UsernameFlow& m_username_flow;
    const IdentityFlow::Confirm m_confirm;
    const Show m_show;
    const ConfirmClaim m_confirm_claim;
    Step m_step{Step::IDLE};
    QString m_error;
    //! The one call in flight, until it calls back or is dropped.
    std::weak_ptr<void> m_call;
    //! The invitation being funded, and the unlock its key is exported under.
    std::optional<uint32_t> m_index;
    std::string m_inviter;
    CTransactionRef m_tx;
    std::optional<WalletModel::UnlockContext> m_unlock;
    //! The link being claimed, kept parsed (its key inside the SDK) while
    //! the claim can be tried again.
    std::shared_ptr<const platform::Invitation> m_invitation;
};

#endif // BITCOIN_QT_PLATFORM_INVITATIONFLOW_H
