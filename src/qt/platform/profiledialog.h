// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_PROFILEDIALOG_H
#define BITCOIN_QT_PLATFORM_PROFILEDIALOG_H

#include <QDialog>

class PlatformService;

QT_BEGIN_NAMESPACE
class QLineEdit;
class QPlainTextEdit;
class QLabel;
class QDialogButtonBox;
class QCloseEvent;
QT_END_NAMESPACE

/** View / edit this wallet's DashPay profile (display name, message, avatar). */
class ProfileDialog : public QDialog
{
    Q_OBJECT

public:
    ProfileDialog(PlatformService& service, QWidget* parent = nullptr);

private Q_SLOTS:
    void save();
    void onProfileUpdated(bool ok, const QString& error);

protected:
    void closeEvent(QCloseEvent* event) override;
    void reject() override;

private:
    PlatformService& m_service;
    QLineEdit* m_display_name{nullptr};
    QPlainTextEdit* m_public_message{nullptr};
    QLineEdit* m_avatar_url{nullptr};
    QLabel* m_status{nullptr};
    QDialogButtonBox* m_buttons{nullptr};
    bool m_pending{false};
};

#endif // BITCOIN_QT_PLATFORM_PROFILEDIALOG_H
