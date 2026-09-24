// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/dashpayoptionswidget.h>

#include <qt/masternodewidgets.h>
#include <qt/platform/platformpage.h>
#include <qt/platform/platformservice.h>
#include <qt/platform/platformui.h>
#include <qt/walletmodel.h>

#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>

DashPayOptionsWidget::DashPayOptionsWidget(PlatformPage& page, WalletModel& wallet_model, QWidget* parent) :
    QGroupBox(tr("DashPay"), parent),
    m_page(&page),
    m_wallet_model(wallet_model)
{
    setObjectName("dashPayOptions");
    auto* layout = new QVBoxLayout(this);
    layout->setSpacing(MasternodeWidgetUtil::TITLE_SPACING);
    m_state = new QLabel(this);
    m_state->setWordWrap(true);
    m_state->setTextFormat(Qt::PlainText);
    auto* hint = PlatformUi::makeHint(tr("Each wallet has its own DashPay setting. Other wallets are not affected."), this);
    m_reason = PlatformUi::makeHint({}, this);
    m_button = new QPushButton(this);
    PlatformUi::makeSecondary(m_button);
    m_button->setAutoDefault(false);
    layout->addWidget(m_state);
    layout->addWidget(hint);
    layout->addWidget(m_reason);
    layout->addWidget(m_button, 0, Qt::AlignLeft);

    connect(m_button, &QPushButton::clicked, this, [this] {
        if (!m_page) return;
        // The window the confirmations belong to is the Options dialog.
        if (PlatformService::IsEnabled(m_wallet_model.wallet())) {
            m_page->disableDashPay(window());
        } else {
            m_page->enableDashPay(window());
        }
    });
    connect(&page, &PlatformPage::dashPayStateChanged, this, &DashPayOptionsWidget::updateState);
    updateState();
}

void DashPayOptionsWidget::updateState()
{
    if (!m_page) return;
    using Gate = PlatformAvailability::Gate;
    const QString wallet{m_wallet_model.getDisplayName()};
    const PlatformAvailability availability{m_page->availability()};
    const bool unusable{availability.gate == Gate::NO_PLATFORM || availability.gate == Gate::LEGACY_WALLET ||
                        availability.gate == Gate::NO_PRIVATE_KEYS};
    m_reason->hide();
    m_button->setEnabled(true);
    if (availability.enabled) {
        m_state->setText(tr("DashPay is on for wallet “%1”.").arg(wallet));
        m_button->setText(tr("Disable DashPay…"));
        m_button->setToolTip(tr("Stop contacting Dash Platform for this wallet and forget its local DashPay data"));
        m_button->show();
    } else if (unusable) {
        m_state->setText(tr("Wallet “%1” can't use DashPay. %2").arg(wallet, availability.reason));
        m_button->hide();
    } else {
        m_state->setText(tr("DashPay is off for wallet “%1”.").arg(wallet));
        m_button->setText(tr("Enable DashPay…"));
        m_button->setToolTip(tr("Register a username and pay your contacts by username"));
        m_button->show();
    }
}
