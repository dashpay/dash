// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_PLATFORMRECOVERY_H
#define BITCOIN_QT_PLATFORM_PLATFORMRECOVERY_H

#include <platform/types.h>
#include <qt/platform/platformui.h>
#include <qt/walletmodel.h>
#include <wallet/platformtypes.h>

#include <QObject>
#include <QString>

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

class PlatformService;

QT_BEGIN_NAMESPACE
class QThread;
class QTimer;
QT_END_NAMESPACE

/**
 * Seed-only recovery of Dash Platform state. A wallet restored from its
 * recovery phrase re-derives every private key but has none of the local
 * platform records. When no identity record exists, this probe rebuilds
 * them from proved Platform queries:
 *
 *   1. scan MASTER auth keys per identity index (gap limit of consecutive
 *      proven-absent indexes) via getIdentityByPublicKeyHash;
 *   2. synthesize the identity record: REGISTERED when namesOfIdentity
 *      proves a username, IDENTITY_CONFIRMED (no label) otherwise so the
 *      GUI offers name registration; the record names the identity's keys
 *      this wallet signs and runs ECDH with, each checked against the key
 *      the wallet derives at that id, so an identity registered by another
 *      wallet from the same seed (a different key layout) still works, and
 *      one whose keys this wallet does not hold is reported, not restored;
 *   3. remember every request this identity sent, answered or not, and
 *      restore established contacts (a request proved in both directions),
 *      decrypting their xpub from each contact's newest incoming request and
 *      re-importing the friendship keychains;
 *   4. rebuild per-contact outbound payment cursors from wallet history,
 *      labelling the payments found as payments to the contact, and start a
 *      wallet rescan so pre-restore inbound payments appear.
 *
 * Only a proven absence counts toward the gap: an unanswered probe ends the
 * session "incomplete" and the next start retries. The contact phase is
 * owed until it completes: a "platform/recovery-pending" record marks an
 * identity whose contacts were not (all) restored, and the next start
 * resumes there. Every Platform read is one page; the probe paces itself
 * one request per step and never blocks the wallet.
 *
 * Until the probe has proved that the seed has no identity, no new
 * registration may start: it would burn an asset lock on an identity
 * Platform refuses as a duplicate of the existing one.
 */
class PlatformRecovery : public QObject
{
    Q_OBJECT

public:
    //! What the probe has concluded about the seed so far.
    enum class Outcome {
        PENDING,      //!< not run to a conclusion yet
        NEEDS_UNLOCK, //!< the wallet must be unlocked before it can run
        NO_IDENTITY,  //!< proven: no identity of this seed on Platform
        RESTORED,     //!< an identity record exists (found, or already local)
        UNUSABLE,     //!< an identity exists but carries no key this wallet holds
        //! the last run could not get an answer; lastError() says why and
        //! the next start (a context refresh, an unlock, the user) retries
        FAILED,
    };

    explicit PlatformRecovery(PlatformService& service, QObject* parent = nullptr);
    ~PlatformRecovery() override;

    //! Begin (or resume) the probe. PlatformService calls this after every
    //! node-context refresh and wallet unlock; it is a no-op while a run is
    //! in progress or once nothing is owed.
    void maybeStart();
    //! maybeStart() on a locked wallet the user asked to unlock for it: the
    //! wallet stays unlocked for the scan only and is locked again when the
    //! scan ends or after two minutes, whichever comes first; a scan still
    //! running then leaves what needs the keys for the next unlock. Nothing
    //! starts when the user declines.
    void unlockAndStart();
    //! The service stopped: the scan's unlock is released and a rescan still
    //! running is aborted and waited for, while the wallet still exists.
    void stop();

    Outcome outcome() const { return m_outcome; }
    //! A run is in progress.
    bool running() const { return m_running; }
    //! Why the last run failed (Outcome::FAILED).
    const PlatformUi::UserError& lastError() const { return m_last_error; }
    //! Why a new identity registration may not start yet; empty when it may.
    QString registrationBlocker() const;

Q_SIGNALS:
    //! Emitted on the GUI thread whenever outcome() or running() changes,
    //! also when a start concludes without running (a locked wallet).
    void stateChanged();
    //! Emitted on the GUI thread when a run ends. recovered is true when an
    //! identity record exists at that point.
    void finished(bool recovered);

private:
    struct RestoredContact {
        platform::Identifier identity{};
        wallet::CompactXpub xpub{};
    };

    void probeIndex(uint32_t index, uint32_t consecutive_absent);
    void restoreIdentity();
    void collectNames(const platform::Identifier& start_after, std::vector<platform::DpnsName> names);
    void collectRequests(bool to_me, const platform::Identifier& start_after,
                         std::vector<platform::ContactRequest> requests);
    void restoreNextContact();
    void rebuildPayCursors();
    void startRescan();
    //! Ends the run. The contact phase stays owed (the pending record is
    //! kept and a later start resumes it) unless every contact is settled.
    void finish(bool recovered, const std::string& outcome);
    //! Ends a run that could not get an answer for `status`.
    void fail(const platform::Status& status, const std::string& outcome);
    void reset();
    //! Change outcome() or running(), emitting stateChanged().
    void setOutcome(Outcome outcome);
    void setRunning(bool running);
    //! The wallet cannot derive the identity's keys right now.
    bool walletLocked() const;

    PlatformService& m_service;
    bool m_running{false};
    Outcome m_outcome{Outcome::PENDING};
    PlatformUi::UserError m_last_error;
    //! First identity the scan proved to exist (the one recovered).
    std::optional<uint32_t> m_found_index;
    platform::Identity m_identity;
    uint32_t m_extra_identities{0};
    //! Hash160 of the MASTER key of each identity index the scan may probe,
    //! derived when the run starts.
    std::vector<std::array<uint8_t, 20>> m_probe_hashes;
    std::vector<platform::ContactRequest> m_incoming;
    std::vector<platform::ContactRequest> m_outgoing;
    //! Established contacts pending restore (their incoming request).
    std::vector<platform::ContactRequest> m_established;
    std::vector<RestoredContact> m_restored;
    //! The unlock unlockAndStart() holds until the run ends or
    //! m_unlock_timer fires.
    std::optional<WalletModel::UnlockContext> m_unlock;
    QTimer* m_unlock_timer{nullptr};
    //! A contact was skipped for a reason a later run may resolve.
    bool m_contacts_incomplete{false};
    QThread* m_rescan_thread{nullptr};
};

#endif // BITCOIN_QT_PLATFORM_PLATFORMRECOVERY_H
