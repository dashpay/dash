// Copyright (c) 2011-2021 The Bitcoin Core developers
// Copyright (c) 2014-2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/networkwidget.h>
#include <qt/forms/ui_networkwidget.h>

#include <qt/clientfeeds.h>
#include <qt/clientmodel.h>
#include <qt/guiutil_font.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>

#include <QDateTime>

NetworkWidget::NetworkWidget(QWidget* parent) :
    QWidget(parent),
    ui(new Ui::NetworkWidget)
{
    ui->setupUi(this);

    GUIUtil::setFont({ui->labelChainLocks,
                      ui->labelCreditPool,
                      ui->labelInstantSend,
                      ui->labelMasternodes},
                     {GUIUtil::FontWeight::Bold, 16});

    for (auto* element : {ui->labelChainLocks, ui->labelInstantSend, ui->labelCreditPool}) {
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
    if (!clientModel) {
        return;
    }

    m_feed_chainlock = model->feedChainLock();
    if (m_feed_chainlock) {
        connect(m_feed_chainlock, &ChainLockFeed::dataReady, this, &NetworkWidget::handleClDataChanged);
        handleClDataChanged();
    }

    m_feed_creditpool = model->feedCreditPool();
    if (m_feed_creditpool) {
        connect(m_feed_creditpool, &CreditPoolFeed::dataReady, this, &NetworkWidget::handleCrDataChanged);
        handleCrDataChanged();
    }

    m_feed_instantsend = model->feedInstantSend();
    if (m_feed_instantsend) {
        connect(m_feed_instantsend, &InstantSendFeed::dataReady, this, &NetworkWidget::handleIsDataChanged);
        handleIsDataChanged();
    }

    m_feed_masternode = model->feedMasternode();
    if (m_feed_masternode) {
        connect(m_feed_masternode, &MasternodeFeed::dataReady, this, &NetworkWidget::handleMnDataChanged);
        handleMnDataChanged();
    }

    if (clientModel->getOptionsModel()) {
        m_display_unit = clientModel->getOptionsModel()->getDisplayUnit();
        connect(clientModel->getOptionsModel(), &OptionsModel::displayUnitChanged, this, &NetworkWidget::updateDisplayUnit);
    }
}

void NetworkWidget::handleCrDataChanged()
{
    if (!m_feed_creditpool) {
        return;
    }
    const auto data = m_feed_creditpool->data();
    if (!data) {
        return;
    }

    m_creditpool_diff = data->m_counts.m_diff;
    m_creditpool_limit = data->m_counts.m_limit;
    m_creditpool_locked = data->m_counts.m_locked;

    ui->labelCrLastBlock->setText(GUIUtil::formatAmount(m_display_unit, m_creditpool_diff, /*is_signed=*/true, /*truncate=*/2));
    ui->labelCrLocked->setText(GUIUtil::formatAmount(m_display_unit, m_creditpool_locked, /*is_signed=*/false, /*truncate=*/2));
    ui->labelCrPending->setText(QString::number(data->m_pending_unlocks));
    ui->labelCrLimit->setText(GUIUtil::formatAmount(m_display_unit, m_creditpool_limit, /*is_signed=*/false, /*truncate=*/2));
}

void NetworkWidget::handleMnDataChanged()
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

void NetworkWidget::handleClDataChanged()
{
    if (!m_feed_chainlock) {
        return;
    }
    const auto data = m_feed_chainlock->data();
    if (!data) {
        return;
    }
    ui->bestClHash->setText(data->m_hash);
    ui->bestClHeight->setText(QString::number(data->m_height));
    ui->bestClTime->setText(QDateTime::fromSecsSinceEpoch(data->m_block_time).toString());
}

void NetworkWidget::handleIsDataChanged()
{
    if (!m_feed_instantsend) {
        return;
    }
    const auto data = m_feed_instantsend->data();
    if (!data) {
        return;
    }
    ui->labelISLocks->setText(QString::number(data->m_counts.m_verified));
    ui->labelISPending->setText(QString::number(data->m_counts.m_unverified));
    ui->labelISWaiting->setText(QString::number(data->m_counts.m_awaiting_tx));
    ui->labelISUnprotected->setText(QString::number(data->m_counts.m_unprotected_tx));
}

void NetworkWidget::updateDisplayUnit(BitcoinUnit unit)
{
    m_display_unit = unit;
    ui->labelCrLastBlock->setText(GUIUtil::formatAmount(m_display_unit, m_creditpool_diff, /*is_signed=*/true, /*truncate=*/2));
    ui->labelCrLocked->setText(GUIUtil::formatAmount(m_display_unit, m_creditpool_locked, /*is_signed=*/false, /*truncate=*/2));
    ui->labelCrLimit->setText(GUIUtil::formatAmount(m_display_unit, m_creditpool_limit, /*is_signed=*/false, /*truncate=*/2));
}
