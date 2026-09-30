// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_PLATFORMOPTINDIALOG_H
#define BITCOIN_QT_PLATFORM_PLATFORMOPTINDIALOG_H

#include <QDialog>

QT_BEGIN_NAMESPACE
class QCheckBox;
class QDialogButtonBox;
QT_END_NAMESPACE

/** Asks the wallet owner to enable DashPay for this wallet after stating
 *  what the feature discloses to Platform nodes. Accepting writes the
 *  per-wallet opt-in record; nothing contacts Platform before that. */
class PlatformOptInDialog : public QDialog
{
    Q_OBJECT

public:
    explicit PlatformOptInDialog(const QString& wallet_name, QWidget* parent = nullptr);

private:
    QCheckBox* m_acknowledge{nullptr};
    QDialogButtonBox* m_buttons{nullptr};
};

#endif // BITCOIN_QT_PLATFORM_PLATFORMOPTINDIALOG_H
