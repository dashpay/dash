// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/platformpage.h>

#include <qt/guiutil.h>

#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

PlatformPage::PlatformPage(PlatformService::Sources sources, QWidget* parent) :
    QWidget(parent),
    m_sources(std::move(sources))
{
    auto* layout{new QVBoxLayout(this)};
    auto* title{new QLabel(tr("DashPay"), this)};
    GUIUtil::setFont({title}, GUIUtil::FontWeight::Bold, 18);
    auto* notice{new QLabel(tr("DashPay connects to evonodes, which see your IP address unless you use a proxy."), this)};
    notice->setObjectName("dashPayNotice");
    notice->setWordWrap(true);
    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_start = new QPushButton(tr("Start DashPay"), this);
    m_start->setObjectName("dashPayStart");
    m_stop = new QPushButton(tr("Turn off DashPay for this wallet"), this);
    m_stop->setObjectName("dashPayStop");
    layout->addWidget(title);
    layout->addWidget(notice);
    layout->addWidget(m_status);
    layout->addWidget(m_start, 0, Qt::AlignLeft);
    layout->addWidget(m_stop, 0, Qt::AlignLeft);
    layout->addStretch();
    GUIUtil::updateFonts();

    connect(m_start, &QPushButton::clicked, this, [this] {
        if (!m_service->start()) {
            QMessageBox::warning(this, tr("DashPay"), tr("The wallet could not save that DashPay is started."));
        }
    });
    connect(m_stop, &QPushButton::clicked, this, [this] {
        if (!m_service->stop()) {
            QMessageBox::warning(this, tr("DashPay"), tr("The wallet could not save that DashPay is turned off."));
        }
    });
    refresh();
}

PlatformPage::~PlatformPage()
{
    // Before the child widgets go, which later layers connect to the service.
    if (m_service) Q_EMIT serviceChanged(nullptr);
}

void PlatformPage::setWalletModel(WalletModel* wallet_model)
{
    m_wallet_model = wallet_model;
    resetService();
}

void PlatformPage::setClientModel(ClientModel* client_model)
{
    m_client_model = client_model;
    resetService();
}

void PlatformPage::resetService()
{
    if (m_service) {
        Q_EMIT serviceChanged(nullptr);
        m_service.reset();
    }
    if (m_wallet_model && m_client_model) {
        m_service = std::make_unique<PlatformService>(*m_wallet_model, *m_client_model, m_sources);
        connect(m_service.get(), &PlatformService::changed, this, &PlatformPage::refresh);
        Q_EMIT serviceChanged(m_service.get());
    }
    refresh();
}

void PlatformPage::refresh()
{
    using Gate = PlatformService::Gate;
    const Gate gate{m_service ? m_service->gate() : Gate::NO_CONSENT};
    QString status{m_service ? m_service->reason() : QString{}};
    if (gate == Gate::NONE) {
        status = m_service->client() ? tr("DashPay is on for this wallet.")
                                     : tr("DashPay is switching to the current evonodes…");
    }
    m_status->setText(status);
    m_status->setVisible(!status.isEmpty());
    m_start->setVisible(m_service && gate == Gate::NO_CONSENT);
    m_stop->setVisible(m_service && m_service->consented());
}
