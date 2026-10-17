// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_RECOVERYFLOW_H
#define BITCOIN_QT_PLATFORM_RECOVERYFLOW_H

#include <platform/types.h>
#include <qt/walletmodel.h>
#include <uint256.h>

#include <QObject>
#include <QString>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>

class ContactFlow;
class IdentityFlow;
class PlatformService;
class UsernameFlow;

QT_BEGIN_NAMESPACE
class QThread;
QT_END_NAMESPACE

/**
 * Recovers the wallet's DashPay state from its seed, as the mobile wallets
 * do on every load: the identities found by their MASTER keys; the selected
 * one's usernames, a username still waiting for its masternode vote, and
 * contacts; and the contacts of every identity, whose payments to us the
 * wallet must see. Nothing of it is kept but what the wallet itself needs.
 *
 * Our receiving chain of a contact made elsewhere (another app on this seed,
 * or before the wallet was restored) is imported, born when our request to
 * them was created. Payments to it are already in blocks the wallet has
 * scanned, so it owes a rescan from that time, recorded before the chain is
 * imported and erased once a rescan has succeeded; a wallet loaded while it
 * is owed rescans. A node that pruned the blocks the rescan reads refuses it.
 */
class RecoveryFlow : public QObject
{
    Q_OBJECT

public:
    enum class Step {
        IDLE,
        FINDING, //!< from the passphrase until the contacts are read
    };
    RecoveryFlow(PlatformService& service, IdentityFlow& identity_flow, UsernameFlow& username_flow,
                 ContactFlow& contact_flow);
    ~RecoveryFlow() override;

    Step step() const { return m_step; }
    //! Whether the identity's pending username was read, or failed to be.
    bool pendingRead() const { return m_pending_read; }
    //! A username of the identity still waiting for its masternode vote.
    const std::optional<std::string>& pendingName() const { return m_pending_name; }
    bool rescanning() const { return m_rescan != nullptr; }
    //! Where the owed rescan would start, when this node has pruned blocks from there on.
    const std::optional<int>& prunedFrom() const { return m_pruned_from; }
    //! What the recovery or its rescan last failed at; empty when neither did.
    const QString& error() const { return m_error; }

    //! The first DashPay click on a restored wallet: start DashPay, then find
    //! the identity and its contacts under one passphrase.
    void find();

Q_SIGNALS:
    void changed();

private:
    void update();
    //! Give the passphrase back once the identities are looked up and their
    //! contacts are read, or can't be.
    void found();
    void readPendingName();
    //! Read our requests from the identities not selected.
    void readOthers();
    //! Import our receiving chain towards each contact an identity sent a
    //! request to, where the wallet has none.
    void ensureKeychains();
    void rescan();
    void finish(const QString& error = {});

    PlatformService& m_service;
    IdentityFlow& m_identity_flow;
    UsernameFlow& m_username_flow;
    ContactFlow& m_contact_flow;
    Step m_step{Step::IDLE};
    std::optional<WalletModel::UnlockContext> m_unlock;
    std::weak_ptr<void> m_pending_call;
    bool m_pending_read{false};
    std::optional<std::string> m_pending_name;
    //! Our requests from each identity not selected: by recipient, when the
    //! newest was created, in seconds.
    std::map<platform::Identifier, std::map<platform::Identifier, int64_t>> m_others_sent;
    std::map<platform::Identifier, std::weak_ptr<void>> m_others_calls;
    //! Our identity and contact of each chain looked for since the identity
    //! was found.
    std::set<std::pair<uint256, uint256>> m_ensured;
    QThread* m_rescan{nullptr};
    //! A chain was imported while the rescan ran, which scanned blocks without it.
    bool m_owed_again{false};
    std::optional<int> m_pruned_from;
    QString m_error;
};

#endif // BITCOIN_QT_PLATFORM_RECOVERYFLOW_H
