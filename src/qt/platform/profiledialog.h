// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_PROFILEDIALOG_H
#define BITCOIN_QT_PLATFORM_PROFILEDIALOG_H

#include <QDialog>
#include <QString>

class PlatformService;
namespace PlatformUi {
class MessageLine;
} // namespace PlatformUi

QT_BEGIN_NAMESPACE
class QCloseEvent;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
QT_END_NAMESPACE

/** View / edit this wallet's DashPay profile: display name and public
 *  message. Every other field of an existing profile is carried unchanged.
 *  Nothing can be edited until the current profile has loaded (or is proved
 *  absent): a save before that would publish empty fields over it. */
class ProfileDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ProfileDialog(PlatformService& service, QWidget* parent = nullptr);

private Q_SLOTS:
    void load();
    void save();
    void onProfileUpdated(bool ok, const QString& error, const QString& details);
    void updateState();

protected:
    void changeEvent(QEvent* event) override;
    void closeEvent(QCloseEvent* event) override;
    void reject() override;

private:
    PlatformService& m_service;
    QLineEdit* m_display_name{nullptr};
    QLabel* m_name_counter{nullptr};
    QPlainTextEdit* m_public_message{nullptr};
    QLabel* m_message_counter{nullptr};
    QProgressBar* m_busy{nullptr};
    PlatformUi::MessageLine* m_status{nullptr};
    QDialogButtonBox* m_buttons{nullptr};
    //! The fields as loaded; Save needs a change.
    QString m_loaded_name;
    QString m_loaded_message;
    bool m_loaded{false};
    bool m_pending{false};
};

#endif // BITCOIN_QT_PLATFORM_PROFILEDIALOG_H
