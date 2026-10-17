// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_PLATFORMPAGE_H
#define BITCOIN_QT_PLATFORM_PLATFORMPAGE_H

#include <consensus/amount.h>
#include <qt/platform/platformservice.h>

#include <QWidget>

#include <memory>
#include <optional>

class BitcoinAmountField;
class ContactFlow;
class ContactsPage;
class IdentityFlow;
class InvitationFlow;
class RecoveryFlow;
class UsernameFlow;

QT_BEGIN_NAMESPACE
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
QT_END_NAMESPACE

/** The DashPay tab of one wallet. Only built with --enable-platform-gui.
 *
 *  It owns the wallet's PlatformService while both models are set, offers
 *  the first DashPay click (Find my identity) with a one-line privacy notice,
 *  and says why DashPay can't run when a gate is closed. Once DashPay runs it
 *  recovers and shows the wallet's identity, its username and its balance;
 *  creates, continues or tops up the identity; registers its username, a
 *  premium one through the masternode vote, whose tally it shows;
 *  keeps its contacts, handing a payment to one to the Send tab; and makes,
 *  claims and reclaims invitations. */
class PlatformPage : public QWidget
{
    Q_OBJECT

public:
    explicit PlatformPage(PlatformService::Sources sources = {}, QWidget* parent = nullptr);
    ~PlatformPage() override;

    void setWalletModel(WalletModel* wallet_model);
    void setClientModel(ClientModel* client_model);
    PlatformService* service() const { return m_service.get(); }
    IdentityFlow* identityFlow() const { return m_identity_flow.get(); }
    UsernameFlow* usernameFlow() const { return m_username_flow.get(); }
    ContactFlow* contactFlow() const { return m_contact_flow.get(); }
    RecoveryFlow* recoveryFlow() const { return m_recovery_flow.get(); }
    InvitationFlow* invitationFlow() const { return m_invitation_flow.get(); }

Q_SIGNALS:
    //! A service was created, or is about to be destroyed (nullptr) while
    //! the models it holds and the page's widgets still exist.
    void serviceChanged(PlatformService* service);
    //! Pay a contact at this address in the Send tab.
    void payTo(const QString& address);

private:
    void resetService();
    void refresh();
    bool confirmFunding(CAmount amount, CAmount fee);
    //! Whether the user sends `amount` for `fee`; question has %1 the amount, %2 the fee and %3 the total.
    bool confirmPayment(const QString& question, CAmount amount, CAmount fee);
    bool confirmContest();
    void showInvitation(const SecureString& link);
    bool confirmClaim(CAmount amount, const std::optional<platform::InviterInfo>& inviter);

    const PlatformService::Sources m_sources;
    WalletModel* m_wallet_model{nullptr};
    ClientModel* m_client_model{nullptr};
    std::unique_ptr<PlatformService> m_service;
    //! Destroyed before the service it calls through.
    std::unique_ptr<IdentityFlow> m_identity_flow;
    //! Destroyed before the identity flow it builds on.
    std::unique_ptr<UsernameFlow> m_username_flow;
    //! Destroyed before the identity flow it reads.
    std::unique_ptr<ContactFlow> m_contact_flow;
    //! Destroyed before the flows it reads.
    std::unique_ptr<RecoveryFlow> m_recovery_flow;
    //! Destroyed before the flows it reads.
    std::unique_ptr<InvitationFlow> m_invitation_flow;

    QLabel* m_username{nullptr};
    QLabel* m_identity{nullptr};
    QLabel* m_contest{nullptr};
    QLabel* m_status{nullptr};
    QLabel* m_identity_status{nullptr};
    QLabel* m_recovery_status{nullptr};
    QPushButton* m_find{nullptr};
    QPushButton* m_create{nullptr};
    QPushButton* m_continue{nullptr};
    BitcoinAmountField* m_top_up_amount{nullptr};
    QPushButton* m_top_up{nullptr};
    QLineEdit* m_username_input{nullptr};
    QPushButton* m_register{nullptr};
    QLabel* m_username_status{nullptr};
    ContactsPage* m_contacts{nullptr};
    QPushButton* m_invite{nullptr};
    QLineEdit* m_invitation_link{nullptr};
    QPushButton* m_claim{nullptr};
    QPushButton* m_retry_claim{nullptr};
    QLabel* m_invitation_status{nullptr};
    QTableWidget* m_invitations{nullptr};
    QPushButton* m_stop{nullptr};
};

#endif // BITCOIN_QT_PLATFORM_PLATFORMPAGE_H
