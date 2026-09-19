// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_PLATFORMPAGE_H
#define BITCOIN_QT_PLATFORM_PLATFORMPAGE_H

#include <QWidget>

#include <memory>

class ClientModel;
class PlatformService;
class WalletModel;

class ContactsPage;

QT_BEGIN_NAMESPACE
class QLabel;
class QPushButton;
class QStackedWidget;
class QVBoxLayout;
QT_END_NAMESPACE

/** DashPay (Dash Platform) page: usernames, profiles and contacts for a
 *  single wallet. Only built with --enable-platform-gui.
 *
 *  Shows a welcome/upsell panel when the wallet has no identity, and a
 *  dashboard (profile header, quick actions, contacts) once registration
 *  has started. The per-wallet PlatformService is created lazily the first
 *  time both models are available and the feature is usable on this network.
 */
class PlatformPage : public QWidget
{
    Q_OBJECT

public:
    explicit PlatformPage(QWidget* parent = nullptr);
    ~PlatformPage() override;

    void setWalletModel(WalletModel* wallet_model);
    void setClientModel(ClientModel* client_model);

Q_SIGNALS:
    void platformServiceReady(PlatformService* service);
    //! Ask the wallet view to open the Send tab with this username prefilled.
    void sendToContact(const QString& username);

private Q_SLOTS:
    void openWizard();
    void openContactPicker();
    void refresh();

private:
    void maybeCreateService();
    void updateAvatar(const QString& name);

    WalletModel* walletModel{nullptr};
    ClientModel* clientModel{nullptr};
    std::unique_ptr<PlatformService> m_service;

    QStackedWidget* m_stack{nullptr};

    // Welcome page.
    QLabel* m_welcome{nullptr};
    QWidget* m_welcome_steps{nullptr};
    QPushButton* m_create_button{nullptr};

    // Dashboard page.
    QLabel* m_avatar{nullptr};
    QLabel* m_dashboard_username{nullptr};
    QLabel* m_dashboard_profile{nullptr};
    QLabel* m_dashboard_balance{nullptr};
    QLabel* m_dashboard_status{nullptr};
    QLabel* m_pending_banner{nullptr};
    QPushButton* m_send_button{nullptr};
    QPushButton* m_progress_button{nullptr};
    QVBoxLayout* m_dashboard_layout{nullptr};
    ContactsPage* m_contacts_page{nullptr};
};

#endif // BITCOIN_QT_PLATFORM_PLATFORMPAGE_H
