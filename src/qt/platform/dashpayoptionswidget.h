// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_DASHPAYOPTIONSWIDGET_H
#define BITCOIN_QT_PLATFORM_DASHPAYOPTIONSWIDGET_H

#include <QGroupBox>
#include <QPointer>

class PlatformPage;
class WalletModel;

QT_BEGIN_NAMESPACE
class QLabel;
class QPushButton;
QT_END_NAMESPACE

/** The DashPay group of the Options dialog's Wallet page, for the wallet the
 *  main window shows. Opting in is a per-wallet record rather than a global
 *  setting, so the group names the wallet and its button acts at once
 *  through its own confirmation, never through the dialog's OK or Cancel. */
class DashPayOptionsWidget : public QGroupBox
{
    Q_OBJECT

public:
    DashPayOptionsWidget(PlatformPage& page, WalletModel& wallet_model, QWidget* parent = nullptr);

private Q_SLOTS:
    void updateState();

private:
    QPointer<PlatformPage> m_page;
    WalletModel& m_wallet_model;
    QLabel* m_state{nullptr};
    QLabel* m_reason{nullptr};
    QPushButton* m_button{nullptr};
};

#endif // BITCOIN_QT_PLATFORM_DASHPAYOPTIONSWIDGET_H
