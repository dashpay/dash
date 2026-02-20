// Copyright (c) 2011-2021 The Bitcoin Core developers
// Copyright (c) 2014-2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_NETWORKWIDGET_H
#define BITCOIN_QT_NETWORKWIDGET_H

#include <cstddef>

#include <QString>
#include <QWidget>

class ClientModel;
class MasternodeFeed;
namespace Ui {
class NetworkWidget;
} // namespace Ui

class NetworkWidget : public QWidget
{
    Q_OBJECT

public:
    explicit NetworkWidget(QWidget* parent = nullptr);
    ~NetworkWidget() override;

    void setClientModel(ClientModel* model);

public Q_SLOTS:
    /** Update number of masternodes shown in the UI */
    void updateMasternodeCount();
    /** Set latest chainlocked hash and height shown in the UI */
    void setChainLock(const QString& bestChainLockHash, int bestChainLockHeight);
    /** Set number of InstantSend locks */
    void setInstantSendLockCount(size_t count);

private:
    Ui::NetworkWidget* ui;
    ClientModel* clientModel{nullptr};
    MasternodeFeed* m_feed_masternode{nullptr};
};

#endif // BITCOIN_QT_NETWORKWIDGET_H
