// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_USERNAMEFLOW_H
#define BITCOIN_QT_PLATFORM_USERNAMEFLOW_H

#include <platform/types.h>
#include <qt/walletmodel.h>

#include <QObject>
#include <QString>

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

class IdentityFlow;
class PlatformService;

/**
 * Registers the DashPay username of the wallet's identity, as the mobile
 * wallets do: a label DPNS accepts and that resolves to nobody is registered
 * with one SDK call, signed with the identity's HIGH authentication key. A
 * wallet without an identity creates it first through IdentityFlow, under
 * the same unlock.
 *
 * A premium (contested) label is registered, as the mobile wallets do,
 * only once the user has accepted that its contest fund is never refunded,
 * won or lost. The name is the identity's once the masternodes award it;
 * until then it is a pending name, whose vote can be read.
 *
 * Nothing is persisted: the identity's names, read from Platform whenever
 * the identity is found, say whether a registration whose answer was lost
 * landed.
 */
class UsernameFlow : public QObject
{
    Q_OBJECT

public:
    enum class Step {
        IDLE,
        CHECKING,  //!< resolving the label
        PREPARING, //!< unlocked; waiting for the identity being created, or for a client
        REGISTERING,
    };

    //! The contest fund in credits a premium username pays into its vote,
    //! as the mobile wallets state it.
    static constexpr uint64_t CONTEST_FUND{20'000'000'000};
    //! What a new identity is funded with to register a premium username.
    static constexpr CAmount PREMIUM_REGISTRATION_AMOUNT{25'000'000};
    //! The credits an existing identity is topped up to before registering
    //! one: the fund and 0.01 DASH, which also pays the top-up's own fee.
    static constexpr uint64_t PREMIUM_CREDITS{CONTEST_FUND + 1'000'000'000};

    //! Whether the user pays CONTEST_FUND, never refunded, to join the
    //! masternode vote on a premium username.
    using ConfirmContest = std::function<bool()>;

    UsernameFlow(PlatformService& service, IdentityFlow& identity_flow, ConfirmContest confirm_contest);

    Step step() const { return m_step; }
    //! Whether the identity's names were read since it was found.
    bool namesRead() const { return m_names_read; }
    //! The identity's username; nullopt when it has none or they weren't read.
    const std::optional<std::string>& username() const { return m_username; }
    //! Why the last registration or contest read failed; empty when neither did.
    const QString& error() const { return m_error; }
    //! The vote last read with readContest().
    const std::optional<platform::VoteState>& contest() const { return m_contest; }

    //! Register `label` for the wallet's identity, creating the identity
    //! first if the wallet has none.
    void registerName(const std::string& label);
    //! Read the masternode vote on a pending premium username.
    void readContest(const std::string& label);

Q_SIGNALS:
    void changed();

private:
    void update();
    void readNames();
    void onResolved(platform::Result<platform::Identifier> result);
    void send(const platform::Identity& identity);
    void onRegistered(platform::Result<platform::Identifier> result);
    void resolve();
    void finish(const QString& error = {});

    PlatformService& m_service;
    IdentityFlow& m_identity_flow;
    const ConfirmContest m_confirm_contest;
    Step m_step{Step::IDLE};
    std::string m_label;
    //! Taken once the label is free, and held until the name is sent: a new
    //! identity is funded and the name signed under it.
    std::optional<WalletModel::UnlockContext> m_unlock;
    //! The balance of an identity being topped up for a premium label: the
    //! name is sent once the balance has grown.
    std::optional<uint64_t> m_short_balance;
    //! The step's call and the names read, until they call back or are dropped.
    std::weak_ptr<void> m_call;
    std::weak_ptr<void> m_names_call;
    std::weak_ptr<void> m_contest_call;
    bool m_names_read{false};
    std::optional<std::string> m_username;
    std::optional<platform::VoteState> m_contest;
    QString m_error;
};

#endif // BITCOIN_QT_PLATFORM_USERNAMEFLOW_H
