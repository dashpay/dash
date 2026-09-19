// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_IDENTITYFLOW_H
#define BITCOIN_QT_PLATFORM_IDENTITYFLOW_H

#include <consensus/amount.h>
#include <platform/types.h>
#include <platform/walletrecords.h>
#include <uint256.h>

#include <QObject>

#include <array>
#include <cstdint>
#include <memory>
#include <string>

class PlatformService;

/**
 * Persistent, resumable state machine driving identity creation and DPNS
 * username registration:
 *
 *   NONE -> FUNDING_SENT -> FUNDING_LOCKED -> IDENTITY_BROADCAST
 *        -> IDENTITY_CONFIRMED -> PREORDER_BROADCAST -> PREORDER_WAIT
 *        -> DOMAIN_BROADCAST -> { REGISTERED | CONTESTED_PENDING -> ... }
 *
 * Every mutation of the persisted record happens BEFORE the corresponding
 * irreversible action (in particular the preorder salt is stored before the
 * preorder is broadcast). advance() is idempotent and event-driven; on
 * restart the flow re-derives ground truth (wallet tx status, proved
 * platform queries) instead of trusting the recorded step blindly.
 *
 * The record is stored in the wallet DB under the "identity/0" platform
 * data key (single identity per wallet in this version).
 */
class IdentityFlow : public QObject
{
    Q_OBJECT

public:
    //! Persisted snapshot (platform::IdentityRecord, serialized into the
    //! wallet DB). The type lives outside Qt so seed-only recovery and unit
    //! tests share the exact serialization.
    using Record = platform::IdentityRecord;
    using State = Record::State;

    explicit IdentityFlow(PlatformService& service, QObject* parent = nullptr);

    const Record& record() const { return m_record; }
    bool active() const
    {
        return m_record.state != State::NONE && m_record.state != State::REGISTERED &&
               m_record.state != State::FAILED;
    }

    //! Begin a new registration. Creates+commits the asset lock transaction
    //! and persists the initial record. Returns false with error set when the
    //! flow is already active or the wallet refuses. For an identity restored
    //! without a name (seed-only recovery leaves it in IDENTITY_CONFIRMED
    //! with an empty label), no asset lock is created: the name registration
    //! is funded by the identity's existing credits.
    bool start(const QString& label, CAmount funding_amount, QString& error);

    //! Re-read the persisted record after an external writer (seed-only
    //! recovery) replaced it, and notify the GUI.
    void reload();

    //! Drive the state machine one step if possible. Safe to call at any
    //! time (timer ticks, islock signals, client callbacks).
    void advance();

    //! Re-derive progress from ground truth after a restart, then advance.
    void resume();

    //! Clear a FAILED flow so the user can retry from scratch. Never clears
    //! an in-progress flow.
    void reset();

Q_SIGNALS:
    void stateChanged();
    void failed(const QString& step, const QString& error);

private:
    void setState(State state);
    void fail(const QString& step, const QString& error, bool retryable);
    bool load();
    //! Persists the current record. Returns false if the wallet DB write
    //! failed (the caller must not proceed with an irreversible step).
    bool save();

    void checkFundingLock();
    void broadcastIdentityCreate();
    void confirmIdentity();
    void broadcastPreorder();
    void broadcastDomain();
    void confirmDomain();
    void checkContestedOutcome();

    PlatformService& m_service;
    Record m_record;
    bool m_step_in_flight{false};
    int m_retries{0};
    //! Consecutive checkFundingLock() polls that found the funding tx
    //! unconfirmed, outside the mempool and refusing rebroadcast.
    int m_funding_dead_polls{0};
};

#endif // BITCOIN_QT_PLATFORM_IDENTITYFLOW_H
