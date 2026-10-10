// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_IDENTITYFLOW_H
#define BITCOIN_QT_PLATFORM_IDENTITYFLOW_H

#include <consensus/amount.h>
#include <platform/signer.h>
#include <platform/types.h>
#include <platform/walletrecords.h>
#include <qt/platform/platformui.h>
#include <qt/walletmodel.h>
#include <uint256.h>
#include <wallet/platformtypes.h>

#include <QObject>
#include <QString>

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <vector>

#include <util/result.h>

class PlatformService;

QT_BEGIN_NAMESPACE
class QTimer;
QT_END_NAMESPACE

/**
 * Persistent, resumable state machine driving identity creation and DPNS
 * username registration:
 *
 *   NONE -> FUNDING_SENT -> FUNDING_LOCKED -> IDENTITY_BROADCAST
 *        -> IDENTITY_CONFIRMED -> PREORDER_BROADCAST -> PREORDER_WAIT
 *        -> DOMAIN_BROADCAST -> { REGISTERED | CONTESTED_PENDING -> ... }
 *
 * IDENTITY_CONFIRMED resolves the name before it preorders: a name that
 * is already this identity's goes straight to REGISTERED.
 *
 * Every mutation of the persisted record happens BEFORE the corresponding
 * irreversible action (the preorder salt is stored before the preorder is
 * broadcast). advance() is idempotent and event-driven; on restart the flow
 * re-derives ground truth (wallet tx status, proved platform queries)
 * instead of trusting the recorded step blindly.
 *
 * Failures are graded: an unanswered or unverified read or broadcast keeps
 * the state and is retried on the next tick for as long as it takes (a
 * network outage never fails a registration), a builder or key error is
 * retried a bounded number of times, and a consensus refusal the flow
 * cannot steer on ends in FAILED. FAILED never discards what is on chain:
 * reset() returns to the funding or the confirmed identity so "try again"
 * re-uses the asset lock or the identity's credits instead of burning more.
 *
 * A registration signs once and broadcasts in sequence. The passphrase the
 * user enters to fund the identity keeps the wallet unlocked while the
 * funding payment gets its InstantSend lock (at most a minute, usually a
 * few seconds); the identity-create, username preorder and domain and, when
 * the user chose a display name, the DashPay profile are then signed in one
 * go and persisted in the record before the wallet is locked again. A brand
 * new identity has used no contract, so its first DPNS nonces are 1 and 2
 * and its first DashPay nonce is 1. Each step then broadcasts its persisted
 * transition and is confirmed by a proved re-query before the next one goes
 * out; the domain waits until Platform has taken the preorder's nonce. A
 * step whose transition is missing (the lock did not come in time, a record
 * of an earlier layout, a transition refused as stale) or was built under
 * another protocol version than Platform now runs signs when it gets there,
 * asking for the passphrase once for everything it signs, and parks in
 * NEEDS_UNLOCK when the user declines; a profile built under another
 * protocol version is left for the user to add from the dashboard. Confirmations are polled with a
 * backoff for a few minutes before anything is broadcast again. While the
 * service refuses writes (protocol version ahead of this build) or the
 * network changed, signing steps wait without changing state.
 *
 * The record is stored in the wallet DB under platform::records::IDENTITY
 * (single identity per wallet in this version).
 */
class IdentityFlow : public QObject
{
    Q_OBJECT

public:
    using Record = platform::IdentityRecord;
    using State = Record::State;

    //! One identity key registered at creation.
    struct KeySpec {
        uint32_t id;
        platform::IdentityPublicKey::Purpose purpose;
        platform::IdentityPublicKey::SecurityLevel security_level;
        //! Bound to the DashPay contactRequest document type.
        bool contact_request_bound;
    };
    //! The keys every identity registers: 0 AUTH/MASTER, 1 AUTH/HIGH (the
    //! key documents are signed with), 2 ENCRYPTION/MEDIUM and
    //! 3 DECRYPTION/MEDIUM (the contact-request pair), the last two bound to
    //! DashPay contactRequest documents as the mobile wallets register them.
    static const std::vector<KeySpec>& RegistrationKeys();

    explicit IdentityFlow(PlatformService& service, QObject* parent = nullptr);

    const Record& record() const { return m_record; }
    bool active() const
    {
        return m_record.state != State::NONE && m_record.state != State::REGISTERED && m_record.state != State::FAILED;
    }
    //! The registered identity, once it is confirmed on Platform (also after
    //! a failed name registration, which leaves it there).
    std::optional<platform::Identifier> identityId() const;
    //! An asset lock is (or may be) on chain that no identity consumed yet:
    //! the registration is funding or creating the identity, waits for the
    //! passphrase there, or failed there and "try again" re-uses the lock.
    //! The record is its only trace (seed recovery finds identities, not
    //! asset locks), so wiping the records now would strand the payment.
    static bool HoldsUnconsumedFunding(const Record& record);
    bool holdsUnconsumedFunding() const { return HoldsUnconsumedFunding(m_record); }
    //! Why the last attempt failed, worded when it is shown so a record
    //! written by an earlier build reads like a new one, and the raw result
    //! for "Show details". Empty when nothing failed.
    QString lastErrorText() const;
    QString lastErrorDetails() const;

    //! Begin a new registration. Creates+commits the asset lock transaction
    //! and persists the initial record; the wallet stays unlocked until the
    //! funding payment is locked and the registration is signed, or a minute
    //! has passed. display_name, when set, is published as the DashPay
    //! profile once the username is registered. Returns false with error set
    //! when the flow is already active or the wallet refuses. For an
    //! identity without a name (seed-only recovery or an earlier
    //! registration whose name step failed leave it in IDENTITY_CONFIRMED
    //! with an empty label), no asset lock is created: the name registration
    //! is funded by the identity's existing credits.
    //!
    //! A new identity is funded only right after a proved lookup showed
    //! that none is registered under the MASTER key it would register (a
    //! wallet restored from its recovery phrase, or one that turned DashPay
    //! off and on again, has no record of the one it may have). Until then
    //! start() returns false: it begins the lookup, keeping the passphrase
    //! the user just entered for the call that funds, and
    //! existingIdentity() says how the lookup stands.
    bool start(const QString& label, CAmount funding_amount, QString& error, const QString& display_name = {});

    //! What the lookup a new registration makes before funding has found.
    enum class ExistingIdentity {
        UNKNOWN,    //!< not looked up, or its answer was used by a registration
        CHECKING,   //!< the lookup is running
        NONE,       //!< proven: no identity; the next start() funds a new one
        FOUND,      //!< proven: this wallet has an identity; nothing is funded
        UNVERIFIED, //!< no proved answer; nothing is funded, start() looks again
    };
    ExistingIdentity existingIdentity() const { return m_existing_identity; }
    //! Why the lookup does not let a new identity be funded (FOUND or
    //! UNVERIFIED); empty otherwise.
    const PlatformUi::UserError& existingIdentityError() const { return m_existing_identity_error; }

    //! Re-read the persisted record after an external writer (seed-only
    //! recovery, a wipe) replaced or removed it, and notify the GUI.
    void reload();

    //! Drive the state machine one step if possible. Safe to call at any
    //! time (timer ticks, client callbacks); a flow parked in NEEDS_UNLOCK
    //! only moves through retryAfterUnlock(), and a step waiting for a
    //! confirmation only polls when its backoff allows.
    void advance();

    //! A DashPay profile chosen during registration is still being published.
    bool profilePending() const { return !m_record.signed_profile.empty(); }
    //! The profile chosen during registration is being published or was
    //! published; false once it was given up (the user adds it instead).
    bool profileChosen() const { return !m_record.profile_display_name.empty(); }

    //! The user acted (opened the wizard, unlocked the wallet): a flow parked
    //! in NEEDS_UNLOCK asks for the unlock again.
    void retryAfterUnlock();

    //! Where a funding payment that did not get its lock stands.
    enum class FundingWait {
        NONE,     //!< not funding, or the payment is on its way to a lock
        RELEASED, //!< the network refused it and its coins were released
        ENDING,   //!< a payment that spends its coins is pending or not final
    };
    FundingWait fundingWait() const;
    //! Offered in FundingWait::RELEASED only: send the funding payment's
    //! coins back to this wallet, less the fee, which ends the registration
    //! once it is final unless the funding payment confirms first. Asks for
    //! the passphrase. Returns false with error set when nothing was sent.
    bool releaseFunding(QString& error);

    //! Leave FAILED so the user can try again, keeping whatever the failed
    //! attempt put on chain: a confirmed identity waits for a new name, an
    //! unconsumed asset lock is re-used, and only a registration that never
    //! funded anything starts over from NONE.
    void reset();

Q_SIGNALS:
    void stateChanged();
    void failed(const QString& step, const QString& error, const QString& details);
    //! The lookup for an existing identity ended; see existingIdentity().
    void existingIdentityChecked();

private:
    //! How a step failure is treated.
    enum class Severity {
        TRANSIENT, //!< keep the state, retry on the next tick without limit
        RETRYABLE, //!< keep the state, FAILED after a bounded number of tries
        FATAL,     //!< FAILED now
    };

    //! How a failed read or broadcast of that status is treated.
    static Severity SeverityOf(const platform::Status& status);

    bool setState(State state);
    void fail(const QString& step, const QString& error, Severity severity, const platform::Status* status = nullptr);
    //! fail() with the plain-language text for a failed client call.
    void fail(const QString& step, const platform::Status& status, PlatformUi::Context context, Severity severity);
    //! Why the name was lost to another identity: registered, or won in a vote.
    QString takenText() const;
    QString lostVoteText() const;
    bool load();
    //! Persists the current record. Returns false if the wallet DB write
    //! failed (the caller must not proceed with an irreversible step).
    bool save();
    //! Deletes the persisted record.
    void eraseRecord();
    //! Park the current step until the user unlocks the wallet.
    void needUnlock(State resume_state);
    //! Unlock the wallet once for everything a signing step signs: the
    //! operations minted while `unlock` lives do not ask again, nor do they
    //! while the registration's own unlock is held. Parks in NEEDS_UNLOCK
    //! (resuming at resume_state) when the user declines, waits in place
    //! while the service refuses writes.
    bool unlockForStep(const QString& step, State resume_state, std::optional<WalletModel::UnlockContext>& unlock);
    //! Sign one document transition with key inside an unlock scope.
    util::Result<platform::Built> signDocument(
        platform::OperationKind kind, const platform::IdentityPublicKey& key,
        const std::function<util::Result<platform::Built>(const platform::SigningOperation&)>& build);
    //! Sign the username preorder at nonce and its domain at nonce + 1 into
    //! the record (not saved yet).
    bool signUsername(const platform::IdentityPublicKey& key, uint64_t nonce, std::string& error);
    //! Sign the domain alone at nonce into the record (not saved yet).
    bool signDomain(const platform::IdentityPublicKey& key, uint64_t nonce, std::string& error);
    //! Sign the profile a new identity publishes, at its first DashPay
    //! nonce; a failure only leaves the profile for the user to add.
    void signProfile(const platform::IdentityPublicKey& key);
    //! Lock the wallet again after a registration's funding wait.
    void releaseRegistrationUnlock();
    //! Whether the lookup proved that no identity is registered under
    //! master_key_hash; the answer is used up by the registration it lets
    //! fund. Otherwise error says why not, and a lookup is started unless
    //! it found one or is running; `unlock` is kept for the start() that
    //! funds once it proves there is none.
    bool noExistingIdentity(const std::array<uint8_t, 20>& master_key_hash, WalletModel::UnlockContext& unlock,
                            QString& error);
    //! Whether a transition signed ahead was built under the protocol
    //! version Platform runs now (or either is not known).
    bool signedForCurrentVersion(const Record::SignedTransition& signed_transition) const;

    //! The backoff of a step waiting for Platform: whether it may poll now,
    //! when it polls next after an unconfirmed answer, and whether it has
    //! waited long enough to broadcast again.
    bool pollDue() const;
    void schedulePoll();
    bool waitExpired() const;
    //! Start a new wait (after a broadcast) with the first poll interval.
    void restartWait();
    //! Whether a signed transition may be broadcast now; fails the step in
    //! place with the reason otherwise.
    bool sendAllowed(const QString& step);

    void checkFundingLock();
    //! Notify the GUI when fundingWait() changed since it last looked.
    void updateFundingWait();
    void broadcastIdentityCreate();
    void sendIdentityCreate();
    void confirmIdentity();
    //! Proved resolve of the name before the preorder: adopts a name this
    //! identity already owns, fails for one another identity owns.
    void checkName();
    //! Reads the proved identity and its DPNS contract nonce, then builds the
    //! document step while the record is still at `step`.
    void prepareDocumentStep(State step,
                             void (IdentityFlow::*build)(const platform::IdentityPublicKey& key, uint64_t nonce));
    //! Sign the username if it is not signed yet, then send the preorder.
    void broadcastPreorder(const platform::IdentityPublicKey& key, uint64_t nonce);
    //! signed_ahead: the preorder was not signed by this step, so a refusal
    //! of its document id means it was built before Platform upgraded.
    void sendPreorder(bool signed_ahead);
    //! Proved re-query of the preorder: Platform has taken its nonce.
    void confirmPreorder();
    //! With the next DPNS nonce read: send the signed domain, or sign it
    //! first when it is missing (a record of an earlier layout) or its nonce
    //! went to a domain Platform did not apply.
    void broadcastDomain(const platform::IdentityPublicKey& key, uint64_t next_nonce);
    void sendDomain(bool signed_ahead);
    void confirmDomain();
    void checkContestedOutcome();
    //! Publish the profile signed during registration, then confirm it.
    void publishProfile();
    void confirmProfile();
    //! Stop publishing the signed profile: confirmed (empty reason), or
    //! given up for reason, which leaves it for the user to add.
    void dropSignedProfile(const std::string& reason);
    //! REGISTERED, dropping the username transitions it no longer needs.
    void finishRegistration();
    //! Broadcasts a signed transition and reports the typed outcome.
    void broadcast(const QString& step, const std::vector<uint8_t>& bytes,
                   std::function<void(const platform::Status&)> on_result);

    PlatformService& m_service;
    Record m_record;
    //! The record as last read or written.
    std::vector<unsigned char> m_saved_record;
    //! A client request of the current step is outstanding.
    bool m_step_in_flight{false};
    //! A signing step is running on the GUI thread; the passphrase dialog
    //! it may open spins a nested event loop, so ticks must not re-enter.
    bool m_signing{false};
    int m_retries{0};
    //! Since when checkFundingLock() finds the funding tx unconfirmed,
    //! outside the mempool and refusing rebroadcast (unix seconds, 0 if not).
    int64_t m_funding_dead_since{0};
    FundingWait m_funding_wait{FundingWait::NONE};
    //! The current step's wait for Platform (unix seconds): when it began,
    //! when it may poll next, and the interval after that.
    int64_t m_wait_started{0};
    int64_t m_next_poll{0};
    int64_t m_poll_interval{0};
    //! The unlock the user gave to fund the registration, held until the
    //! funding lock arrives and the registration is signed, or until
    //! m_registration_unlock_timer fires, while m_funding_lock_poll drives
    //! the flow every second.
    std::optional<WalletModel::UnlockContext> m_registration_unlock;
    QTimer* m_registration_unlock_timer{nullptr};
    QTimer* m_funding_lock_poll{nullptr};
    //! Platform refused the InstantSend proof's signature (the signing
    //! quorum rotated out): the next attempt waits for the ChainLock proof.
    bool m_prefer_chain_proof{false};
    //! The signed identity-create went out this session; a registration
    //! resumed after a restart sends it again before it checks for it.
    bool m_identity_create_sent{false};
    //! The preorder went out this session; after a restart it is sent
    //! again before the flow waits for it.
    bool m_preorder_sent{false};
    //! Domains signed again after theirs was spent without registering the
    //! name; bounded before the whole username is signed again.
    int m_domain_resigns{0};
    //! The profile signed during registration was accepted for broadcast
    //! this session and is being confirmed, and how often it was sent.
    bool m_profile_sent{false};
    int m_profile_sends{0};
    //! The lookup for an identity registered under m_existing_identity_key
    //! (the Hash160 of this wallet's identity MASTER key).
    ExistingIdentity m_existing_identity{ExistingIdentity::UNKNOWN};
    std::array<uint8_t, 20> m_existing_identity_key{};
    //! When it answered (unix seconds): a proven absence funds only while
    //! it is as fresh as the passphrase the lookup kept.
    int64_t m_existing_identity_time{0};
    PlatformUi::UserError m_existing_identity_error;
};

#endif // BITCOIN_QT_PLATFORM_IDENTITYFLOW_H
