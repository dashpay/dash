// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_PLATFORMPAGE_H
#define BITCOIN_QT_PLATFORM_PLATFORMPAGE_H

#include <platform/client.h>

#include <QPointer>
#include <QWidget>

#include <functional>
#include <memory>
#include <optional>

class ClientModel;
class ContactsPage;
class CreateUsernameWizard;
class PlatformService;
class WalletModel;
struct PlatformAvailability;
namespace PlatformUi {
class MessageLine;
} // namespace PlatformUi

QT_BEGIN_NAMESPACE
class QFrame;
class QLabel;
class QProgressBar;
class QPushButton;
class QHideEvent;
class QShowEvent;
class QStackedWidget;
class QTimer;
class QVBoxLayout;
QT_END_NAMESPACE

/** DashPay (Dash Platform) page for a single wallet. Only built with
 *  --enable-platform-gui.
 *
 *  Shows the opt-in panel until the wallet owner enables DashPay, then why
 *  the service cannot start (network settings, network inactive, syncing,
 *  no ChainLock) with the action that resolves it, the welcome panel with
 *  the username wizard while the wallet has no identity, and the dashboard
 *  (profile header, registration state, quick actions, contacts) once
 *  registration has started. The per-wallet PlatformService is created only
 *  when the wallet has opted in and every gate passes; nothing contacts
 *  Platform before that. */
class PlatformPage : public QWidget
{
    Q_OBJECT

public:
    //! Makes the client the page's service reaches Platform through.
    using ClientFactory = std::function<std::unique_ptr<platform::PlatformClient>(const platform::ClientConfig&)>;

    explicit PlatformPage(QWidget* parent = nullptr, ClientFactory make_client = platform::MakeSdkPlatformClient);
    ~PlatformPage() override;

    void setWalletModel(WalletModel* wallet_model);
    void setClientModel(ClientModel* client_model);
    //! Why DashPay can or cannot run for this wallet now.
    PlatformAvailability availability() const;
    //! Turning DashPay off wipes the records, the only trace of an asset
    //! lock no identity consumed yet: it waits while the wallet holds one,
    //! also while a gate keeps the service from starting.
    bool holdsUnconsumedFunding() const;
    //! Why DashPay can't be turned off while holdsUnconsumedFunding().
    QString disableBlockedReason() const;
    //! The username this wallet registered, or empty.
    QString registeredUsername() const;

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
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private Q_SLOTS:
    void openWizard();
    //! Redraw from local state; no Platform read.
    void refresh();
    //! Re-read what the dashboard shows of the identity (profile, balance,
    //! contacts, a premium username's votes). Only while the page is shown:
    //! every read discloses the identity and its contacts to an evonode, so
    //! an idle wallet stays quiet.
    void fetchDashboard();

private:
    //! What the notice action button of the welcome page does.
    enum class NoticeAction {
        NONE,
        TURN_NETWORK_ON,
    };

    void maybeCreateService();
    void connectService();
    //! Page 0 with the steps (nothing blocks) or a notice saying what does.
    void showWelcome(const PlatformAvailability& availability);
    void setNotice(const QString& icon, const QString& title, const QString& body, bool busy, NoticeAction action);
    void refreshDashboard(const PlatformAvailability& availability);
    //! Confirm, then send the coins of a funding payment the network did
    //! not accept back to this wallet (IdentityFlow::releaseFunding()).
    void releaseFunding();
    void updateAvatar();
    //! A balance on Dash Platform in the wallet's display unit.
    QString formatBalance(quint64 credits) const;
    //! The balance line of the header, in the current display unit.
    void showBalance();
    //! fetchDashboard() unless a fetch started moments ago.
    void fetchDashboardIfStale();
    //! Read a premium username's votes again, at most every few minutes.
    void refreshVotesIfDue();
    void runNoticeAction(NoticeAction action);

    const ClientFactory m_make_client;
    WalletModel* walletModel{nullptr};
    ClientModel* clientModel{nullptr};
    std::unique_ptr<PlatformService> m_service;
    //! The service stopped at detach, kept until the page goes: a dialog,
    //! or a nested event loop under a click on this page, may still use it.
    std::unique_ptr<PlatformService> m_stopped_service;

    QStackedWidget* m_stack{nullptr};

    // Page 0: opt-in, why DashPay cannot run, or the welcome panel.
    QWidget* m_welcome_steps{nullptr};
    QList<QLabel*> m_step_badges;
    QLabel* m_privacy_note{nullptr};
    QFrame* m_notice{nullptr};
    QLabel* m_notice_icon{nullptr};
    QLabel* m_notice_title{nullptr};
    QLabel* m_notice_body{nullptr};
    QProgressBar* m_notice_busy{nullptr};
    QPushButton* m_notice_button{nullptr};
    NoticeAction m_notice_action{NoticeAction::NONE};
    QPushButton* m_enable_button{nullptr};
    QPushButton* m_create_button{nullptr};
    QPushButton* m_settings_button{nullptr};

    // Page 1: dashboard.
    QLabel* m_avatar{nullptr};
    QLabel* m_username{nullptr};
    QLabel* m_display_name{nullptr};
    QLabel* m_message{nullptr};
    QLabel* m_balance{nullptr};
    QPushButton* m_edit_profile_button{nullptr};
    QFrame* m_state_card{nullptr};
    QLabel* m_state_icon{nullptr};
    QString m_state_icon_name;
    QLabel* m_state_title{nullptr};
    QLabel* m_state_body{nullptr};
    PlatformUi::MessageLine* m_state_error{nullptr};
    QLabel* m_state_hint{nullptr};
    QProgressBar* m_state_busy{nullptr};
    QPushButton* m_state_button{nullptr};
    PlatformUi::MessageLine* m_saved_line{nullptr};
    QLabel* m_alert{nullptr};
    QPushButton* m_alert_button{nullptr};
    NoticeAction m_alert_action{NoticeAction::NONE};
    QVBoxLayout* m_dashboard_layout{nullptr};
    //! Where the contacts go in m_dashboard_layout, above the spare space.
    int m_contacts_index{0};
    ContactsPage* m_contacts_page{nullptr};
    QTimer* m_fetch_timer{nullptr}; //!< fallback reads while shown
    //! When the last dashboard fetch and vote read started (GetTime()).
    int64_t m_last_fetch{0};
    int64_t m_votes_read{0};
    //! The balance and profile read again after a failed read.
    QTimer* m_balance_retry{nullptr};
    QTimer* m_profile_retry{nullptr};

    //! What the header last learned about the profile: the display name
    //! (for the avatar) and whether one exists.
    QString m_profile_display_name;
    std::optional<bool> m_have_profile;
    //! Reading the profile failed and none was read before.
    bool m_profile_failed{false};
    //! The identity's balance on Dash Platform (in credits), once read.
    std::optional<quint64> m_credits;
    //! The last vote count read for a premium username.
    QString m_votes;
    //! A gate closed after the service started: Platform actions wait.
    bool m_paused{false};
    //! The client could not be created for the node's network settings.
    bool m_client_failed{false};
    QPointer<CreateUsernameWizard> m_wizard;
};

#endif // BITCOIN_QT_PLATFORM_PLATFORMPAGE_H
