// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/platformpage.h>

#include <base58.h>
#include <qt/bitcoinamountfield.h>
#include <qt/bitcoinunits.h>
#include <qt/guiutil.h>
#include <qt/optionsmodel.h>
#include <qt/platform/contactflow.h>
#include <qt/platform/contactspage.h>
#include <qt/platform/identityflow.h>
#include <qt/platform/platformui.h>
#include <qt/platform/recoveryflow.h>
#include <qt/platform/usernameflow.h>
#include <qt/walletmodel.h>

#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>

namespace {
//! The masternode vote on our pending username, as the mobile wallets word it.
QString ContestStatus(const platform::VoteState& state, const platform::Identifier& ours)
{
    using Winner = platform::VoteState::Winner;
    if (state.winner == Winner::LOCKED) return PlatformPage::tr("Locked — nobody receives this username");
    if (state.winner == Winner::IDENTITY) {
        return state.winner_identity == ours ? PlatformPage::tr("You won this username")
                                             : PlatformPage::tr("This username went to someone else");
    }
    uint32_t mine{0}, others{0};
    for (const auto& contender : state.contenders) {
        if (contender.identity == ours) {
            mine = contender.votes;
        } else {
            others = std::max(others, contender.votes);
        }
    }
    QString status{PlatformPage::tr("Voting in progress")};
    if (state.lock > std::max(mine, others)) {
        status = PlatformPage::tr("Voting in progress — lock votes are ahead");
    } else if (mine > std::max(others, state.lock)) {
        status = PlatformPage::tr("Voting in progress — you are ahead");
    }
    return PlatformPage::tr("%1. Votes: you %2, best other contender %3, lock %4, abstain %5.")
        .arg(status)
        .arg(mine)
        .arg(others)
        .arg(state.lock)
        .arg(state.abstain);
}
} // namespace

PlatformPage::PlatformPage(PlatformService::Sources sources, QWidget* parent) :
    QWidget(parent),
    m_sources(std::move(sources))
{
    auto* layout{new QVBoxLayout(this)};
    auto* title{new QLabel(tr("DashPay"), this)};
    GUIUtil::setFont({title}, GUIUtil::FontWeight::Bold, 18);
    m_username = new QLabel(this);
    m_username->setObjectName("dashPayUsername");
    m_username->setTextInteractionFlags(Qt::TextSelectableByMouse | Qt::LinksAccessibleByMouse);
    GUIUtil::setFont({m_username}, GUIUtil::FontWeight::Bold);
    m_identity = new QLabel(this);
    m_identity->setObjectName("dashPayIdentity");
    m_identity->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_contest = new QLabel(this);
    m_contest->setObjectName("dashPayContestStatus");
    m_contest->setWordWrap(true);
    auto* notice{new QLabel(tr("DashPay connects to evonodes, which see your IP address unless you use a proxy."), this)};
    notice->setObjectName("dashPayNotice");
    notice->setWordWrap(true);
    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    m_identity_status = new QLabel(this);
    m_identity_status->setObjectName("dashPayIdentityStatus");
    m_identity_status->setWordWrap(true);
    m_recovery_status = new QLabel(this);
    m_recovery_status->setObjectName("dashPayRecoveryStatus");
    m_recovery_status->setWordWrap(true);

    m_find = new QPushButton(tr("Find my identity"), this);
    m_find->setObjectName("dashPayFindIdentity");
    m_create = new QPushButton(tr("Create identity"), this);
    m_create->setObjectName("dashPayCreateIdentity");
    m_continue = new QPushButton(tr("Continue"), this);
    m_continue->setObjectName("dashPayContinue");
    m_top_up_amount = new BitcoinAmountField(this);
    m_top_up_amount->setObjectName("dashPayTopUpAmount");
    m_top_up_amount->SetMinValue(IdentityFlow::MIN_TOP_UP);
    m_top_up_amount->setValue(IdentityFlow::MIN_TOP_UP);
    m_top_up = new QPushButton(tr("Top up"), this);
    m_top_up->setObjectName("dashPayTopUp");
    auto* actions{new QHBoxLayout};
    actions->addWidget(m_find);
    actions->addWidget(m_create);
    actions->addWidget(m_continue);
    actions->addWidget(m_top_up_amount);
    actions->addWidget(m_top_up);
    actions->addStretch();

    m_username_input = new QLineEdit(this);
    m_username_input->setObjectName("dashPayUsernameInput");
    m_username_input->setPlaceholderText(tr("Username"));
    m_register = new QPushButton(tr("Register username"), this);
    m_register->setObjectName("dashPayRegisterUsername");
    m_username_status = new QLabel(this);
    m_username_status->setObjectName("dashPayUsernameStatus");
    m_username_status->setWordWrap(true);
    auto* username_row{new QHBoxLayout};
    username_row->addWidget(m_username_input);
    username_row->addWidget(m_register);
    username_row->addStretch();

    m_contacts = new ContactsPage(this);
    m_contacts->setObjectName("dashPayContactsPage");

    auto* header{new QHBoxLayout};
    header->addWidget(m_username);
    header->addWidget(m_identity);
    header->addStretch();

    m_stop = new QPushButton(tr("Turn off DashPay for this wallet"), this);
    m_stop->setObjectName("dashPayStop");
    layout->addWidget(title);
    layout->addLayout(header);
    layout->addWidget(m_contest);
    layout->addWidget(notice);
    layout->addWidget(m_status);
    layout->addWidget(m_identity_status);
    layout->addWidget(m_recovery_status);
    layout->addLayout(actions);
    layout->addWidget(m_username_status);
    layout->addLayout(username_row);
    layout->addWidget(m_contacts);
    layout->addWidget(m_stop, 0, Qt::AlignLeft);
    layout->addStretch();
    GUIUtil::updateFonts();

    connect(m_stop, &QPushButton::clicked, this, [this] {
        if (!m_service->stop()) {
            QMessageBox::warning(this, tr("DashPay"), tr("The wallet could not save that DashPay is turned off."));
        }
    });
    connect(m_find, &QPushButton::clicked, this, [this] { m_recovery_flow->find(); });
    // Only the pending username is a link.
    connect(m_username, &QLabel::linkActivated, this, [this] {
        if (const auto& pending{m_recovery_flow->pendingName()}) m_username_flow->readContest(*pending);
    });
    connect(m_create, &QPushButton::clicked, this, [this] { m_identity_flow->create(); });
    connect(m_continue, &QPushButton::clicked, this, [this] { m_identity_flow->resume(); });
    const auto register_name = [this] {
        m_username_flow->registerName(m_username_input->text().trimmed().toStdString());
    };
    connect(m_register, &QPushButton::clicked, this, register_name);
    connect(m_username_input, &QLineEdit::returnPressed, this, register_name);
    connect(m_top_up, &QPushButton::clicked, this, [this] {
        bool valid{false};
        const CAmount amount{m_top_up_amount->value(&valid)};
        if (valid && amount >= IdentityFlow::MIN_TOP_UP) m_identity_flow->topUp(amount);
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
    if (m_wallet_model) m_top_up_amount->setDisplayUnit(m_wallet_model->getOptionsModel()->getDisplayUnit());
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
        m_contacts->setFlow(nullptr);
        m_recovery_flow.reset();
        m_contact_flow.reset();
        m_username_flow.reset();
        m_identity_flow.reset();
        m_service.reset();
    }
    if (m_wallet_model && m_client_model) {
        m_service = std::make_unique<PlatformService>(*m_wallet_model, *m_client_model, m_sources);
        connect(m_service.get(), &PlatformService::changed, this, &PlatformPage::refresh);
        m_identity_flow = std::make_unique<IdentityFlow>(*m_service, [this](CAmount amount, CAmount fee) {
            return confirmFunding(amount, fee);
        });
        connect(m_identity_flow.get(), &IdentityFlow::changed, this, &PlatformPage::refresh);
        m_username_flow = std::make_unique<UsernameFlow>(*m_service, *m_identity_flow,
                                                         [this] { return confirmContest(); });
        connect(m_username_flow.get(), &UsernameFlow::changed, this, &PlatformPage::refresh);
        m_contact_flow = std::make_unique<ContactFlow>(*m_service, *m_identity_flow);
        m_contacts->setFlow(m_contact_flow.get());
        connect(m_contact_flow.get(), &ContactFlow::changed, this, &PlatformPage::refresh);
        connect(m_contact_flow.get(), &ContactFlow::payTo, this, &PlatformPage::payTo);
        m_recovery_flow = std::make_unique<RecoveryFlow>(*m_service, *m_identity_flow, *m_username_flow, *m_contact_flow);
        connect(m_recovery_flow.get(), &RecoveryFlow::changed, this, &PlatformPage::refresh);
        Q_EMIT serviceChanged(m_service.get());
    }
    refresh();
}

bool PlatformPage::confirmFunding(CAmount amount, CAmount fee)
{
    const BitcoinUnit unit{m_wallet_model->getOptionsModel()->getDisplayUnit()};
    return QMessageBox::question(this, tr("Confirm DashPay payment"),
                                 tr("Send %1 to Dash Platform as identity credits?\n\nNetwork fee: %2\nTotal: %3")
                                     .arg(BitcoinUnits::formatWithUnit(unit, amount),
                                          BitcoinUnits::formatWithUnit(unit, fee),
                                          BitcoinUnits::formatWithUnit(unit, amount + fee)),
                                 QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes;
}

bool PlatformPage::confirmContest()
{
    const BitcoinUnit unit{m_wallet_model->getOptionsModel()->getDisplayUnit()};
    return QMessageBox::warning(this, tr("Contested name"),
                                tr("This name requires a masternode vote. The contest fee of %1 is spent when you "
                                   "submit and is not returned, even if you do not win the name.")
                                    .arg(PlatformUi::FormatCredits(unit, UsernameFlow::CONTEST_FUND)),
                                QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel) == QMessageBox::Yes;
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
    m_stop->setVisible(m_service && m_service->consented());

    const IdentityFlow* flow{m_identity_flow.get()};
    const bool running{flow && m_service->client()};
    const BitcoinUnit unit{m_wallet_model ? m_wallet_model->getOptionsModel()->getDisplayUnit() : BitcoinUnit::DASH};
    const platform::Identity* identity{flow && flow->identity() ? &*flow->identity() : nullptr};
    if (identity) {
        m_identity->setText(tr("Identity %1 · Balance %2")
                                .arg(QString::fromStdString(EncodeBase58(identity->id)),
                                     PlatformUi::FormatCredits(unit, identity->balance)));
    }
    m_identity->setVisible(identity != nullptr);
    const UsernameFlow* names{m_username_flow.get()};
    const auto username{names && identity ? names->username() : std::nullopt};
    const RecoveryFlow* recovery{m_recovery_flow.get()};
    const auto pending{recovery && identity && !username ? recovery->pendingName() : std::nullopt};
    if (pending) {
        m_username->setTextFormat(Qt::RichText);
        m_username->setText(
            tr("%1 (waiting for the masternode vote)")
                .arg(QStringLiteral("<a href=\"contest\">%1</a>").arg(QString::fromStdString(*pending).toHtmlEscaped())));
    } else {
        m_username->setTextFormat(Qt::PlainText);
        m_username->setText(QString::fromStdString(username.value_or(std::string{})));
    }
    m_username->setVisible(username || pending);
    const auto contest{pending ? names->contest() : std::nullopt};
    if (contest) m_contest->setText(ContestStatus(*contest, identity->id));
    m_contest->setVisible(contest.has_value());

    using Step = IdentityFlow::Step;
    const Step step{flow ? flow->step() : Step::IDLE};
    const auto funding{running ? flow->funding() : std::nullopt};
    using NameStep = UsernameFlow::Step;
    const NameStep name_step{names ? names->step() : NameStep::IDLE};
    const ContactFlow* contacts{m_contact_flow.get()};
    const bool idle{running && step == Step::IDLE && name_step == NameStep::IDLE && contacts &&
                    contacts->step() == ContactFlow::Step::IDLE && recovery &&
                    recovery->step() == RecoveryFlow::Step::IDLE};
    //! Nothing in flight and no funding waiting for Platform.
    const bool ready{idle && !funding};
    QString identity_status;
    switch (step) {
    case Step::IDLE:
        if (!running) break;
        if (!flow->error().isEmpty()) {
            identity_status = flow->error();
        } else if (funding) {
            identity_status = tr("A funding payment of %1 has not reached Dash Platform yet.")
                                  .arg(BitcoinUnits::formatWithUnit(unit, funding->amount));
        } else if (!flow->lookedUp()) {
            identity_status = tr("Unlock the wallet so DashPay can find your identity.");
        } else if (!identity) {
            identity_status = tr("This wallet has no DashPay identity yet. Creating one buys %1 of credits.")
                                  .arg(BitcoinUnits::formatWithUnit(unit, IdentityFlow::REGISTRATION_AMOUNT));
        }
        break;
    case Step::LOOKING_UP:
        identity_status = tr("Looking up your DashPay identity…");
        break;
    case Step::FUNDING:
        identity_status = tr("Preparing the funding payment…");
        break;
    case Step::WAITING_FOR_LOCK:
        identity_status = tr("Waiting for the network to lock the funding payment…");
        break;
    case Step::SENDING:
        identity_status = funding && funding->top_up ? tr("Adding the credits to your identity…")
                                                     : tr("Creating your DashPay identity…");
        break;
    }
    m_identity_status->setText(identity_status);
    m_identity_status->setVisible(!identity_status.isEmpty());
    // The first DashPay click, and on a locked wallet the one each session.
    m_find->setVisible((m_service && gate == Gate::NO_CONSENT) || (ready && !flow->lookedUp()));
    m_create->setVisible(ready && flow->lookedUp() && !identity);
    m_continue->setVisible(idle && funding.has_value());
    m_top_up_amount->setVisible(ready && identity);
    m_top_up->setVisible(ready && identity);

    QString username_status;
    switch (name_step) {
    case NameStep::IDLE:
        if (running) username_status = names->error();
        break;
    case NameStep::CHECKING:
        username_status = tr("Checking that the username is free…");
        break;
    case NameStep::PREPARING:
        // Until the identity exists its own status says what runs.
        if (identity) username_status = tr("Registering your username…");
        break;
    case NameStep::REGISTERING:
        username_status = tr("Registering your username…");
        break;
    }
    m_username_status->setText(username_status);
    m_username_status->setVisible(!username_status.isEmpty());
    const bool can_register{ready && flow->lookedUp() &&
                            (!identity || (names->namesRead() && !username && recovery->pendingRead() && !pending))};
    m_username_input->setVisible(can_register);
    m_register->setVisible(can_register);

    QString recovery_status;
    if (recovery) {
        if (recovery->step() == RecoveryFlow::Step::FINDING && step != Step::LOOKING_UP) {
            recovery_status = tr("Reading your DashPay contacts…");
        } else if (!recovery->error().isEmpty()) {
            recovery_status = recovery->error();
        } else if (recovery->rescanning()) {
            recovery_status = tr("Rescanning the wallet for payments from your DashPay contacts…");
        } else if (recovery->prunedFrom()) {
            recovery_status = tr("Payments from your DashPay contacts need a wallet rescan from block %1, but this "
                                 "node has pruned those blocks. To see them, turn off pruning and restart Dash Core "
                                 "with -reindex.")
                                  .arg(*recovery->prunedFrom());
        }
    }
    m_recovery_status->setText(recovery_status);
    m_recovery_status->setVisible(!recovery_status.isEmpty());

    // Contacts find the identity by its username.
    m_contacts->setVisible(running && identity && username);
    m_contacts->refresh(ready);
}
