// Copyright (c) 2011-2021 The Bitcoin Core developers
// Copyright (c) 2014-2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_NETWORKWIDGET_H
#define BITCOIN_QT_NETWORKWIDGET_H

#include <QWidget>

class ChainLockFeed;
class ClientModel;
class InstantSendFeed;
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

private Q_SLOTS:
    void handleClDataChanged();
    void handleIsDataChanged();
    void handleMnDataChanged();

private:
    Ui::NetworkWidget* ui;
    ClientModel* clientModel{nullptr};
    ChainLockFeed* m_feed_chainlock{nullptr};
    InstantSendFeed* m_feed_instantsend{nullptr};
    MasternodeFeed* m_feed_masternode{nullptr};
};

#endif // BITCOIN_QT_NETWORKWIDGET_H
