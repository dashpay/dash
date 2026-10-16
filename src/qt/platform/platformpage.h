// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_PLATFORMPAGE_H
#define BITCOIN_QT_PLATFORM_PLATFORMPAGE_H

#include <consensus/amount.h>
#include <qt/platform/platformservice.h>

#include <QWidget>

#include <memory>

class BitcoinAmountField;
class ContactFlow;
class ContactsPage;
class IdentityFlow;
class NotificationFeed;
class NotificationsPage;
class RecoveryFlow;
class UsernameFlow;
class UsernameSearchDialog;

QT_BEGIN_NAMESPACE
class QLabel;
class QLineEdit;
class QPushButton;
QT_END_NAMESPACE

/** The DashPay tab of one wallet. Only built with --enable-platform-gui.
 *
 *  It owns the wallet's PlatformService while both models are set, offers
 *  the first DashPay click (Find my identity) with a one-line privacy notice,
 *  and says why DashPay can't run when a gate is closed. Once DashPay runs it
 *  recovers and shows the wallet's identity, its username and its balance;
 *  creates, continues or tops up the identity; registers its username, a
 *  premium one through the masternode vote, whose tally it shows; and
 *  keeps its contacts, handing a payment to one to the Send tab. */
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
    NotificationFeed* notificationFeed() const { return m_notifications.get(); }
    RecoveryFlow* recoveryFlow() const { return m_recovery_flow.get(); }
    //! Pay the contact a username belongs to, as typed in the Send tab. The
    //! address comes with payTo(); anyone else is offered a contact request.
    void payUsername(const QString& username);
    //! The wallet received `amount` at `address`; the feed lists it when a
    //! contact paid it.
    void incomingPayment(const QString& address, CAmount amount);

Q_SIGNALS:
    //! A service was created, or is about to be destroyed (nullptr) while
    //! the models it holds and the page's widgets still exist.
    void serviceChanged(PlatformService* service);
    //! Pay a contact at this address in the Send tab. `username` is the one
    //! typed there; empty when the payment starts from the contacts list.
    void payTo(const QString& address, const QString& username = {});
    //! Paying a typed username chose no address; `error` says why.
    void usernameNotPaid(const QString& username, const QString& error);
    //! Something new among the contacts, for the tray.
    void notification(const QString& text);

private:
    void resetService();
    void refresh();
    void searchUsernames();
    void offerContactRequest(const QString& username);
    bool confirmFunding(CAmount amount, CAmount fee);
    bool confirmContest();

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
    //! Destroyed before the flow it reads, and made before the recovery flow so that it sees the
    //! contacts while the recovery still holds the wallet unlocked.
    std::unique_ptr<NotificationFeed> m_notifications;
    //! Destroyed before the flows it reads.
    std::unique_ptr<RecoveryFlow> m_recovery_flow;
    //! Destroyed before the flows it reads.
    std::unique_ptr<UsernameSearchDialog> m_search;

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
    NotificationsPage* m_notifications_page{nullptr};
    QPushButton* m_stop{nullptr};
};

#endif // BITCOIN_QT_PLATFORM_PLATFORMPAGE_H
