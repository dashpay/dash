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
struct PlatformAvailability;

QT_BEGIN_NAMESPACE
class QFrame;
class QLabel;
class QProgressBar;
class QPushButton;
class QStackedWidget;
QT_END_NAMESPACE

/** DashPay (Dash Platform) page for a single wallet. Only built with
 *  --enable-platform-gui.
 *
 *  Shows the opt-in panel until the wallet owner enables DashPay, then why
 *  the service cannot start (network settings, network inactive, syncing,
 *  no ChainLock) with the action that resolves it, or, once it runs, the
 *  dashboard. The per-wallet PlatformService is created only when the
 *  wallet has opted in and every gate passes; nothing contacts Platform
 *  before that. */
class PlatformPage : public QWidget
{
    Q_OBJECT

public:
    explicit PlatformPage(QWidget* parent = nullptr);
    ~PlatformPage() override;

    void setWalletModel(WalletModel* wallet_model);
    void setClientModel(ClientModel* client_model);
    //! Why DashPay can or cannot run for this wallet now.
    PlatformAvailability availability() const;

    //! Ask for the opt-in with its disclosure, then turn DashPay on for this
    //! wallet. Dialogs are parented to `dialog_parent`. Returns whether it
    //! was turned on.
    bool enableDashPay(QWidget* dialog_parent);
    //! Confirm, then turn DashPay off for this wallet and delete its local
    //! DashPay data. Returns whether it was turned off.
    bool disableDashPay(QWidget* dialog_parent);

Q_SIGNALS:
    void platformServiceReady(PlatformService* service);
    //! Ask the main window to open the DashPay settings of this wallet.
    void dashPaySettingsRequested();
    //! The page was brought up to date: whether DashPay is on, or can be
    //! turned off, may have changed.
    void dashPayStateChanged();

protected:
    void changeEvent(QEvent* event) override;

private Q_SLOTS:
    void refresh();

private:
    //! What the notice action button of the welcome page does.
    enum class NoticeAction {
        NONE,
        TURN_NETWORK_ON,
    };

    void maybeCreateService();
    void setNotice(const QString& icon, const QString& title, const QString& body, bool busy, NoticeAction action);
    void runNoticeAction(NoticeAction action);

    WalletModel* walletModel{nullptr};
    ClientModel* clientModel{nullptr};
    std::unique_ptr<PlatformService> m_service;

    QStackedWidget* m_stack{nullptr};

    // Page 0: opt-in / unavailable.
    QLabel* m_privacy_note{nullptr};
    QFrame* m_notice{nullptr};
    QLabel* m_notice_icon{nullptr};
    QLabel* m_notice_title{nullptr};
    QLabel* m_notice_body{nullptr};
    QProgressBar* m_notice_busy{nullptr};
    QPushButton* m_notice_button{nullptr};
    NoticeAction m_notice_action{NoticeAction::NONE};
    QPushButton* m_enable_button{nullptr};
    QPushButton* m_settings_button{nullptr};

    // Page 1: dashboard.
    QWidget* m_dashboard{nullptr};
    QLabel* m_dashboard_status{nullptr};
};

#endif // BITCOIN_QT_PLATFORM_PLATFORMPAGE_H
