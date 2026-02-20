// Copyright (c) 2011-2021 The Bitcoin Core developers
// Copyright (c) 2014-2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/networkwidget.h>
#include <qt/forms/ui_networkwidget.h>

#include <qt/clientfeeds.h>
#include <qt/clientmodel.h>
#include <qt/guiutil.h>
#include <qt/guiutil_font.h>

NetworkWidget::NetworkWidget(QWidget* parent) :
    QWidget(parent),
    ui(new Ui::NetworkWidget)
{
    ui->setupUi(this);

    GUIUtil::setFont({ui->labelNetwork,
                      ui->labelBlockChain,
                      ui->labelMempoolTitle},
                     {GUIUtil::FontWeight::Bold, 16});

    for (auto* element : {ui->labelBlockChain, ui->labelMempoolTitle}) {
        element->setContentsMargins(0, 10, 0, 0);
    }
}

NetworkWidget::~NetworkWidget()
{
    delete ui;
}

void NetworkWidget::setClientModel(ClientModel* model)
{
    clientModel = model;

    if (clientModel) {
        connect(model, &ClientModel::chainLockChanged, this, &NetworkWidget::setChainLock);
        connect(model, &ClientModel::islockCountChanged, this, &NetworkWidget::setInstantSendLockCount);

        m_feed_masternode = model->feedMasternode();
        if (m_feed_masternode) {
            connect(m_feed_masternode, &MasternodeFeed::dataReady, this, &NetworkWidget::updateMasternodeCount);
            updateMasternodeCount();
        }
    }
}

void NetworkWidget::updateMasternodeCount()
{
    if (!m_feed_masternode) {
        return;
    }
    const auto data = m_feed_masternode->data();
    if (!data || !data->m_valid) {
        return;
    }
    ui->masternodeCount->setText(tr("Total: %1 (Enabled: %2)")
        .arg(QString::number(data->m_counts.m_total_mn))
        .arg(QString::number(data->m_counts.m_valid_mn)));
    ui->evoCount->setText(tr("Total: %1 (Enabled: %2)")
        .arg(QString::number(data->m_counts.m_total_evo))
        .arg(QString::number(data->m_counts.m_valid_evo)));
}

void NetworkWidget::setChainLock(const QString& bestChainLockHash, int bestChainLockHeight)
{
    ui->bestChainLockHash->setText(bestChainLockHash);
    ui->bestChainLockHeight->setText(QString::number(bestChainLockHeight));
}

void NetworkWidget::setInstantSendLockCount(size_t count)
{
    ui->instantSendLockCount->setText(QString::number(count));
}
