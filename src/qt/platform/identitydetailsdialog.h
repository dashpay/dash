// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_IDENTITYDETAILSDIALOG_H
#define BITCOIN_QT_PLATFORM_IDENTITYDETAILSDIALOG_H

#include <platform/types.h>

#include <QDialog>
#include <QString>

#include <optional>

class PlatformService;
namespace PlatformUi {
class MessageLine;
} // namespace PlatformUi

QT_BEGIN_NAMESPACE
class QFrame;
class QLabel;
class QProgressBar;
class QPushButton;
class QScrollArea;
class QTableWidget;
QT_END_NAMESPACE

/** What this wallet's DashPay identity is on Dash Platform: username, id,
 *  state, its balance on Dash Platform, profile and (behind Show keys) its
 *  keys. Reads the identity and its profile once when opened, again only on
 *  Try again after a failure; while DashPay is paused it shows what the
 *  wallet knows and reads nothing. */
class IdentityDetailsDialog : public QDialog
{
    Q_OBJECT

public:
    //! paused: a gate closed after the service started; nothing is read.
    IdentityDetailsDialog(PlatformService& service, bool paused, QWidget* parent = nullptr);

protected:
    void changeEvent(QEvent* event) override;
    //! Opens as tall as its content, which only the laid-out dialog knows.
    void showEvent(QShowEvent* event) override;

private Q_SLOTS:
    void load();
    void onIdentity(const platform::Identity& identity);
    void onProfile(const QString& identity_hex, const QString& display_name, const QString& public_message,
                   quint64 revision);

private:
    void setBusy(bool busy);
    //! Grow, never past 85% of the screen, until the content needs no
    //! scrolling: every card, and the keys table at its full width once shown.
    void fitToContent();
    //! The balance in the wallet's current display unit.
    void showBalance();

    PlatformService& m_service;
    const bool m_paused;
    QScrollArea* m_scroll{nullptr};
    QProgressBar* m_busy{nullptr};
    PlatformUi::MessageLine* m_status{nullptr};
    QLabel* m_balance{nullptr};
    QLabel* m_display_name{nullptr};
    QLabel* m_message{nullptr};
    QPushButton* m_keys_toggle{nullptr};
    QFrame* m_keys_card{nullptr};
    QTableWidget* m_keys{nullptr};
    std::optional<platform::Identity> m_identity;
    bool m_have_profile{false};
    //! The reads load() started that have not answered yet. A failure only
    //! counts for a read of this dialog's; the dashboard's own profile reads
    //! answer through the same signals.
    bool m_identity_pending{false};
    bool m_profile_pending{false};
};

#endif // BITCOIN_QT_PLATFORM_IDENTITYDETAILSDIALOG_H
