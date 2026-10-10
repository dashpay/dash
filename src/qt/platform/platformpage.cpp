// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/platformpage.h>

#include <chainparams.h>
#include <consensus/params.h>
#include <interfaces/node.h>
#include <platform/client.h>
#include <qt/clientmodel.h>
#include <qt/guiutil.h>
#include <qt/masternodewidgets.h>
#include <qt/platform/platformoptindialog.h>
#include <qt/platform/platformservice.h>
#include <qt/platform/platformui.h>
#include <qt/sharedmnwidgets.h>
#include <qt/walletmodel.h>

#include <QEvent>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>

using MasternodeWidgetUtil::CARD_PADDING;
using MasternodeWidgetUtil::GROUP_SPACING;
using MasternodeWidgetUtil::TITLE_SPACING;

namespace {
constexpr int NOTICE_ICON_SIZE{16};
constexpr int PANEL_MIN_WIDTH{520};
constexpr int PANEL_MAX_WIDTH{620};

platform::ClientConfig::Network NetworkOf(const std::string& network_id)
{
    if (network_id == CBaseChainParams::MAIN) return platform::ClientConfig::Network::MAIN;
    if (network_id == CBaseChainParams::TESTNET) return platform::ClientConfig::Network::TESTNET;
    if (network_id == CBaseChainParams::DEVNET) return platform::ClientConfig::Network::DEVNET;
    return platform::ClientConfig::Network::REGTEST;
}

//! A page whose content is a column of at most max_width, centred between
//! stretches rather than by alignment, so the layout sizes wrapped labels
//! by their height for the column's width. A panel is centred vertically
//! too; a document starts at the top.
QWidget* CenteredColumn(QWidget* parent, int max_width, bool panel, QVBoxLayout*& content_layout)
{
    auto* page = new QWidget(parent);
    auto* outer = new QVBoxLayout(page);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* content = new QWidget(page);
    if (panel) content->setMinimumWidth(PANEL_MIN_WIDTH);
    content->setMaximumWidth(max_width);
    content_layout = new QVBoxLayout(content);
    content_layout->setContentsMargins(24, panel ? 24 : 12, 24, 12);
    content_layout->setSpacing(GROUP_SPACING);
    auto* row = new QHBoxLayout();
    row->addStretch();
    row->addWidget(content, /*stretch=*/1);
    row->addStretch();
    if (panel) outer->addStretch();
    outer->addLayout(row);
    outer->addStretch();
    return page;
}

QLabel* CenteredLabel(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text, parent);
    label->setWordWrap(true);
    label->setAlignment(Qt::AlignHCenter);
    return label;
}

QLabel* CenteredHint(const QString& text, QWidget* parent)
{
    auto* label = PlatformUi::makeHint(text, parent);
    label->setAlignment(Qt::AlignHCenter);
    return label;
}

//! Where DashPay is turned off, on the pages where it is on but not working.
QPushButton* SettingsButton(QWidget* parent)
{
    auto* button = new QPushButton(QObject::tr("DashPay settings…"), parent);
    PlatformUi::makeSecondary(button);
    button->setToolTip(QObject::tr("Turn DashPay on or off for this wallet"));
    return button;
}

void SetIcon(QLabel* label, const QString& icon, GUIUtil::ThemedColor color)
{
    label->setVisible(!icon.isEmpty());
    if (!icon.isEmpty()) label->setPixmap(GUIUtil::getIcon(icon, color).pixmap(NOTICE_ICON_SIZE, NOTICE_ICON_SIZE));
}

} // namespace

PlatformPage::PlatformPage(QWidget* parent) :
    QWidget(parent)
{
    setObjectName("PlatformPage");
    auto* outer = new QVBoxLayout(this);
    m_stack = new QStackedWidget(this);
    outer->addWidget(m_stack);

    // Page 0: opt-in, or why DashPay cannot run.
    QVBoxLayout* welcome_layout{nullptr};
    QWidget* welcome_page{CenteredColumn(this, PANEL_MAX_WIDTH, /*panel=*/true, welcome_layout)};
    auto* title = new QLabel(tr("DashPay"), welcome_page);
    title->setAlignment(Qt::AlignHCenter);
    GUIUtil::setFont({title}, GUIUtil::FontWeight::Bold, 18);
    auto* intro = CenteredLabel(tr("Send and receive Dash using usernames instead of addresses."), welcome_page);
    m_privacy_note = CenteredHint(tr("Nothing is sent to Dash Platform until you enable DashPay."), welcome_page);

    // Why DashPay cannot run (or start) right now, with what resolves it.
    m_notice = MasternodeWidgetUtil::makeCard(welcome_page);
    auto* notice_layout = new QVBoxLayout(m_notice);
    notice_layout->setContentsMargins(CARD_PADDING, CARD_PADDING, CARD_PADDING, CARD_PADDING);
    notice_layout->setSpacing(TITLE_SPACING);
    auto* notice_title_row = new QHBoxLayout();
    notice_title_row->setSpacing(TITLE_SPACING);
    m_notice_icon = new QLabel(m_notice);
    m_notice_icon->setFixedSize(NOTICE_ICON_SIZE, NOTICE_ICON_SIZE);
    m_notice_title = new QLabel(m_notice);
    m_notice_title->setWordWrap(true);
    GUIUtil::setFont({m_notice_title}, GUIUtil::FontWeight::Bold);
    notice_title_row->addWidget(m_notice_icon);
    notice_title_row->addWidget(m_notice_title, /*stretch=*/1);
    m_notice_body = new QLabel(m_notice);
    m_notice_body->setWordWrap(true);
    m_notice_body->setTextFormat(Qt::PlainText);
    m_notice_busy = PlatformUi::makeBusyBar(m_notice);
    m_notice_button = new QPushButton(m_notice);
    notice_layout->addLayout(notice_title_row);
    notice_layout->addWidget(m_notice_body);
    notice_layout->addWidget(m_notice_busy);
    notice_layout->addWidget(m_notice_button, 0, Qt::AlignLeft);
    connect(m_notice_button, &QPushButton::clicked, this, [this] { runNoticeAction(m_notice_action); });

    m_enable_button = new QPushButton(tr("Enable DashPay for this wallet…"), welcome_page);
    connect(m_enable_button, &QPushButton::clicked, this, [this] { enableDashPay(this); });
    m_settings_button = SettingsButton(welcome_page);
    connect(m_settings_button, &QPushButton::clicked, this, &PlatformPage::dashPaySettingsRequested);
    welcome_layout->addWidget(title);
    welcome_layout->addWidget(intro);
    welcome_layout->addWidget(m_privacy_note);
    welcome_layout->addWidget(m_notice);
    welcome_layout->addWidget(m_enable_button, 0, Qt::AlignHCenter);
    welcome_layout->addSpacing(GROUP_SPACING);
    welcome_layout->addWidget(m_settings_button, 0, Qt::AlignHCenter);
    m_stack->addWidget(welcome_page);

    // Page 1: dashboard.
    m_dashboard = new QWidget(this);
    auto* dashboard_layout = new QVBoxLayout(m_dashboard);
    m_dashboard_status = new QLabel(tr("DashPay is enabled for this wallet."), m_dashboard);
    m_dashboard_status->setWordWrap(true);
    dashboard_layout->addWidget(m_dashboard_status);
    dashboard_layout->addStretch();
    m_stack->addWidget(m_dashboard);

    GUIUtil::updateFonts();
    SharedMnFitWrappedLabels(this);
}

PlatformPage::~PlatformPage() = default;

void PlatformPage::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() != QEvent::StyleChange) return;
    // Everything styled from code follows the theme.
    PlatformUi::restyle(this);
    refresh();
}

void PlatformPage::setWalletModel(WalletModel* wallet_model)
{
    walletModel = wallet_model;
    refresh();
}

void PlatformPage::setClientModel(ClientModel* client_model)
{
    if (!client_model && m_service) {
        // Detached at shutdown, before the wallet and client models are
        // deleted: the service stops while the models it holds still exist.
        Q_EMIT platformServiceReady(nullptr);
        m_service.reset();
    }
    clientModel = client_model;
    if (clientModel) {
        // The gates that open on their own (sync finished, first ChainLock
        // seen) retry the service creation; nothing is fetched by these.
        connect(clientModel, &ClientModel::networkActiveChanged, this, &PlatformPage::refresh);
        connect(clientModel, &ClientModel::numBlocksChanged, this, &PlatformPage::refresh);
        // A route that appears later (the Tor controller's onion proxy, which
        // the node's first onion peers follow) retries the service creation.
        connect(clientModel, &ClientModel::numConnectionsChanged, this, [this] {
            if (!m_service) refresh();
        });
        connect(clientModel, &ClientModel::chainLockChanged, this, &PlatformPage::refresh);
    }
    refresh();
}

void PlatformPage::maybeCreateService()
{
    if (m_service || !walletModel || !clientModel) return;
    if (!PlatformService::Availability(*walletModel, *clientModel).available()) return;
    // The route is fixed for the service's life, as the node's proxies are
    // for the process's; the Tor controller adding an onion proxy later is
    // picked up by a service created after it.
    const auto route{PlatformRoute::Choose(PlatformNetworkSettings::Read(clientModel->node()))};
    if (!route) return;
    platform::ClientConfig config;
    config.network = NetworkOf(Params().NetworkIDString());
    config.platform_llmq_type = static_cast<uint8_t>(Params().GetConsensus().llmqTypePlatform);
    config.proxy = route->proxy;
    auto client{platform::MakeSdkPlatformClient(config)};
    if (!client) return;
    m_service = std::make_unique<PlatformService>(*walletModel, *clientModel, std::move(client), *route, this);
    connect(m_service.get(), &PlatformService::unsupportedProtocolVersion, this, &PlatformPage::refresh);
    connect(m_service.get(), &PlatformService::reachabilityChanged, this, &PlatformPage::refresh);
    Q_EMIT platformServiceReady(m_service.get());
}

PlatformAvailability PlatformPage::availability() const
{
    if (!walletModel || !clientModel) return {};
    return PlatformService::Availability(*walletModel, *clientModel, m_service.get());
}

bool PlatformPage::enableDashPay(QWidget* dialog_parent)
{
    if (!walletModel || !clientModel) return false;
    PlatformOptInDialog dialog(walletModel->getDisplayName(), dialog_parent);
    if (dialog.exec() != QDialog::Accepted) return false;
    if (!PlatformService::Enable(walletModel->wallet())) {
        QMessageBox::warning(dialog_parent, tr("Enable DashPay"),
                             tr("The DashPay setting could not be saved to the wallet."));
        return false;
    }
    refresh();
    return true;
}

bool PlatformPage::disableDashPay(QWidget* dialog_parent)
{
    if (!walletModel) return false;
    QMessageBox box(dialog_parent);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(tr("Disable DashPay"));
    box.setText(tr("Disable DashPay for “%1”?").arg(walletModel->getDisplayName()));
    box.setInformativeText(tr("Dash Core will stop contacting Dash Platform for this wallet and delete its local "
                              "DashPay data.\n\nYour username, profile and contact requests stay on Dash Platform. "
                              "Enabling DashPay again, in this wallet or in one restored from the same recovery "
                              "phrase, recovers them."));
    auto* disable{box.addButton(tr("Disable DashPay"), QMessageBox::DestructiveRole)};
    PlatformUi::makeSecondary(disable);
    auto* cancel{box.addButton(QMessageBox::Cancel)};
    box.setDefaultButton(cancel);
    box.setEscapeButton(cancel);
    box.exec();
    if (box.clickedButton() != disable) return false;
    m_service.reset();
    Q_EMIT platformServiceReady(nullptr);
    if (!PlatformService::WipeRecords(walletModel->wallet())) {
        QMessageBox::warning(dialog_parent, tr("Disable DashPay"),
                             tr("Some DashPay data could not be deleted from the wallet. Try disabling DashPay "
                                "again."));
        refresh();
        return false;
    }
    refresh();
    return true;
}

void PlatformPage::runNoticeAction(NoticeAction action)
{
    switch (action) {
    case NoticeAction::TURN_NETWORK_ON:
        if (clientModel) clientModel->node().setNetworkActive(true);
        break;
    case NoticeAction::NONE:
        break;
    }
    refresh();
}

void PlatformPage::setNotice(const QString& icon, const QString& title, const QString& body, bool busy, NoticeAction action)
{
    m_notice->setVisible(!title.isEmpty());
    SetIcon(m_notice_icon, icon, GUIUtil::ThemedColor::ORANGE);
    m_notice_title->setText(title);
    m_notice_body->setText(body);
    m_notice_body->setVisible(!body.isEmpty());
    m_notice_busy->setVisible(busy);
    m_notice_action = action;
    switch (action) {
    case NoticeAction::TURN_NETWORK_ON:
        m_notice_button->setText(tr("Turn network on"));
        break;
    case NoticeAction::NONE:
        break;
    }
    m_notice_button->setVisible(action != NoticeAction::NONE);
    // Measured again for its new text.
    m_notice->updateGeometry();
}

void PlatformPage::refresh()
{
    using Gate = PlatformAvailability::Gate;
    if (!walletModel || !clientModel) return;
    if (!m_service) maybeCreateService();
    Q_EMIT dashPayStateChanged();

    const PlatformAvailability availability{PlatformService::Availability(*walletModel, *clientModel, m_service.get())};
    m_privacy_note->setVisible(!availability.enabled);
    m_enable_button->setVisible(!availability.enabled && availability.gate == Gate::NONE);
    m_settings_button->setVisible(availability.enabled);
    switch (availability.gate) {
    case Gate::NONE:
        setNotice({}, {}, {}, false, NoticeAction::NONE);
        break;
    case Gate::NETWORK_SETTINGS:
    case Gate::UNREACHABLE:
        setNotice(QStringLiteral("proxy"), availability.title, availability.reason, false, NoticeAction::NONE);
        break;
    case Gate::NETWORK_INACTIVE:
        setNotice(QStringLiteral("warning"), availability.title, availability.reason, false, NoticeAction::TURN_NETWORK_ON);
        break;
    case Gate::SYNCING:
    case Gate::NO_CHAINLOCK:
        setNotice({}, availability.title, availability.reason, /*busy=*/true, NoticeAction::NONE);
        break;
    case Gate::LEGACY_WALLET:
    case Gate::NO_PRIVATE_KEYS:
        setNotice(QStringLiteral("warning"), availability.title, availability.reason, false, NoticeAction::NONE);
        break;
    }

    if (!m_service) {
        m_stack->setCurrentIndex(0);
        return;
    }
    m_stack->setCurrentIndex(1);
}
