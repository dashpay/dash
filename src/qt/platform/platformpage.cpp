// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/platformpage.h>

#include <chainparams.h>
#include <consensus/params.h>
#include <interfaces/node.h>
#include <qt/clientmodel.h>
#include <qt/guiutil.h>
#include <qt/masternodewidgets.h>
#include <qt/optionsmodel.h>
#include <qt/platform/contactspage.h>
#include <qt/platform/createusernamewizard.h>
#include <qt/platform/identityflow.h>
#include <qt/platform/platformoptindialog.h>
#include <qt/platform/platformservice.h>
#include <qt/platform/platformui.h>
#include <qt/platform/profiledialog.h>
#include <qt/sharedmnwidgets.h>
#include <qt/walletmodel.h>
#include <util/strencodings.h>
#include <util/time.h>

#include <QEvent>
#include <QFrame>
#include <QGuiApplication>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHideEvent>
#include <QLabel>
#include <QLocale>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QStackedWidget>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>

using MasternodeWidgetUtil::CARD_PADDING;
using MasternodeWidgetUtil::GROUP_SPACING;
using MasternodeWidgetUtil::TITLE_SPACING;

namespace {
constexpr int AVATAR_SIZE{56};
constexpr int STEP_BADGE_SIZE{24};
constexpr int NOTICE_ICON_SIZE{16};
constexpr int PANEL_MIN_WIDTH{520};
constexpr int PANEL_MAX_WIDTH{620};
//! Dashboard re-read cadence while the page is shown and no ChainLock came.
constexpr int DASHBOARD_FETCH_INTERVAL_MS{5 * 60'000};
//! Showing the page again reads nothing when a read started this recently
//! (switching tabs back and forth).
constexpr int64_t SHOW_FETCH_FLOOR_SECONDS{30};
//! A premium username's votes are re-read on a ChainLock at most this often.
constexpr int64_t VOTES_REFRESH_SECONDS{10 * 60};
//! A read of the dashboard that failed is made again after this long.
constexpr int READ_RETRY_MS{30'000};
//! The dashboard reads as a document, not across a wide window.
constexpr int DASHBOARD_MAX_WIDTH{1040};

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

//! The icon and colour of a notice or state card, by the icon name.
GUIUtil::ThemedColor IconColor(const QString& icon, bool error)
{
    if (error) return GUIUtil::ThemedColor::RED;
    return icon == QLatin1String("voting") ? GUIUtil::ThemedColor::BLUE : GUIUtil::ThemedColor::ORANGE;
}
} // namespace

PlatformPage::PlatformPage(QWidget* parent, ClientFactory make_client) :
    QWidget(parent),
    m_make_client(std::move(make_client))
{
    setObjectName("PlatformPage");
    auto* outer = new QVBoxLayout(this);
    m_stack = new QStackedWidget(this);
    outer->addWidget(m_stack);

    // Page 0: opt-in, why DashPay cannot run, or the welcome panel.
    QVBoxLayout* welcome_layout{nullptr};
    QWidget* welcome_page{CenteredColumn(this, PANEL_MAX_WIDTH, /*panel=*/true, welcome_layout)};
    auto* title = new QLabel(tr("DashPay"), welcome_page);
    title->setAlignment(Qt::AlignHCenter);
    GUIUtil::setFont({title}, GUIUtil::FontWeight::Bold, 18);
    auto* intro = CenteredLabel(tr("Send and receive Dash using usernames instead of addresses."), welcome_page);

    m_welcome_steps = new QWidget(welcome_page);
    auto* steps_layout = new QGridLayout(m_welcome_steps);
    steps_layout->setContentsMargins(0, 0, 0, 0);
    steps_layout->setHorizontalSpacing(14);
    steps_layout->setVerticalSpacing(GROUP_SPACING);
    steps_layout->setColumnStretch(1, 1);
    const auto add_step = [this, steps_layout](int row, const QString& heading, const QString& detail) {
        auto* badge = new QLabel(m_welcome_steps);
        badge->setFixedSize(STEP_BADGE_SIZE, STEP_BADGE_SIZE);
        m_step_badges.append(badge);
        auto* text_layout = new QVBoxLayout();
        text_layout->setContentsMargins(0, 0, 0, 0);
        text_layout->setSpacing(2);
        auto* step_heading = new QLabel(heading, m_welcome_steps);
        GUIUtil::setFont({step_heading}, GUIUtil::FontWeight::Bold);
        text_layout->addWidget(step_heading);
        text_layout->addWidget(PlatformUi::makeHint(detail, m_welcome_steps));
        steps_layout->addWidget(badge, row, 0, Qt::AlignTop);
        steps_layout->addLayout(text_layout, row, 1);
    };
    add_step(0, tr("Claim your username"), tr("Register a unique username on Dash Platform."));
    add_step(1, tr("Connect with people"),
             tr("Send contact requests to people you know. Once they accept, you're connected."));
    add_step(2, tr("Pay privately by username"),
             tr("Each payment goes to a fresh address that only you and your contact can link."));
    auto* cost_note = CenteredHint(tr("Registering a username uses a small payment from this wallet to fund your Dash "
                                      "Platform identity."),
                                   welcome_page);
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
    m_create_button = new QPushButton(tr("Create a username…"), welcome_page);
    connect(m_create_button, &QPushButton::clicked, this, &PlatformPage::openWizard);
    m_settings_button = SettingsButton(welcome_page);
    connect(m_settings_button, &QPushButton::clicked, this, &PlatformPage::dashPaySettingsRequested);
    welcome_layout->addWidget(title);
    welcome_layout->addWidget(intro);
    welcome_layout->addWidget(m_welcome_steps);
    welcome_layout->addWidget(cost_note);
    welcome_layout->addWidget(m_privacy_note);
    welcome_layout->addWidget(m_notice);
    welcome_layout->addWidget(m_enable_button, 0, Qt::AlignHCenter);
    welcome_layout->addWidget(m_create_button, 0, Qt::AlignHCenter);
    welcome_layout->addSpacing(GROUP_SPACING);
    welcome_layout->addWidget(m_settings_button, 0, Qt::AlignHCenter);
    m_stack->addWidget(welcome_page);

    // Page 1: dashboard, a centred column.
    QVBoxLayout* dl{nullptr};
    QWidget* dash_page{CenteredColumn(this, DASHBOARD_MAX_WIDTH, /*panel=*/false, dl)};
    m_dashboard_layout = dl;

    // Header: avatar | username, profile, balance | the profile and identity
    // buttons, top-aligned on the right.
    auto* header = new QHBoxLayout();
    header->setSpacing(12);
    m_avatar = new QLabel(dash_page);
    m_avatar->setFixedSize(AVATAR_SIZE, AVATAR_SIZE);
    header->addWidget(m_avatar, 0, Qt::AlignTop);
    auto* identity = new QVBoxLayout();
    identity->setSpacing(2);
    m_username = new QLabel(dash_page);
    GUIUtil::setFont({m_username}, GUIUtil::FontWeight::Bold, PlatformUi::SECTION_HEADING_SIZE);
    m_display_name = new QLabel(dash_page);
    m_display_name->setTextFormat(Qt::PlainText); // profile text, not markup
    m_message = PlatformUi::makeHint({}, dash_page);
    m_message->setTextFormat(Qt::PlainText);
    m_balance = PlatformUi::makeHint({}, dash_page);
    identity->addWidget(m_username);
    identity->addWidget(m_display_name);
    identity->addWidget(m_message);
    identity->addWidget(m_balance);
    header->addLayout(identity, /*stretch=*/1);
    auto* header_actions = new QHBoxLayout();
    header_actions->setSpacing(TITLE_SPACING);
    m_edit_profile_button = new QPushButton(dash_page);
    PlatformUi::makeSecondary(m_edit_profile_button);
    connect(m_edit_profile_button, &QPushButton::clicked, this, [this] {
        if (!m_service) return;
        ProfileDialog dialog(*m_service, this);
        if (dialog.exec() == QDialog::Accepted) {
            m_saved_line->setTransientMessage(PlatformUi::MessageLine::Severity::Success, tr("Profile saved."));
        }
        fetchDashboard();
    });
    header_actions->addWidget(m_edit_profile_button);
    header->addLayout(header_actions);
    header->setAlignment(header_actions, Qt::AlignTop | Qt::AlignRight);
    dl->addLayout(header);

    // Registration state, with its action under its text.
    m_state_card = MasternodeWidgetUtil::makeCard(dash_page);
    auto* card = new QVBoxLayout(m_state_card);
    card->setContentsMargins(CARD_PADDING, CARD_PADDING, CARD_PADDING, CARD_PADDING);
    card->setSpacing(TITLE_SPACING);
    auto* state_title_row = new QHBoxLayout();
    state_title_row->setSpacing(TITLE_SPACING);
    m_state_icon = new QLabel(m_state_card);
    m_state_icon->setFixedSize(NOTICE_ICON_SIZE, NOTICE_ICON_SIZE);
    m_state_title = new QLabel(m_state_card);
    m_state_title->setWordWrap(true);
    m_state_title->setTextFormat(Qt::PlainText);
    GUIUtil::setFont({m_state_title}, GUIUtil::FontWeight::Bold);
    state_title_row->addWidget(m_state_icon);
    state_title_row->addWidget(m_state_title, /*stretch=*/1);
    m_state_body = new QLabel(m_state_card);
    m_state_body->setWordWrap(true);
    m_state_body->setTextFormat(Qt::PlainText);
    m_state_error = new PlatformUi::MessageLine(m_state_card);
    m_state_error->setIconShown(false); // the title row carries it
    m_state_hint = PlatformUi::makeHint({}, m_state_card);
    m_state_busy = PlatformUi::makeBusyBar(m_state_card);
    m_state_button = new QPushButton(m_state_card);
    connect(m_state_button, &QPushButton::clicked, this, [this] {
        if (m_service && m_service->identityFlow().fundingWait() == IdentityFlow::FundingWait::RELEASED) {
            releaseFunding();
        } else {
            openWizard();
        }
    });
    card->addLayout(state_title_row);
    card->addWidget(m_state_body);
    card->addWidget(m_state_error);
    card->addWidget(m_state_hint);
    card->addWidget(m_state_busy);
    card->addWidget(m_state_button, 0, Qt::AlignLeft);
    dl->addWidget(m_state_card);
    m_saved_line = new PlatformUi::MessageLine(dash_page);
    dl->addWidget(m_saved_line);

    // Paused / frozen, in the overview's alert style.
    auto* alert_row = new QHBoxLayout();
    m_alert = new QLabel(dash_page);
    m_alert->setObjectName("labelAlerts");
    m_alert->setWordWrap(true);
    m_alert->setTextFormat(Qt::PlainText);
    m_alert_button = new QPushButton(tr("Turn network on"), dash_page);
    PlatformUi::makeSecondary(m_alert_button);
    connect(m_alert_button, &QPushButton::clicked, this, [this] { runNoticeAction(m_alert_action); });
    alert_row->addWidget(m_alert, /*stretch=*/1);
    alert_row->addWidget(m_alert_button, 0, Qt::AlignVCenter);
    dl->addLayout(alert_row);

    // The contacts are inserted here once the service exists; the column
    // leaves the spare height below them.
    m_contacts_index = dl->count();

    m_stack->addWidget(dash_page);

    // Coming back to the window counts as showing the page again.
    connect(qApp, &QGuiApplication::applicationStateChanged, this, [this](Qt::ApplicationState state) {
        if (state == Qt::ApplicationActive && isVisible()) fetchDashboardIfStale();
    });

    m_fetch_timer = new QTimer(this);
    m_fetch_timer->setInterval(DASHBOARD_FETCH_INTERVAL_MS);
    connect(m_fetch_timer, &QTimer::timeout, this, &PlatformPage::refreshVotesIfDue);
    // One pending retry per read: another failure restarts it rather than
    // adding a second.
    m_balance_retry = new QTimer(this);
    m_balance_retry->setSingleShot(true);
    m_balance_retry->setInterval(READ_RETRY_MS);
    connect(m_balance_retry, &QTimer::timeout, this, [this] {
        if (m_service && isVisible() && !m_paused && m_service->haveEndpoints()) m_service->refreshIdentityBalance();
    });
    m_profile_retry = new QTimer(this);
    m_profile_retry->setSingleShot(true);
    m_profile_retry->setInterval(READ_RETRY_MS);
    connect(m_profile_retry, &QTimer::timeout, this, [this] {
        if (!m_service || !isVisible() || m_paused || !m_service->haveEndpoints()) return;
        if (const auto id{m_service->myIdentityId()}) m_service->loadProfile(*id);
    });

    GUIUtil::updateFonts();
    SharedMnFitWrappedLabels(this);
    updateAvatar();
    for (int i = 0; i < m_step_badges.size(); ++i) {
        m_step_badges[i]->setPixmap(PlatformUi::stepBadgePixmap(i + 1, STEP_BADGE_SIZE, devicePixelRatioF()));
    }
}

PlatformPage::~PlatformPage()
{
    // The wizard works on the service, which goes first; the service stops
    // while what it reports to is whole.
    delete m_wizard;
    if (m_service) m_service->stop();
}

void PlatformPage::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() != QEvent::StyleChange) return;
    // Everything painted or styled from code follows the theme.
    PlatformUi::restyle(this);
    for (int i = 0; i < m_step_badges.size(); ++i) {
        m_step_badges[i]->setPixmap(PlatformUi::stepBadgePixmap(i + 1, STEP_BADGE_SIZE, devicePixelRatioF()));
    }
    refresh();
}

void PlatformPage::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    // Headings are bold from the first paint, as on the other tabs.
    if (!event->spontaneous()) GUIUtil::updateFonts();
    fetchDashboardIfStale();
}

void PlatformPage::fetchDashboardIfStale()
{
    // Switching tabs back and forth reads nothing new.
    if (GetTime() - m_last_fetch >= SHOW_FETCH_FLOOR_SECONDS) fetchDashboard();
}

void PlatformPage::refreshVotesIfDue()
{
    if (!m_service || !isVisible() || m_paused || !m_service->haveEndpoints()) return;
    const auto& rec{m_service->identityFlow().record()};
    if (rec.state != IdentityFlow::State::CONTESTED_PENDING || GetTime() - m_votes_read < VOTES_REFRESH_SECONDS) return;
    m_votes_read = GetTime();
    m_service->checkContestedNameState(QString::fromStdString(rec.normalized_label));
}

void PlatformPage::hideEvent(QHideEvent* event)
{
    // The wizard belongs to this page: switching to another page closes it
    // (the registration continues in the background), hiding the main
    // window to the tray does not.
    if (m_wizard && !event->spontaneous() && !window()->isHidden()) m_wizard->close();
    QWidget::hideEvent(event);
}

void PlatformPage::setWalletModel(WalletModel* wallet_model)
{
    walletModel = wallet_model;
    if (walletModel && walletModel->getOptionsModel()) {
        connect(walletModel->getOptionsModel(), &OptionsModel::displayUnitChanged, this, [this] {
            showBalance();
            refresh();
        });
    }
    refresh();
}

void PlatformPage::setClientModel(ClientModel* client_model)
{
    if (!client_model && m_service) {
        // Detached at shutdown, before the wallet and client models are
        // deleted: the service stops while the models it holds still exist.
        // Nothing that holds it is deleted here: a nested event loop (a
        // passphrase prompt, a dialog opened from the contacts) may be
        // running inside one of them. The wizard goes once control is back
        // in the main loop, the contacts page and the service with the page.
        // What runs meanwhile (the wizard finishing) refreshes nothing.
        clientModel = nullptr;
        if (m_wizard) m_wizard->close();
        Q_EMIT platformServiceReady(nullptr);
        m_contacts_page->hide();
        m_service->stop();
        m_stopped_service = std::move(m_service);
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
        // What others change (a premium username's votes) is read again on
        // a new block while the page is shown.
        connect(clientModel, &ClientModel::chainLockChanged, this, &PlatformPage::refreshVotesIfDue);
    }
    refresh();
}

void PlatformPage::maybeCreateService()
{
    if (m_service || m_client_failed || !walletModel || !clientModel) return;
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
    auto client{m_make_client(config)};
    // The settings it failed on are fixed until restart: not tried again.
    m_client_failed = !client;
    if (!client) return;
    m_service = std::make_unique<PlatformService>(*walletModel, *clientModel, std::move(client), *route, this);
    connectService();
    Q_EMIT platformServiceReady(m_service.get());
}

void PlatformPage::connectService()
{
    connect(m_service.get(), &PlatformService::unsupportedProtocolVersion, this, &PlatformPage::refresh);
    connect(m_service.get(), &PlatformService::reachabilityChanged, this, &PlatformPage::refresh);
    // Network activity back on (or sync done): what the dashboard read while
    // Platform was out of reach is read again once there are endpoints.
    connect(m_service.get(), &PlatformService::endpointsAvailable, this, &PlatformPage::fetchDashboard);
    connect(m_service.get(), &PlatformService::identityStateChanged, this, [this] {
        refresh();
        // Registration just completed: the dashboard has nothing loaded yet.
        if (isVisible() && m_service->identityFlow().record().state == IdentityFlow::State::REGISTERED) {
            fetchDashboard();
        }
    });
    connect(m_service.get(), &PlatformService::contestedNameState, this,
            [this](const QString& normalized_label, const platform::ContestedNameState& state, const QString& error) {
                using State = IdentityFlow::State;
                if (!m_service || m_service->identityFlow().record().state != State::CONTESTED_PENDING) return;
                if (QString::fromStdString(m_service->identityFlow().record().normalized_label) != normalized_label)
                    return;
                if (!error.isEmpty() || state.outcome != platform::ContestedNameState::Outcome::OPEN) return;
                uint32_t my_votes{0}, best_other{0};
                const auto my_id{m_service->identityFlow().record().identity_id};
                for (const auto& contender : state.contenders) {
                    if (contender.identity == my_id) {
                        my_votes = contender.votes;
                    } else {
                        best_other = std::max(best_other, contender.votes);
                    }
                }
                m_votes = tr("Votes so far: you %1 · highest other %2 · abstain %3 · lock %4")
                              .arg(my_votes)
                              .arg(best_other)
                              .arg(state.abstain_votes)
                              .arg(state.lock_votes);
                if (state.ends_at > 0) {
                    m_votes += QLatin1Char('\n') +
                               tr("Voting ends %1").arg(GUIUtil::dateTimeStr(static_cast<qint64>(state.ends_at / 1000)));
                }
                m_votes += QLatin1Char('\n') + tr("Updated at %1.").arg(QLocale().toString(QTime::currentTime(),
                                                                                         QLocale::ShortFormat));
                refresh();
            });
    connect(m_service.get(), &PlatformService::profileLoaded, this,
            [this](const QString& identity_hex, const QString& display_name, const QString& public_message) {
                const auto my_id{m_service->myIdentityId()};
                if (!my_id || QString::fromStdString(HexStr(*my_id)) != identity_hex) return;
                m_profile_display_name = display_name;
                m_have_profile = !display_name.isEmpty() || !public_message.isEmpty();
                m_profile_failed = false;
                m_display_name->setText(display_name);
                if (*m_have_profile) {
                    m_message->setText(public_message);
                } else if (m_service->identityFlow().profilePending()) {
                    // The profile chosen at registration is not on Platform yet.
                    m_message->setText(tr("Publishing your profile to Dash Platform…"));
                } else {
                    m_message->setText(tr("No profile yet. Add a display name so people recognize you."));
                }
                refresh();
            });
    connect(m_service.get(), &PlatformService::profileLoadFailed, this,
            [this](const QString& identity_hex, const QString& error, const QString& details) {
                const auto my_id{m_service->myIdentityId()};
                if (!my_id || QString::fromStdString(HexStr(*my_id)) != identity_hex) return;
                // Keep any previously loaded profile text; only replace the
                // loading placeholder so the header never sticks on "Loading".
                if (!m_have_profile) {
                    m_profile_failed = true;
                    m_message->setText(tr("Your profile could not be loaded. Dash Core will try again."));
                    m_message->setToolTip(error + QLatin1Char('\n') + details);
                    refresh();
                }
                if (isVisible()) m_profile_retry->start();
            });
    connect(m_service.get(), &PlatformService::identityBalanceLoaded, this, [this](quint64 credits) {
        m_credits = credits;
        showBalance();
        refresh();
    });
    connect(m_service.get(), &PlatformService::identityBalanceFailed, this,
            [this](const QString& error, const QString& details) {
                // A figure read before stays; it is read again shortly.
                if (!m_credits) {
                    m_balance->setText(tr("Balance on Dash Platform: unavailable"));
                    m_balance->setToolTip(error + QLatin1Char('\n') + details);
                }
                if (isVisible()) m_balance_retry->start();
            });

    // Embed the contacts UI in the dashboard once the service exists.
    m_contacts_page = new ContactsPage(*m_service, this);
    connect(m_contacts_page, &ContactsPage::emptyChanged, this, &PlatformPage::refresh);
    m_dashboard_layout->insertWidget(m_contacts_index, m_contacts_page);
    GUIUtil::updateFonts();
    if (isVisible()) fetchDashboard();
}

void PlatformPage::fetchDashboard()
{
    using State = IdentityFlow::State;
    const auto state{m_service ? m_service->identityFlow().record().state : State::NONE};
    const bool registered{state == State::REGISTERED};
    // Nothing is read without endpoints: endpointsAvailable() fetches again
    // once they are pushed.
    if (!m_service || m_paused || !m_service->haveEndpoints() || !isVisible() ||
        (!registered && state != State::CONTESTED_PENDING && !m_service->identityFlow().record().AwaitsUsername())) {
        m_fetch_timer->stop();
        m_balance_retry->stop();
        m_profile_retry->stop();
        return;
    }
    m_last_fetch = GetTime();
    if (state == State::CONTESTED_PENDING) {
        m_votes_read = m_last_fetch;
        m_service->checkContestedNameState(QString::fromStdString(m_service->identityFlow().record().normalized_label));
    }
    // What the identity holds is shown for an identity still without a
    // username too: it pays for that username.
    m_service->refreshIdentityBalance();
    if (registered) {
        if (!m_have_profile) m_message->setText(tr("Loading profile…"));
        if (const auto id{m_service->myIdentityId()}) m_service->loadProfile(*id);
        m_contacts_page->refreshIfShown();
    }
    m_fetch_timer->start();
}

PlatformAvailability PlatformPage::availability() const
{
    if (!walletModel || !clientModel) return {};
    return PlatformService::Availability(*walletModel, *clientModel, m_service.get());
}

bool PlatformPage::holdsUnconsumedFunding() const
{
    if (m_service) return m_service->identityFlow().holdsUnconsumedFunding();
    return walletModel && PlatformService::HoldsUnconsumedFunding(walletModel->wallet());
}

QString PlatformPage::disableBlockedReason() const
{
    using FundingWait = IdentityFlow::FundingWait;
    // Without a running service the flow's view is not known.
    if (!m_service) return tr("You can turn DashPay off once the username registration has created your identity.");
    switch (m_service->identityFlow().fundingWait()) {
    case FundingWait::RELEASED:
        return tr("You can turn DashPay off once the username registration has ended. The network did not accept "
                  "its funding payment: release that payment on the DashPay tab to get its coins back.");
    case FundingWait::ENDING:
        return tr("You can turn DashPay off once the username registration has ended, which happens when the "
                  "payment that spent its funding coins is final. The DashPay tab says what to do if it does not "
                  "confirm.");
    case FundingWait::NONE:
        break;
    }
    return tr("You can turn DashPay off once your identity has been created, usually within a few minutes.");
}

void PlatformPage::releaseFunding()
{
    if (!m_service) return;
    QMessageBox box(this);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(tr("Release the funding payment"));
    box.setText(tr("Send the coins of the funding payment back to this wallet?"));
    box.setInformativeText(tr("This ends the username registration once the new payment is confirmed, and costs a "
                              "transaction fee. If the network accepts the funding payment first, the registration "
                              "continues instead and the new payment is not needed."));
    auto* release{box.addButton(tr("Release"), QMessageBox::AcceptRole)};
    auto* cancel{box.addButton(QMessageBox::Cancel)};
    box.setDefaultButton(cancel);
    box.setEscapeButton(cancel);
    box.exec();
    if (box.clickedButton() != release || !m_service) return;
    QString error;
    if (!m_service->identityFlow().releaseFunding(error)) {
        QMessageBox::warning(this, tr("Release the funding payment"), error);
    }
    refresh();
}

QString PlatformPage::registeredUsername() const { return m_service ? m_service->myUsername() : QString{}; }

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
    // A registration may have sent its funding payment while the dialog was
    // open; its record is the only way to recover that asset lock.
    if (holdsUnconsumedFunding()) {
        QMessageBox::warning(dialog_parent, tr("Disable DashPay"), disableBlockedReason());
        return false;
    }
    delete m_wizard;
    // Consumers drop their service pointer while it is still valid.
    Q_EMIT platformServiceReady(nullptr);
    delete m_contacts_page;
    m_contacts_page = nullptr;
    m_service.reset();
    m_profile_display_name.clear();
    m_have_profile.reset();
    m_profile_failed = false;
    m_credits.reset();
    m_votes.clear();
    for (QLabel* label : {m_display_name, m_message, m_balance}) {
        label->clear();
        label->setToolTip({});
    }
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

void PlatformPage::openWizard()
{
    if (!m_service) return;
    // A wizard still around (left open behind the window) shows an earlier
    // state: this click starts from the current one.
    delete m_wizard;
    using State = IdentityFlow::State;
    IdentityFlow& flow{m_service->identityFlow()};
    // A failed registration is cleared first so the wizard restarts from
    // name entry instead of dead-ending on the progress view; a flow parked
    // on the wallet passphrase asks for it again now.
    if (flow.record().state == State::FAILED) flow.reset();
    flow.retryAfterUnlock();
    m_wizard = new CreateUsernameWizard(*m_service, *walletModel, this);
    m_wizard->setAttribute(Qt::WA_DeleteOnClose);
    connect(m_wizard, &QDialog::finished, this, &PlatformPage::refresh);
    connect(m_wizard, &CreateUsernameWizard::addProfileRequested, m_edit_profile_button, &QPushButton::click,
            Qt::QueuedConnection);
    const auto& rec{flow.record()};
    // A recovered identity without a username starts at name entry like a
    // fresh registration.
    if (rec.state != State::NONE && rec.state != State::REGISTERED && !rec.AwaitsUsername()) {
        // A registration is already underway: jump straight to the progress
        // view instead of asking for a name again.
        m_wizard->startAtProgress();
    }
    // Window-modal to the main window, which every wallet shares.
    m_wizard->open();
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

QString PlatformPage::formatBalance(quint64 credits) const
{
    return PlatformUi::formatPlatformBalance(walletModel->getOptionsModel()->getDisplayUnit(), credits);
}

void PlatformPage::showBalance()
{
    if (!m_credits) return;
    m_balance->setText(tr("Balance on Dash Platform: %1").arg(formatBalance(*m_credits)));
    m_balance->setToolTip(tr("Dash held by your DashPay identity on Dash Platform. It pays the Dash Platform fees for "
                             "changes such as your profile and contact requests. It is separate from your wallet "
                             "balance and came from your username registration."));
}

void PlatformPage::updateAvatar()
{
    using State = IdentityFlow::State;
    // Neutral until the username is ours or up for the vote: a registration
    // may still lose it.
    QString username;
    if (m_service) {
        const auto& rec{m_service->identityFlow().record()};
        if (rec.state == State::REGISTERED || rec.state == State::CONTESTED_PENDING) {
            username = QString::fromStdString(rec.label);
        }
    }
    m_avatar->setPixmap(PlatformUi::avatarPixmap(username, m_profile_display_name, AVATAR_SIZE, devicePixelRatioF()));
    m_avatar->setAccessibleName(username.isEmpty() ? tr("Avatar") : tr("Avatar for %1").arg(username));
}

void PlatformPage::setNotice(const QString& icon, const QString& title, const QString& body, bool busy, NoticeAction action)
{
    m_notice->setVisible(!title.isEmpty());
    SetIcon(m_notice_icon, icon, IconColor(icon, /*error=*/false));
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

void PlatformPage::showWelcome(const PlatformAvailability& availability)
{
    using Gate = PlatformAvailability::Gate;
    m_stack->setCurrentIndex(0);
    const bool opted_in{availability.enabled};
    m_privacy_note->setVisible(!opted_in);
    m_settings_button->setVisible(opted_in);

    // What keeps DashPay from running, then what keeps a registration from
    // starting.
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
    bool blocked{availability.gate != Gate::NONE};
    if (!blocked && m_client_failed) {
        blocked = true;
        setNotice(QStringLiteral("warning"), tr("DashPay couldn't start"),
                  tr("DashPay couldn't use your network settings to connect to Dash Platform. The debug log has "
                     "the details."),
                  false, NoticeAction::NONE);
    }
    m_welcome_steps->setVisible(!blocked);
    m_enable_button->setVisible(!opted_in && availability.gate == Gate::NONE);
    m_create_button->setVisible(opted_in && m_service && !blocked);
    m_create_button->setEnabled(m_service && !m_service->writesFrozen());
}

void PlatformPage::refresh()
{
    if (!walletModel || !clientModel) return;
    if (!m_service) maybeCreateService();
    Q_EMIT dashPayStateChanged();

    const PlatformAvailability availability{PlatformService::Availability(*walletModel, *clientModel, m_service.get())};
    if (!m_service) {
        showWelcome(availability);
        return;
    }
    // A running service pauses while a gate is closed again (network
    // activity off, the node syncing) and resumes by itself.
    const bool was_paused{m_paused};
    m_paused = availability.gate != PlatformAvailability::Gate::NONE;
    if (m_paused) {
        m_balance_retry->stop();
        m_profile_retry->stop();
    }
    if (was_paused && !m_paused) {
        // Reads wait for the endpoints the node pushes again on resume
        // (endpointsAvailable() fetches then); with endpoints kept (a sync
        // pause) they are made now.
        if (m_service->haveEndpoints()) {
            fetchDashboard();
        } else {
            m_service->refreshNodeContext();
        }
    }

    if (m_service->identityFlow().record().state == IdentityFlow::State::NONE) {
        // A pause says why nothing can start.
        showWelcome(availability);
        return;
    }
    m_stack->setCurrentIndex(1);
    refreshDashboard(availability);
}

void PlatformPage::refreshDashboard(const PlatformAvailability& availability)
{
    using State = IdentityFlow::State;
    using Gate = PlatformAvailability::Gate;
    const auto& rec{m_service->identityFlow().record()};
    const QString label{QString::fromStdString(rec.label)};
    const bool registered{rec.state == State::REGISTERED};

    // One notice at a time: frozen writes, then a pause.
    const bool frozen{m_service->writesFrozen()};
    m_alert_action = NoticeAction::NONE;
    if (frozen) {
        m_alert->setText(tr("Dash Platform has been upgraded. You can still see your DashPay data, but update Dash "
                            "Core to register, edit your profile or add contacts."));
    } else if (m_paused) {
        m_alert->setText(availability.gate == Gate::NETWORK_INACTIVE
                             ? tr("DashPay is paused while network activity is turned off.")
                             : availability.reason);
        if (availability.gate == Gate::NETWORK_INACTIVE) m_alert_action = NoticeAction::TURN_NETWORK_ON;
    }
    m_alert->setVisible(frozen || m_paused);
    m_alert_button->setVisible(!m_alert->isHidden() && m_alert_action != NoticeAction::NONE);
    const bool writable{!frozen && !m_paused};

    // Header.
    updateAvatar();
    m_username->setText(label.isEmpty() ? tr("No username yet") : label);
    m_username->setToolTip(QString::fromStdString(rec.normalized_label));
    m_display_name->setVisible(registered && !m_display_name->text().isEmpty());
    m_message->setVisible(registered && !m_message->text().isEmpty());
    m_balance->setVisible(m_credits.has_value() || !m_balance->text().isEmpty());
    m_edit_profile_button->setVisible(registered);
    // The profile chosen at registration is still going out.
    const bool profile_pending{m_service->identityFlow().profilePending()};
    // Only a profile read (or proved absent) can be edited without losing
    // what another wallet set; a failed read offers what it will be once one
    // succeeds.
    const bool profile_known{m_have_profile.has_value()};
    m_edit_profile_button->setText((m_have_profile.value_or(true) && !m_profile_failed) || profile_pending
                                       ? tr("Edit profile…")
                                       : tr("Add profile…"));
    m_edit_profile_button->setEnabled(writable && profile_known && !profile_pending);
    m_edit_profile_button->setToolTip(profile_pending ? tr("Your profile is being published to Dash Platform.")
                                                      : tr("Set the display name and message other DashPay users see"));

    // State card.
    m_state_card->setVisible(!registered);
    m_state_error->clear();
    m_state_hint->hide();
    m_state_busy->hide();
    QString icon;
    bool error_icon{false};
    const auto title_label{label.isEmpty() ? tr("No username yet") : label};
    m_state_button->setEnabled(true);
    if (rec.state == State::FAILED) {
        icon = QStringLiteral("warning");
        error_icon = true;
        m_state_title->setText(tr("Registration did not finish"));
        m_state_body->hide();
        m_state_error->setMessage(PlatformUi::MessageLine::Severity::Error, m_service->identityFlow().lastErrorText(),
                                  m_service->identityFlow().lastErrorDetails());
        m_state_hint->setText(PlatformUi::failureReassurance(rec));
        m_state_hint->show();
        m_state_button->setText(tr("Try again…"));
        m_state_button->setEnabled(writable);
    } else if (rec.AwaitsUsername()) {
        m_state_title->setText(tr("Choose a username to finish setting up DashPay"));
        m_state_body->setText(m_credits ? tr("Your DashPay identity is ready, with %1 on Dash Platform to pay for a "
                                             "username. Choose one so people can find you. No new payment is needed.")
                                              .arg(formatBalance(*m_credits))
                                        : tr("Your DashPay identity is ready. Choose a username so people can find "
                                             "you. No new payment is needed."));
        m_state_body->show();
        m_state_button->setText(tr("Choose a username…"));
        m_state_button->setEnabled(writable);
    } else if (rec.state == State::CONTESTED_PENDING) {
        icon = QStringLiteral("voting");
        m_state_title->setText(tr("Masternodes are voting on “%1”").arg(title_label));
        m_state_body->setText(tr("“%1” is a premium username, so masternodes vote on who gets it. This can take up to "
                                 "two weeks, and someone else may win it. The result will appear here.")
                                  .arg(title_label));
        m_state_body->show();
        m_state_hint->setText(m_votes);
        m_state_hint->setVisible(!m_votes.isEmpty());
    } else if (rec.state == State::NEEDS_UNLOCK) {
        icon = QStringLiteral("lock_closed");
        m_state_title->setText(tr("Waiting for your passphrase"));
        m_state_body->setText(tr("The next registration step signs with your wallet's keys. Unlock the wallet to "
                                 "continue."));
        m_state_body->show();
        m_state_hint->setText(PlatformUi::registrationStepLine(rec));
        m_state_hint->setVisible(!m_state_hint->text().isEmpty());
        m_state_button->setText(tr("Unlock and continue…"));
        m_state_button->setEnabled(writable);
    } else if (const auto wait{m_service->identityFlow().fundingWait()}; wait != IdentityFlow::FundingWait::NONE) {
        icon = QStringLiteral("warning");
        if (wait == IdentityFlow::FundingWait::RELEASED) {
            m_state_title->setText(tr("The payment for “%1” was not accepted").arg(title_label));
            m_state_body->setText(tr("The network did not accept the payment that funds your DashPay identity, so "
                                     "Dash Core released its coins. The registration continues if the payment is "
                                     "accepted after all. To end it instead, release the payment: its coins come "
                                     "back to this wallet, less a transaction fee, and you can start again."));
            m_state_button->setText(tr("Release the funding payment…"));
        } else {
            m_state_title->setText(tr("The registration of “%1” is ending").arg(title_label));
            m_state_body->setText(tr("A payment that spends the coins of the funding payment was sent. The "
                                     "registration ends once that payment is final, and you can then start again. "
                                     "If that payment does not confirm, abandon it on the Transactions tab to get its "
                                     "coins back."));
            m_state_busy->setVisible(!m_paused);
        }
        m_state_body->show();
    } else if (!registered) {
        m_state_title->setText(tr("Registering “%1”").arg(title_label));
        m_state_body->setText(tr("This usually takes a few minutes. You can keep using the wallet in the meantime."));
        m_state_body->show();
        m_state_hint->setText(PlatformUi::registrationStepLine(rec));
        m_state_hint->setVisible(!m_state_hint->text().isEmpty());
        m_state_busy->setVisible(!m_paused);
        m_state_button->setText(tr("Show progress…"));
    }
    // The votes are read again by themselves, and a payment that spent the
    // funding coins ends the registration: the card has no action then.
    m_state_button->setVisible(rec.state != State::CONTESTED_PENDING &&
                               m_service->identityFlow().fundingWait() != IdentityFlow::FundingWait::ENDING);
    m_state_icon_name = icon;
    SetIcon(m_state_icon, icon, IconColor(icon, error_icon));
    m_state_card->updateGeometry();

    // The contacts show once there is something to show.
    if (m_contacts_page) {
        m_contacts_page->setVisible(registered || !m_contacts_page->isEmpty());
        m_contacts_page->setEnabled(!m_paused);
    }
}
