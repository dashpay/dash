// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/createusernamewizard.h>

#include <platform/helpers.h>
#include <qt/bitcoinunits.h>
#include <qt/guiutil.h>
#include <qt/masternodewidgets.h>
#include <qt/optionsmodel.h>
#include <qt/platform/identityflow.h>
#include <qt/platform/platformservice.h>
#include <qt/platform/platformui.h>
#include <qt/sharedmnwidgets.h>
#include <qt/walletmodel.h>

#include <QEvent>
#include <QFormLayout>
#include <QFrame>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QShowEvent>
#include <QStackedWidget>
#include <QTime>
#include <QTimer>
#include <QVBoxLayout>

using MasternodeWidgetUtil::CARD_PADDING;
using MasternodeWidgetUtil::GROUP_SPACING;
using MasternodeWidgetUtil::ROW_SPACING;
using MasternodeWidgetUtil::TITLE_SPACING;
using Severity = PlatformUi::MessageLine::Severity;
using State = IdentityFlow::State;

namespace {
constexpr int PAGE_ENTRY{0};
constexpr int PAGE_COST{1};
constexpr int PAGE_PROGRESS{2};
constexpr int PAGE_COUNT{3};
constexpr int DEBOUNCE_MS{400};
constexpr int MAX_LABEL_LENGTH{63};
constexpr int STEP_ICON_SIZE{16};

//! A page's layout: the dialog supplies the margins.
QVBoxLayout* PageLayout(QWidget* page, const QString& title, const QString& subtitle, QLabel** title_label = nullptr,
                        QLabel** subtitle_label = nullptr)
{
    auto* layout{MasternodeWidgetUtil::makePageLayout(page)};
    auto* block{MasternodeWidgetUtil::makeBlock(layout)};
    auto* heading{MasternodeWidgetUtil::makePageTitle(title, page)};
    block->addWidget(heading);
    auto* hint{PlatformUi::makeHint(subtitle, page)};
    hint->setVisible(!subtitle.isEmpty());
    block->addWidget(hint);
    if (title_label) *title_label = heading;
    if (subtitle_label) *subtitle_label = hint;
    return layout;
}

bool IsTerminal(State state)
{
    return state == State::REGISTERED || state == State::CONTESTED_PENDING || state == State::FAILED;
}
} // namespace

// ---- Wizard -----------------------------------------------------------------

CreateUsernameWizard::CreateUsernameWizard(PlatformService& service, WalletModel& wallet_model, QWidget* parent) :
    QDialog(parent, GUIUtil::dialog_flags),
    m_service(service)
{
    setWindowTitle(tr("Register a DashPay username"));
    setMinimumSize(520, 420);

    m_entry = new UsernameEntryPage(service, this);
    m_cost = new UsernameCostPage(service, wallet_model, *m_entry, this);
    m_progress = new UsernameProgressPage(service, this);

    m_step_hint = PlatformUi::makeHint({}, this);
    m_stack = new QStackedWidget(this);
    m_stack->insertWidget(PAGE_ENTRY, m_entry);
    m_stack->insertWidget(PAGE_COST, m_cost);
    m_stack->insertWidget(PAGE_PROGRESS, m_progress);
    m_busy = PlatformUi::makeBusyBar(this);

    m_back = new QPushButton(tr("Back"), this);
    PlatformUi::makeSecondary(m_back);
    m_cancel = new QPushButton(this);
    PlatformUi::makeSecondary(m_cancel);
    m_secondary = new QPushButton(tr("Add profile…"), this);
    PlatformUi::makeSecondary(m_secondary);
    m_next = new QPushButton(this);
    for (QPushButton* button : {m_back, m_cancel, m_secondary}) {
        button->setAutoDefault(false);
    }

    auto* buttons = new QHBoxLayout();
    buttons->addWidget(m_back);
    buttons->addStretch();
    buttons->addWidget(m_cancel);
    buttons->addWidget(m_secondary);
    buttons->addWidget(m_next);

    // The dialog owns the page margins; the pages themselves use none so the
    // two do not add up.
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 12, 24, 12);
    layout->setSpacing(GROUP_SPACING);
    layout->addWidget(m_step_hint);
    layout->addWidget(m_stack, /*stretch=*/1);
    layout->addWidget(m_busy);
    layout->addLayout(buttons);

    connect(m_back, &QPushButton::clicked, this, &CreateUsernameWizard::onBack);
    connect(m_next, &QPushButton::clicked, this, &CreateUsernameWizard::onNext);
    connect(m_cancel, &QPushButton::clicked, this, &QDialog::reject);
    connect(m_secondary, &QPushButton::clicked, this, [this] {
        accept();
        Q_EMIT addProfileRequested();
    });
    connect(&m_service, &PlatformService::identityStateChanged, this, &CreateUsernameWizard::updateButtons);
    for (UsernameWizardPage* page : {static_cast<UsernameWizardPage*>(m_entry), static_cast<UsernameWizardPage*>(m_cost),
                                     static_cast<UsernameWizardPage*>(m_progress)}) {
        connect(page, &UsernameWizardPage::completeChanged, this, &CreateUsernameWizard::updateButtons);
    }

    GUIUtil::updateFonts();
    GUIUtil::disableMacFocusRect(this);
    SharedMnFitWrappedLabels(this);
    setCurrentPage(PAGE_ENTRY);
}

void CreateUsernameWizard::startAtProgress() { setCurrentPage(PAGE_PROGRESS); }

void CreateUsernameWizard::changeEvent(QEvent* event)
{
    QDialog::changeEvent(event);
    if (event->type() == QEvent::StyleChange) PlatformUi::restyle(this);
}

bool CreateUsernameWizard::event(QEvent* event)
{
    const bool handled{QDialog::event(event)};
    // A window's layout does not size it for wrapped text: a message that
    // needs more lines than there is room for grows the window, rather than
    // being cut off. It never shrinks under the user.
    if (event->type() == QEvent::LayoutRequest && isVisible()) {
        const int wanted{heightForWidth(width())};
        if (wanted > height()) resize(width(), wanted);
    }
    return handled;
}

void CreateUsernameWizard::showEvent(QShowEvent* event)
{
    QDialog::showEvent(event);
    // Page titles are bold from the first paint.
    if (!event->spontaneous()) GUIUtil::updateFonts();
}

UsernameWizardPage* CreateUsernameWizard::currentPage() const
{
    return static_cast<UsernameWizardPage*>(m_stack->currentWidget());
}

void CreateUsernameWizard::setCurrentPage(int id, bool initialize)
{
    m_stack->setCurrentIndex(id);
    if (initialize) currentPage()->initializePage();
    updateButtons();
}

void CreateUsernameWizard::onBack()
{
    // Only the cost page has a back button. Skip initializePage on the way
    // back so the entry page keeps its state, matching QWizard semantics.
    if (m_stack->currentIndex() == PAGE_COST) setCurrentPage(PAGE_ENTRY, /*initialize=*/false);
}

void CreateUsernameWizard::onNext()
{
    IdentityFlow& flow{m_service.identityFlow()};
    switch (m_stack->currentIndex()) {
    case PAGE_ENTRY:
        if (!m_entry->validatePage()) return;
        setCurrentPage(m_entry->ours() ? PAGE_PROGRESS : PAGE_COST);
        break;
    case PAGE_COST:
        if (!m_cost->validatePage()) return;
        setCurrentPage(PAGE_PROGRESS);
        break;
    case PAGE_PROGRESS:
        switch (flow.record().state) {
        case State::NEEDS_UNLOCK:
            flow.retryAfterUnlock();
            break;
        case State::FAILED:
            // What the failed attempt put on chain is kept: a funded asset
            // lock is registered from again, anything else asks for a name.
            flow.reset();
            if (flow.record().state == State::NONE || flow.record().AwaitsUsername()) {
                setCurrentPage(PAGE_ENTRY);
            } else {
                flow.advance();
            }
            break;
        default:
            accept();
            break;
        }
        break;
    }
    updateButtons();
}

void CreateUsernameWizard::updateButtons()
{
    const int id{m_stack->currentIndex()};
    m_step_hint->setText(tr("Step %1 of %2").arg(id + 1).arg(PAGE_COUNT));
    m_busy->setVisible(currentPage()->isBusy());
    m_back->setVisible(id == PAGE_COST);
    m_secondary->hide();
    m_cancel->show();
    m_cancel->setText(tr("Cancel"));
    m_next->show();
    m_next->setEnabled(currentPage()->isComplete());
    m_next->setToolTip({});
    m_next->setDefault(true);
    switch (id) {
    case PAGE_ENTRY:
        m_next->setText(tr("Next"));
        break;
    case PAGE_COST:
        m_next->setText(tr("Register"));
        break;
    case PAGE_PROGRESS: {
        // Registration runs on without the window: "Cancel" would promise
        // something it never did.
        m_cancel->setText(tr("Close"));
        const State state{m_service.identityFlow().record().state};
        switch (state) {
        case State::NEEDS_UNLOCK: {
            m_next->setText(tr("Unlock and continue…"));
            QString refusal;
            m_next->setEnabled(m_service.writesAllowed(refusal));
            m_next->setToolTip(refusal);
            break;
        }
        case State::FAILED: {
            m_next->setText(tr("Try again…"));
            QString refusal;
            m_next->setEnabled(m_service.writesAllowed(refusal));
            m_next->setToolTip(refusal);
            break;
        }
        case State::REGISTERED:
            m_next->setText(tr("Done"));
            m_cancel->hide();
            // A display name given here is published with the username.
            m_secondary->setVisible(!m_service.identityFlow().profileChosen());
            break;
        case State::CONTESTED_PENDING:
            m_next->setText(tr("Done"));
            m_cancel->hide();
            break;
        default:
            // Working: Enter does nothing, Close only hides the window.
            m_next->hide();
            m_next->setDefault(false);
            break;
        }
        break;
    }
    }
}

// ---- Entry page -------------------------------------------------------------

UsernameEntryPage::UsernameEntryPage(PlatformService& service, QWidget* parent) :
    UsernameWizardPage(parent),
    m_service(service)
{
    auto* layout{PageLayout(this, tr("Choose your username"),
                            tr("Your username is public and unique on Dash Platform. People use it to find and pay "
                               "you."))};
    auto* block{MasternodeWidgetUtil::makeBlock(layout)};
    m_input = new QLineEdit(this);
    m_input->setPlaceholderText(tr("e.g. alice"));
    m_input->setMaxLength(MAX_LABEL_LENGTH);
    block->addWidget(m_input);
    m_rule = PlatformUi::makeHint(tr("3 to 63 characters: letters, numbers and single hyphens, starting and ending "
                                     "with a letter or number."),
                                  this);
    block->addWidget(m_rule);
    m_status = new PlatformUi::MessageLine(this);
    block->addWidget(m_status);
    m_normalized = PlatformUi::makeHint({}, this);
    m_normalized->setTextFormat(Qt::PlainText);
    m_normalized->hide();
    block->addWidget(m_normalized);

    // Signed with the registration, so it costs no second passphrase; only
    // a new identity, whose first DashPay change it is.
    m_profile = new QWidget(this);
    auto* profile{MasternodeWidgetUtil::makeBlock(new QVBoxLayout(m_profile))};
    m_profile->layout()->setContentsMargins(0, 0, 0, 0);
    profile->addWidget(MasternodeWidgetUtil::makeBlockTitle(tr("Display name (optional)"), m_profile));
    m_display_name = new QLineEdit(m_profile);
    m_display_name->setMaxLength(PlatformService::MAX_DISPLAY_NAME_LENGTH);
    m_display_name->setPlaceholderText(tr("e.g. your name or nickname"));
    profile->addWidget(m_display_name);
    profile->addWidget(PlatformUi::makeHint(tr("Shown next to your username so people recognize you. It is public; you "
                                               "can leave it empty and add it later."),
                                            m_profile));
    layout->addWidget(m_profile);
    layout->addStretch();
    setFocusProxy(m_input);

    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(DEBOUNCE_MS);

    connect(m_input, &QLineEdit::textChanged, this, &UsernameEntryPage::onTextChanged);
    connect(m_debounce, &QTimer::timeout, this, [this] {
        const QString name{m_input->text().trimmed()};
        if (!name.isEmpty()) m_service.checkNameAvailability(name);
    });
    connect(m_status, &PlatformUi::MessageLine::actionClicked, this, &UsernameEntryPage::onTextChanged);
    connect(&m_service, &PlatformService::nameAvailability, this, &UsernameEntryPage::onAvailability);
    connect(&m_service, &PlatformService::nameIsOurs, this, &UsernameEntryPage::onNameIsOurs);
    connect(&m_service, &PlatformService::nameAvailabilityFailed, this, &UsernameEntryPage::onAvailabilityFailed);
}

bool UsernameEntryPage::isCurrent(const QString& normalized_label) const
{
    return QString::fromStdString(platform::helpers::NormalizeLabel(m_input->text().trimmed().toStdString())) ==
           normalized_label;
}

void UsernameEntryPage::setChecking(bool checking)
{
    m_checking = checking;
    Q_EMIT completeChanged();
}

void UsernameEntryPage::onAvailabilityFailed(const QString& normalized_label, const QString& error, const QString& details)
{
    if (!isCurrent(normalized_label)) return;
    m_available = false;
    m_status->setMessage(Severity::Error, tr("Dash Core couldn't check this username. %1").arg(error), details);
    m_status->setAction(tr("Retry"));
    setChecking(false);
}

void UsernameEntryPage::onTextChanged()
{
    m_available = false;
    m_ours = false;
    m_checked_normalized.clear();
    m_debounce->stop();
    m_status->clear();
    const QString typed{m_input->text().trimmed()};
    // DPNS decides availability on the normalized label (lower case, with
    // o->0 and i/l->1 folded): say so when that is not what was typed.
    const QString normalized{QString::fromStdString(platform::helpers::NormalizeLabel(typed.toStdString()))};
    const QString explanation{tr("Stored as “%1”. Dash Platform treats o and 0, and i, l and 1, as the same character "
                                 "so look-alike usernames can't impersonate you.")
                                  .arg(normalized)};
    // The DPNS label rule is the SDK's, not a copy of it: nothing is looked
    // up for a label Platform would refuse.
    const bool valid{platform::helpers::IsValidUsername(typed.toStdString())};
    const bool differs{valid && normalized != typed.toLower()};
    m_normalized->setText(explanation);
    m_normalized->setVisible(differs);
    m_status->setToolTip(differs ? explanation : QString{});
    // The rule is stated once: as the hint, or as the error that says it
    // was broken.
    m_rule->setVisible(typed.isEmpty() || valid);
    if (typed.isEmpty()) {
        setChecking(false);
        return;
    }
    if (!valid) {
        m_status->setMessage(Severity::Error, tr("This can't be a username. Use 3 to 63 letters, numbers and single "
                                                 "hyphens, starting and ending with a letter or number."));
        setChecking(false);
        return;
    }
    m_status->setMessage(Severity::Info, tr("Checking whether “%1” is available…").arg(typed));
    m_debounce->start();
    setChecking(true);
}

void UsernameEntryPage::onAvailability(const QString& normalized_label, bool available, bool contested)
{
    if (!isCurrent(normalized_label)) return; // stale
    const QString typed{m_input->text().trimmed()};
    m_available = available;
    m_ours = false;
    m_contested = contested;
    m_checked_normalized = normalized_label;
    if (!available) {
        m_status->setMessage(Severity::Error, tr("“%1” is already taken. Try adding a number or a hyphen.").arg(typed));
    } else if (contested) {
        m_status->setMessage(Severity::Attention,
                             tr("“%1” is a premium username: it's short and uses only letters, 0, 1 and hyphens. "
                                "Masternodes vote on who gets premium usernames, which can take up to two weeks, and "
                                "someone else may win it. Add a digit from 2 to 9 to get a regular username right "
                                "away.")
                                 .arg(typed));
    } else {
        m_status->setMessage(Severity::Success, tr("“%1” is available.").arg(typed));
    }
    setChecking(false);
}

void UsernameEntryPage::onNameIsOurs(const QString& normalized_label)
{
    // Only an identity still waiting for a name can take it over.
    if (!m_service.identityFlow().record().AwaitsUsername()) {
        onAvailability(normalized_label, /*available=*/false, /*contested=*/false);
        return;
    }
    if (!isCurrent(normalized_label)) return;
    m_available = true;
    m_ours = true;
    m_contested = false;
    m_checked_normalized = normalized_label;
    m_status->setMessage(Severity::Success, tr("“%1” is already yours. Choose Next to finish; no payment is needed.")
                                                .arg(m_input->text().trimmed()));
    setChecking(false);
}

bool UsernameEntryPage::validatePage()
{
    if (!m_ours) return true;
    // The name is registered to our identity already: the flow confirms it
    // with a proved read and completes without any payment.
    QString error;
    if (!m_service.identityFlow().start(username(), /*funding_amount=*/0, error)) {
        m_status->setMessage(Severity::Error, error);
        return false;
    }
    return true;
}

bool UsernameEntryPage::isComplete() const { return m_available && !m_checked_normalized.isEmpty(); }

QString UsernameEntryPage::username() const { return m_input->text().trimmed(); }

void UsernameEntryPage::initializePage()
{
    // Checked each time the page is entered: a failed name step of a new
    // identity comes back here with the identity kept, and the display name
    // is then no longer published with the name.
    m_profile->setVisible(!m_service.identityFlow().record().AwaitsUsername());
}

QString UsernameEntryPage::displayName() const
{
    return m_profile->isHidden() ? QString{} : m_display_name->text().trimmed();
}

// ---- Cost page --------------------------------------------------------------

UsernameCostPage::UsernameCostPage(PlatformService& service, WalletModel& wallet_model, const UsernameEntryPage& entry,
                                   QWidget* parent) :
    UsernameWizardPage(parent),
    m_service(service),
    m_wallet_model(wallet_model),
    m_entry(entry)
{
    auto* layout{PageLayout(this, tr("Confirm registration"), {})};
    auto* form = new QFormLayout();
    form->setVerticalSpacing(ROW_SPACING);
    // Wrapped values get the width of the column and the height they need.
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::DontWrapRows);
    m_username = new QLabel(this);
    m_username->setTextFormat(Qt::PlainText);
    GUIUtil::setFont({m_username}, GUIUtil::FontWeight::Bold);
    m_normalized = new QLabel(this);
    m_normalized->setTextFormat(Qt::PlainText);
    m_wallet = new QLabel(this);
    m_wallet->setTextFormat(Qt::PlainText);
    m_display_name = new QLabel(this);
    m_display_name->setTextFormat(Qt::PlainText);
    m_cost = new QLabel(this);
    m_cost->setWordWrap(true);
    GUIUtil::setFont({m_cost}, GUIUtil::FontWeight::Bold);
    for (const auto& [label, field] : {std::pair{tr("Username"), static_cast<QWidget*>(m_username)},
                                       std::pair{tr("Stored as"), static_cast<QWidget*>(m_normalized)},
                                       std::pair{tr("Display name"), static_cast<QWidget*>(m_display_name)},
                                       std::pair{tr("Wallet"), static_cast<QWidget*>(m_wallet)},
                                       std::pair{tr("Cost"), static_cast<QWidget*>(m_cost)}}) {
        m_rows.append({new QLabel(label, this), field});
    }
    m_form = form;
    layout->addLayout(form);
    m_explanation = PlatformUi::makeHint({}, this);
    layout->addWidget(m_explanation);

    m_premium = MasternodeWidgetUtil::makeCard(this);
    auto* premium_layout = new QHBoxLayout(m_premium);
    premium_layout->setContentsMargins(CARD_PADDING, CARD_PADDING, CARD_PADDING, CARD_PADDING);
    m_premium_text = new PlatformUi::MessageLine(m_premium);
    premium_layout->addWidget(m_premium_text);
    layout->addWidget(m_premium);
    m_error = new PlatformUi::MessageLine(this);
    layout->addWidget(m_error);
    layout->addStretch();

    connect(&m_service, &PlatformService::identityBalanceLoaded, this, &UsernameCostPage::onIdentityBalance);
    connect(&m_service, &PlatformService::identityBalanceFailed, this, [this](const QString& error, const QString& details) {
        if (!m_awaiting_balance) return;
        m_awaiting_balance = false;
        m_premium_text->setMessage(Severity::Error, tr("Your identity's balance could not be checked. %1").arg(error),
                                   details);
        Q_EMIT completeChanged();
    });
}

QString UsernameCostPage::formatAmount(CAmount amount) const
{
    return BitcoinUnits::formatWithUnit(m_wallet_model.getOptionsModel()->getDisplayUnit(), amount);
}

QString UsernameCostPage::formatBalance(uint64_t credits) const
{
    return PlatformUi::formatPlatformBalance(m_wallet_model.getOptionsModel()->getDisplayUnit(), credits);
}

void UsernameCostPage::initializePage()
{
    m_ready = false;
    m_awaiting_balance = false;
    m_premium->hide();
    m_error->clear();
    const QString typed{m_entry.username()};
    const QString normalized{QString::fromStdString(platform::helpers::NormalizeLabel(typed.toStdString()))};
    m_username->setText(typed);
    m_normalized->setText(normalized);
    m_display_name->setText(m_entry.displayName());
    // A row that says nothing is left out rather than hidden, which would
    // leave its spacing behind.
    while (m_form->rowCount() > 0) {
        m_form->takeRow(0);
    }
    for (const auto& [label, field] : m_rows) {
        const bool shown{(field != m_normalized || normalized != typed.toLower()) &&
                         (field != m_display_name || !m_display_name->text().isEmpty())};
        label->setVisible(shown);
        field->setVisible(shown);
        if (shown) m_form->addRow(label, field);
    }
    m_wallet->setText(m_wallet_model.getDisplayName());

    const bool contested{m_entry.contested()};
    const auto funding{m_service.identityFundingAmount(contested)};
    const auto vote_credits{m_service.contestedNameCredits()};
    if (!funding || (contested && !vote_credits)) {
        m_funding_amount = 0;
        m_cost->setText(tr("Not known yet"));
        m_explanation->setText(tr("Dash Platform has not answered a verified query on this network yet. Go back and "
                                  "try again in a moment."));
        Q_EMIT completeChanged();
        return;
    }
    if (m_service.identityFlow().record().AwaitsUsername()) {
        m_funding_amount = 0;
        m_cost->setText(tr("Paid from your balance on Dash Platform"));
        m_explanation->setText(tr("Your identity already exists, so this username is paid from its balance on Dash "
                                  "Platform. No new payment is made."));
        // A premium name is decided by whether the proved balance covers the
        // vote reserve; a regular one only shows it.
        m_awaiting_balance = contested;
        if (contested) {
            m_premium->show();
            m_premium_text->setMessage(Severity::Info, tr("Checking your identity's balance…"));
        }
        m_ready = !contested;
        m_service.refreshIdentityBalance();
        Q_EMIT completeChanged();
        return;
    }
    m_funding_amount = *funding;
    m_cost->setText(formatAmount(m_funding_amount));
    m_explanation->setText(tr("Dash Core sends %1 from this wallet to fund your Dash Platform identity, then registers "
                              "your username. What is left stays as your balance on Dash Platform and pays for later "
                              "changes such as your profile.")
                               .arg(formatAmount(m_funding_amount)));
    if (contested) {
        m_premium->show();
        m_premium_text->setMessage(Severity::Attention,
                                   tr("“%1” is premium. %2 of the cost is held for the masternode vote, which can take "
                                      "up to two weeks. Someone else may win it.")
                                       .arg(typed, formatBalance(*vote_credits)));
    }
    const CAmount available{m_wallet_model.getAvailableBalance(nullptr)};
    if (available < m_funding_amount) {
        m_error->setMessage(Severity::Error, tr("This wallet has %1 available, but registering needs %2 plus a small "
                                                "network fee.")
                                                 .arg(formatAmount(available), formatAmount(m_funding_amount)));
    } else {
        m_ready = true;
    }
    Q_EMIT completeChanged();
}

void UsernameCostPage::onIdentityBalance(quint64 credits)
{
    if (!m_service.identityFlow().record().AwaitsUsername()) return;
    m_explanation->setText(tr("Your identity already exists, so this username is paid from its balance on Dash "
                              "Platform (%1). No new payment is made.")
                               .arg(formatBalance(credits)));
    const auto reserve{m_service.contestedNameCredits()};
    const auto required{m_service.contestedNameRequiredCredits()};
    if (!m_awaiting_balance || !reserve || !required) return;
    m_awaiting_balance = false;
    if (credits >= *required) {
        m_premium_text->setMessage(Severity::Attention,
                                   tr("A premium username locks %1 of your balance on Dash Platform for the "
                                      "masternode vote, and registering it needs up to %2 in all with the fees. You "
                                      "have %3 there, which covers it; no new payment is made.")
                                       .arg(formatBalance(*reserve), formatBalance(*required), formatBalance(credits)));
        m_ready = true;
    } else {
        m_premium_text->setMessage(Severity::Error,
                                   tr("A premium username locks %1 of your balance on Dash Platform for the "
                                      "masternode vote, and registering it needs up to %2 in all with the fees, but "
                                      "you have only %3 there. Dash Core can't add to your balance on Dash Platform "
                                      "yet, so choose a username that is not premium.")
                                       .arg(formatBalance(*reserve), formatBalance(*required), formatBalance(credits)));
        m_ready = false;
    }
    Q_EMIT completeChanged();
}

bool UsernameCostPage::validatePage()
{
    QString error;
    if (!m_service.identityFlow().start(m_entry.username(), m_funding_amount, error, m_entry.displayName())) {
        m_error->setMessage(Severity::Error, error);
        return false;
    }
    return true;
}

// ---- Progress page ----------------------------------------------------------

UsernameProgressPage::UsernameProgressPage(PlatformService& service, QWidget* parent) :
    UsernameWizardPage(parent),
    m_service(service)
{
    auto* layout{PageLayout(this, {}, {}, &m_title, &m_subtitle)};
    m_title->setTextFormat(Qt::PlainText);
    auto* checklist = new QVBoxLayout();
    checklist->setSpacing(ROW_SPACING);
    // Four rows at most (a new identity); an existing identity uses two.
    for (int i = 0; i < 4; ++i) {
        auto* row = new QHBoxLayout();
        row->setSpacing(TITLE_SPACING);
        auto* icon = new QLabel(this);
        icon->setFixedSize(STEP_ICON_SIZE, STEP_ICON_SIZE);
        auto* label = new QLabel(this);
        row->addWidget(icon);
        row->addWidget(label, /*stretch=*/1);
        checklist->addLayout(row);
        m_step_icons.append(icon);
        m_step_labels.append(label);
    }
    layout->addLayout(checklist);
    m_error = new PlatformUi::MessageLine(this);
    layout->addWidget(m_error);
    m_log_toggle = new QPushButton(tr("Show log"), this);
    PlatformUi::makeSecondary(m_log_toggle);
    m_log_toggle->setAutoDefault(false);
    layout->addWidget(m_log_toggle, 0, Qt::AlignLeft);
    m_log = new QPlainTextEdit(this);
    m_log->setObjectName("registrationLog");
    m_log->setReadOnly(true);
    m_log->setFont(GUIUtil::fixedPitchFont());
    m_log->hide();
    layout->addWidget(m_log, /*stretch=*/1);
    layout->addStretch();

    connect(m_log_toggle, &QPushButton::clicked, this, [this] {
        m_log->setVisible(!m_log->isVisible());
        m_log_toggle->setText(m_log->isVisible() ? tr("Hide log") : tr("Show log"));
    });
    connect(&m_service, &PlatformService::identityStateChanged, this, &UsernameProgressPage::refresh);
    connect(&m_service, &PlatformService::flowFailed, this,
            [this](const QString& step, const QString& error) { appendLog(tr("%1: %2").arg(step, error)); });
}

void UsernameProgressPage::initializePage() { refresh(); }

void UsernameProgressPage::appendLog(const QString& line)
{
    // A wait that reports the same thing on every retry is logged once.
    if (line == m_last_logged) return;
    m_last_logged = line;
    m_log->appendPlainText(QStringLiteral("%1 %2").arg(QTime::currentTime().toString("HH:mm:ss"), line));
}

bool UsernameProgressPage::isBusy() const
{
    const State state{m_service.identityFlow().record().state};
    return !IsTerminal(state) && state != State::NEEDS_UNLOCK && state != State::NONE;
}

void UsernameProgressPage::changeEvent(QEvent* event)
{
    UsernameWizardPage::changeEvent(event);
    if (event->type() == QEvent::StyleChange) refresh();
}

void UsernameProgressPage::refresh()
{
    const IdentityFlow& flow{m_service.identityFlow()};
    const auto& rec{flow.record()};
    const QString label{QString::fromStdString(rec.label)};
    const QStringList steps{PlatformUi::registrationSteps(rec)};
    const int current{PlatformUi::registrationStep(rec)};

    for (int i = 0; i < m_step_labels.size(); ++i) {
        QLabel* icon{m_step_icons[i]};
        QLabel* text{m_step_labels[i]};
        icon->setVisible(i < steps.size());
        text->setVisible(i < steps.size());
        if (i >= steps.size()) continue;
        text->setText(steps[i]);
        icon->clear();
        QFont font{text->font()};
        font.setBold(i == current && !IsTerminal(rec.state));
        text->setFont(font);
        if (i < current) {
            icon->setPixmap(GUIUtil::getIcon("synced", GUIUtil::ThemedColor::GREEN).pixmap(STEP_ICON_SIZE, STEP_ICON_SIZE));
            PlatformUi::setTextStyle(text, GUIUtil::ThemedStyle::TS_PRIMARY);
        } else if (i == current && rec.state == State::FAILED) {
            icon->setPixmap(GUIUtil::getIcon("warning", GUIUtil::ThemedColor::RED).pixmap(STEP_ICON_SIZE, STEP_ICON_SIZE));
            PlatformUi::setTextStyle(text, GUIUtil::ThemedStyle::TS_PRIMARY);
        } else if (i == current) {
            PlatformUi::setTextStyle(text, GUIUtil::ThemedStyle::TS_PRIMARY);
        } else {
            PlatformUi::setTextStyle(text, GUIUtil::ThemedStyle::TS_SECONDARY);
        }
    }

    // The log records each step once, as it is first reached; a step the
    // flow goes back to while it waits for Platform is not a new one. Trying
    // again after a failure starts over from the step it resumes at.
    if (rec.state == State::FAILED) {
        m_retrying = true;
    } else if (m_retrying && current >= 0) {
        m_retrying = false;
        m_logged_step = current - 1;
    }
    if (current > m_logged_step && current < steps.size()) {
        for (int i = m_logged_step + 1; i <= current; ++i) {
            appendLog(steps[i]);
        }
        m_logged_step = current;
    }

    m_error->clear();
    m_subtitle->show();
    switch (rec.state) {
    case State::REGISTERED:
        m_title->setText(tr("Username registered"));
        if (flow.profileChosen()) {
            m_subtitle->setText(tr("You're now “%1” on DashPay. Next, add your first contact.").arg(label));
        } else {
            m_subtitle->setText(tr("You're now “%1” on DashPay. Next, add a profile so people recognize you, then "
                                   "add your first contact.")
                                    .arg(label));
        }
        break;
    case State::CONTESTED_PENDING:
        m_title->setText(tr("Premium username submitted"));
        m_subtitle->setText(
            tr("Masternodes will now vote on “%1”. This can take up to two weeks. The DashPay tab shows "
               "the result.")
                .arg(label));
        break;
    case State::FAILED:
        m_title->setText(tr("Registration did not finish"));
        m_error->setMessage(Severity::Error, flow.lastErrorText(), flow.lastErrorDetails());
        m_subtitle->setText(PlatformUi::failureReassurance(rec));
        break;
    default:
        m_title->setText(tr("Registering “%1”").arg(label));
        m_subtitle->setText(
            tr("You can close this window. Registration continues in the background, and the DashPay tab "
               "shows how it's going."));
        break;
    }
    Q_EMIT completeChanged();
}
