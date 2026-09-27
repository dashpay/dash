// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/contactspage.h>

#include <qt/clientmodel.h>
#include <qt/guiutil.h>
#include <qt/masternodewidgets.h>
#include <qt/platform/contactsmodel.h>
#include <qt/platform/platformservice.h>
#include <qt/platform/platformui.h>
#include <qt/platform/usernamesearchdialog.h>
#include <qt/sharedmnwidgets.h>
#include <util/time.h>

#include <QAction>
#include <QCheckBox>
#include <QDate>
#include <QDateTime>
#include <QEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QMenu>
#include <QProgressBar>
#include <QPushButton>
#include <QShowEvent>
#include <QTableView>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <chrono>

using MasternodeWidgetUtil::TITLE_SPACING;
using Severity = PlatformUi::MessageLine::Severity;

namespace {
//! Rows the table shows before it scrolls, and the fewest it reserves.
constexpr int MIN_VISIBLE_ROWS{3};
constexpr int MAX_VISIBLE_ROWS{12};
//! Room added to the widest username so it does not touch the next column.
constexpr int COLUMN_PADDING{24};
//! A ChainLock reads the list again at most this often.
constexpr int64_t CHAINLOCK_REFRESH_SECONDS{60};
//! Without a ChainLock, the list is read again this long after the last read.
constexpr int64_t FALLBACK_REFRESH_SECONDS{5 * 60};
//! The next automatic read after the first, second, … failure in a row.
constexpr std::array<int64_t, 5> FAILURE_BACKOFF_SECONDS{30, 60, 2 * 60, 4 * 60, 10 * 60};
} // namespace

ContactsPage::ContactsPage(PlatformService& service, QWidget* parent) :
    QWidget(parent),
    m_service(service)
{
    setObjectName("ContactsPage");
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(TITLE_SPACING);

    // Heading, what needs attention, and the actions of the selected row next
    // to Add contact, so they sit by the rows instead of under a tall table.
    auto* header = new QHBoxLayout();
    header->setSpacing(TITLE_SPACING);
    auto* heading = new QLabel(tr("Contacts"), this);
    GUIUtil::setFont({heading}, GUIUtil::FontWeight::Bold, PlatformUi::SECTION_HEADING_SIZE);
    m_attention = new QLabel(this);
    PlatformUi::setTextColor(m_attention, GUIUtil::ThemedColor::ORANGE);
    m_attention->hide();
    m_show_ignored = new QCheckBox(this);
    m_show_ignored->hide();
    m_primary_button = new QPushButton(this);
    m_ignore_button = new QPushButton(this);
    PlatformUi::makeSecondary(m_ignore_button);
    m_add_button = new QPushButton(tr("Add contact…"), this);
    header->addWidget(heading);
    header->addWidget(m_attention);
    header->addStretch();
    header->addWidget(m_show_ignored);
    header->addWidget(m_primary_button);
    header->addWidget(m_ignore_button);
    header->addWidget(m_add_button);
    layout->addLayout(header);

    m_model = new ContactsModel(m_service, this);
    m_view = new QTableView(this);
    m_view->setModel(m_model);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setSelectionMode(QAbstractItemView::SingleSelection);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    m_view->setSizeAdjustPolicy(QAbstractScrollArea::AdjustIgnored);
    m_view->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    // Status sits by the data; the space left over goes after it.
    m_view->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);
    m_view->horizontalHeader()->setStretchLastSection(true);
    m_view->verticalHeader()->hide();
    layout->addWidget(m_view);

    // Nothing to list yet: what contacts are for, and how to get one.
    m_empty = new QWidget(this);
    auto* empty_layout = new QVBoxLayout(m_empty);
    empty_layout->setContentsMargins(0, 0, 0, 0);
    empty_layout->setSpacing(TITLE_SPACING);
    auto* empty_title = new QLabel(tr("No contacts yet"), m_empty);
    GUIUtil::setFont({empty_title}, GUIUtil::FontWeight::Bold);
    auto* empty_hint = PlatformUi::makeHint(tr("Add people by their username. Once they accept your request, you can "
                                               "pay each other by username from the Send tab, no addresses needed."),
                                            m_empty);
    m_empty_add_button = new QPushButton(tr("Add contact…"), m_empty);
    empty_layout->addWidget(empty_title);
    empty_layout->addWidget(empty_hint);
    empty_layout->addWidget(m_empty_add_button, 0, Qt::AlignLeft);
    layout->addWidget(m_empty);

    m_loading = new QWidget(this);
    auto* loading_layout = new QVBoxLayout(m_loading);
    loading_layout->setContentsMargins(0, 0, 0, 0);
    auto* loading_bar = PlatformUi::makeBusyBar(m_loading);
    loading_bar->show();
    loading_layout->addWidget(loading_bar);
    loading_layout->addWidget(PlatformUi::makeHint(tr("Loading contacts…"), m_loading));
    layout->addWidget(m_loading);

    // Only when the list may be out of date: normally it never shows a clock.
    m_updated = PlatformUi::makeHint({}, this);
    m_updated->hide();
    layout->addWidget(m_updated);

    m_busy = PlatformUi::makeBusyBar(this);
    layout->addWidget(m_busy);
    m_status = new PlatformUi::MessageLine(this);
    layout->addWidget(m_status);

    // Tab order: the rows, what the selected one allows, then the section's.
    setTabOrder(m_view, m_primary_button);
    setTabOrder(m_primary_button, m_ignore_button);
    setTabOrder(m_ignore_button, m_show_ignored);
    setTabOrder(m_show_ignored, m_add_button);

    // Read again when the node sees a new block, as long as it is shown; a
    // timer covers a node that sees none.
    m_refresh_timer = new QTimer(this);
    m_refresh_timer->setSingleShot(true);
    connect(m_refresh_timer, &QTimer::timeout, this, &ContactsPage::refreshIfDue);
    connect(&m_service.clientModel(), &ClientModel::chainLockChanged, this, &ContactsPage::refreshIfDue);
    connect(m_status, &PlatformUi::MessageLine::actionClicked, this, [this] {
        m_failures = 0;
        refresh();
    });
    connect(m_add_button, &QPushButton::clicked, this, &ContactsPage::addContact);
    connect(m_empty_add_button, &QPushButton::clicked, this, &ContactsPage::addContact);
    // What may be written changes the actions offered.
    connect(&m_service, &PlatformService::identityStateChanged, this, &ContactsPage::updateActions);
    connect(&m_service, &PlatformService::unsupportedProtocolVersion, this, &ContactsPage::updateActions);
    connect(m_show_ignored, &QCheckBox::toggled, m_model, &ContactsModel::setShowIgnored);
    connect(m_primary_button, &QPushButton::clicked, this, &ContactsPage::activateSelected);
    connect(m_ignore_button, &QPushButton::clicked, this, [this] {
        const auto* row{m_model->rowAt(m_view->currentIndex().row())};
        if (row) setSelectedHidden(!row->hidden);
    });
    connect(m_view->selectionModel(), &QItemSelectionModel::currentChanged, this, &ContactsPage::updateActions);
    // Accepting writes to Platform: never on the single click some platforms
    // activate rows with.
    connect(m_view, &QTableView::doubleClicked, this, &ContactsPage::activateSelected);
    auto* activate{new QAction(m_view)};
    activate->setShortcuts({QKeySequence{Qt::Key_Return}, QKeySequence{Qt::Key_Enter}});
    activate->setShortcutContext(Qt::WidgetShortcut);
    m_view->addAction(activate);
    connect(activate, &QAction::triggered, this, &ContactsPage::activateSelected);
    connect(m_view, &QTableView::customContextMenuRequested, this, &ContactsPage::showContextMenu);
    connect(m_model, &QAbstractItemModel::modelAboutToBeReset, this, &ContactsPage::rememberSelection);
    connect(m_model, &QAbstractItemModel::modelReset, this, &ContactsPage::restoreSelection);
    connect(m_model, &QAbstractItemModel::modelReset, this, &ContactsPage::updateActions);
    connect(m_model, &QAbstractItemModel::dataChanged, this, &ContactsPage::updateActions);
    connect(&m_service, &PlatformService::contactRequestPending, this, &ContactsPage::onContactRequestPending);
    connect(&m_service, &PlatformService::contactRequestFinished, this, &ContactsPage::onContactRequestFinished);
    connect(&m_service, &PlatformService::contactsRefreshFailed, this, [this](const QString& error, const QString& details) {
        m_status->setMessage(Severity::Error, tr("Your contacts could not be refreshed. %1").arg(error), details);
        m_status->setAction(tr("Try again"));
        m_refresh_failed = true;
        // The next automatic read backs off while Platform keeps failing.
        const int64_t delay{FAILURE_BACKOFF_SECONDS[std::min<size_t>(m_failures, FAILURE_BACKOFF_SECONDS.size() - 1)]};
        ++m_failures;
        m_next_refresh = GetTime() + delay;
        m_refresh_timer->start(std::chrono::seconds{delay});
        updateStaleness();
    });
    connect(&m_service, &PlatformService::contactsUpdated, this, [this] {
        // A reply Platform took longer to confirm than the flow waited: the
        // list shows where it stands now.
        const bool handed_over{!m_confirming_identity.isEmpty() &&
                               m_service.pendingContactRequest() != m_confirming_identity};
        if (!handed_over) return;
        m_confirming_identity.clear();
        m_status->clear();
    });
    connect(&m_service, &PlatformService::contactsRefreshed, this, [this] {
        m_failures = 0;
        m_last_success = GetTime();
        if (m_refresh_failed) {
            m_refresh_failed = false;
            m_status->clear();
        }
        updateStaleness();
    });

    SharedMnFitWrappedLabels(this);
    updateActions();
}

void ContactsPage::refresh()
{
    m_next_refresh = GetTime() + CHAINLOCK_REFRESH_SECONDS;
    m_refresh_timer->start(std::chrono::seconds{FALLBACK_REFRESH_SECONDS});
    m_model->refresh();
}

void ContactsPage::refreshIfShown()
{
    // A failing Platform is asked again only when the backoff allows.
    if (m_failures == 0 || GetTime() >= m_next_refresh) refresh();
}

void ContactsPage::refreshIfDue()
{
    updateStaleness();
    // Reads disclose the identity: none while the list is not shown, paused
    // or without endpoints to ask.
    if (!isVisible() || !isEnabled() || !m_service.haveEndpoints()) return;
    if (const int64_t now{GetTime()}; now < m_next_refresh) {
        // A coarse timer may fire a little early; wait for the rest.
        if (!m_refresh_timer->isActive()) m_refresh_timer->start(std::chrono::seconds{m_next_refresh - now});
        return;
    }
    refresh();
}

void ContactsPage::updateStaleness()
{
    const bool stale{m_refresh_failed || (m_last_success > 0 && GetTime() - m_last_success > FALLBACK_REFRESH_SECONDS)};
    m_updated->setVisible(stale && m_last_success > 0);
    if (m_last_success > 0) {
        // The time of day, with the date once it is not today.
        const QDateTime updated{QDateTime::fromSecsSinceEpoch(m_last_success)};
        m_updated->setText(tr("Last updated at %1.")
                               .arg(updated.date() == QDate::currentDate()
                                        ? QLocale().toString(updated.time(), QLocale::ShortFormat)
                                        : QLocale().toString(updated, QLocale::ShortFormat)));
    }
}

void ContactsPage::addContact()
{
    UsernameSearchDialog(m_service, this).exec();
    refresh();
}

void ContactsPage::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (!event->spontaneous()) m_auto_selected = false;
    updateStaleness();
}

void ContactsPage::fitTable()
{
    // As tall as its rows (a few at least), scrolling past a dozen: the
    // dashboard puts its spare space below the section, not inside it.
    const int rows{std::clamp(m_model->rowCount(), MIN_VISIBLE_ROWS, MAX_VISIBLE_ROWS)};
    const int row_height{m_view->verticalHeader()->defaultSectionSize()};
    m_view->setFixedHeight(m_view->horizontalHeader()->sizeHint().height() + rows * row_height + 2 * m_view->frameWidth());
    // Username and profile name as wide as their content (with room to
    // breathe), Status right after them.
    m_view->setColumnHidden(ContactsModel::DisplayName, !m_model->hasDisplayNames());
    for (const int column : {ContactsModel::Username, ContactsModel::DisplayName}) {
        m_view->resizeColumnToContents(column);
        m_view->setColumnWidth(column, m_view->columnWidth(column) + COLUMN_PADDING);
    }
}

bool ContactsPage::isEmpty() const { return m_model->rowCount() == 0 && m_model->hiddenCount() == 0; }

bool ContactsPage::showsEmptyState() const { return !m_empty->isHidden(); }

QWidget* ContactsPage::focusTarget() const
{
    if (showsEmptyState()) return m_empty_add_button;
    return m_view->isHidden() ? nullptr : m_view;
}

void ContactsPage::changeEvent(QEvent* event)
{
    QWidget::changeEvent(event);
    if (event->type() == QEvent::EnabledChange) updateActions();
    if (event->type() != QEvent::StyleChange) return;
    PlatformUi::restyle(this);
    m_model->themeChanged();
}

void ContactsPage::rememberSelection()
{
    const auto* row{m_model->rowAt(m_view->currentIndex().row())};
    m_selected_identity = row ? row->identity_hex : QString{};
}

void ContactsPage::restoreSelection()
{
    for (int r = 0; r < m_model->rowCount() && !m_selected_identity.isEmpty(); ++r) {
        if (m_model->rowAt(r)->identity_hex == m_selected_identity) {
            m_view->setCurrentIndex(m_model->index(r, 0));
            return;
        }
    }
    // Requests waiting for an answer show their Accept at once: the first
    // is selected, once per showing, without taking the focus.
    if (m_auto_selected) return;
    for (int r = 0; r < m_model->rowCount(); ++r) {
        const auto* row{m_model->rowAt(r)};
        if (row->kind == ContactsModel::Kind::Incoming && !row->hidden) {
            m_view->setCurrentIndex(m_model->index(r, 0));
            m_auto_selected = true;
            return;
        }
    }
}

void ContactsPage::updateActions()
{
    using Kind = ContactsModel::Kind;
    const bool have_rows{m_model->rowCount() > 0};
    const bool was_empty_state{showsEmptyState()};
    m_view->setVisible(have_rows);
    m_empty->setVisible(!have_rows && m_model->loaded());
    // Nothing loads while DashPay is paused (the dashboard's notice says
    // why): no busy bar that would never finish.
    m_loading->setVisible(!have_rows && !m_model->loaded() && isEnabled());
    if (have_rows) fitTable();
    m_show_ignored->setText(tr("Show hidden (%1)").arg(m_model->hiddenCount()));
    m_show_ignored->setVisible(m_model->hiddenCount() > 0);
    // Requests waiting for an answer: not accepted (one that answers our own
    // request is accepted already) and not ignored.
    int pending{0};
    for (int r = 0; r < m_model->rowCount(); ++r) {
        const auto* request{m_model->rowAt(r)};
        if (request->kind == Kind::Incoming && !request->hidden) ++pending;
    }
    // No plural form: new strings have no English plural entry until the
    // translation files are refreshed.
    m_attention->setText(tr("Requests waiting for you: %1").arg(pending));
    m_attention->setVisible(pending > 0);

    const auto* row{m_model->rowAt(m_view->currentIndex().row())};
    const bool busy{!m_accepting_identity.isEmpty()};
    QString refusal;
    const bool writable{m_service.writesAllowed(refusal)};
    m_busy->setVisible(busy);
    // Only what the selected row allows; nothing while no row is selected.
    m_primary_button->setVisible(false);
    m_ignore_button->setVisible(false);
    if (row) {
        m_ignore_button->setVisible(row->hidden || row->kind == Kind::Incoming || row->kind == Kind::Established);
        m_ignore_button->setText(row->hidden                      ? tr("Show again")
                                 : row->kind == Kind::Established ? tr("Hide contact")
                                                                  : tr("Ignore"));
        m_primary_button->setVisible(!row->hidden && row->kind != Kind::Outgoing && row->kind != Kind::Established);
        switch (row->kind) {
        case Kind::Incoming:
            m_primary_button->setText(tr("Accept"));
            break;
        case Kind::Accepted:
            m_primary_button->setText(m_model->walletLocked() ? tr("Unlock to finish") : tr("Try again"));
            break;
        case Kind::Established:
        case Kind::Outgoing:
            break;
        }
        const bool confirming{row->identity_hex == m_service.pendingContactRequest()};
        m_primary_button->setEnabled(!busy && !confirming && (row->kind != Kind::Incoming || writable));
        m_ignore_button->setEnabled(!busy);
    }

    // One filled button: a row's answer when it needs one, else Add contact
    // (the empty state brings its own); none while registration still runs
    // and its card has the page's action.
    const bool registered{!m_service.myUsername().isEmpty()};
    m_add_button->setVisible(!showsEmptyState());
    for (QPushButton* add : {m_add_button, m_empty_add_button}) {
        add->setEnabled(registered && writable);
        add->setToolTip(registered ? tr("Add someone by their DashPay username")
                                   : tr("Available once your username is registered."));
    }
    PlatformUi::makeSecondary(m_primary_button, !registered);
    PlatformUi::makeSecondary(m_add_button, !registered || m_primary_button->isVisibleTo(this));
    PlatformUi::makeSecondary(m_empty_add_button, !registered);
    if (isEmpty() != m_was_empty || was_empty_state != showsEmptyState()) Q_EMIT emptyChanged();
    m_was_empty = isEmpty();
}

void ContactsPage::activateSelected()
{
    using Kind = ContactsModel::Kind;
    const auto* row{m_model->rowAt(m_view->currentIndex().row())};
    if (!row || row->hidden || !m_accepting_identity.isEmpty()) return;
    switch (row->kind) {
    case Kind::Incoming:
    case Kind::Accepted:
        acceptSelected();
        break;
    case Kind::Established:
    case Kind::Outgoing:
        break;
    }
}

void ContactsPage::acceptSelected()
{
    const auto* row{m_model->rowAt(m_view->currentIndex().row())};
    if (!row || (row->kind != ContactsModel::Kind::Incoming && row->kind != ContactsModel::Kind::Accepted)) return;
    m_accepting_identity = row->identity_hex;
    m_status->setMessage(Severity::Info,
                         tr("Accepting the request from %1…").arg(m_service.contactDisplayString(row->identity_hex)));
    m_refresh_failed = false;
    updateActions();
    QString error;
    if (!m_service.acceptContact(row->identity_hex, error)) {
        onContactRequestFinished(row->identity_hex, false, error, {});
    }
}

void ContactsPage::onContactRequestPending(const QString& id)
{
    if (id != m_accepting_identity) return;
    // Accepted on this side and sent; the row says when Platform confirms.
    m_accepting_identity.clear();
    m_confirming_identity = id;
    m_status->setMessage(Severity::Info, tr("Accepted. Your reply to %1 was sent — waiting for Dash Platform to "
                                            "confirm it.")
                                             .arg(m_service.contactDisplayString(id)));
    m_status->setAction({});
    updateActions();
}

void ContactsPage::onContactRequestFinished(const QString& id, bool ok, const QString& error, const QString& details)
{
    const bool confirming{id == m_confirming_identity};
    if (id != m_accepting_identity && !confirming) return;
    m_accepting_identity.clear();
    m_confirming_identity.clear();
    const QString name{m_service.contactDisplayString(id)};
    if (ok) {
        m_status->setTransientMessage(Severity::Success, tr("You and %1 are now contacts.").arg(name));
    } else if (!confirming) {
        m_status->setMessage(Severity::Error, tr("Could not accept the request from %1. %2").arg(name, error), details);
        m_status->setAction({});
    }
    updateActions();
    m_model->refresh();
}

void ContactsPage::setSelectedHidden(bool hidden)
{
    const auto* row{m_model->rowAt(m_view->currentIndex().row())};
    if (!row) return;
    const QString id{row->identity_hex};
    const QString name{m_service.contactDisplayString(id)};
    const bool established{row->kind == ContactsModel::Kind::Established};
    m_service.setHidden(id, hidden);
    if (!hidden) {
        m_status->clear();
    } else if (established) {
        m_status->setTransientMessage(Severity::Info,
                                      tr("%1 is hidden on this computer. Payments they send still arrive.").arg(name));
    } else {
        m_status->setTransientMessage(Severity::Info, tr("Request from %1 hidden on this computer. They are not "
                                                         "notified. Use “Show hidden” to see it again.")
                                                          .arg(name));
    }
}

void ContactsPage::showContextMenu(const QPoint& pos)
{
    using Kind = ContactsModel::Kind;
    const QModelIndex index{m_view->indexAt(pos)};
    if (!index.isValid()) return;
    m_view->setCurrentIndex(index);
    const auto* row{m_model->rowAt(index.row())};
    if (!row) return;
    const bool busy{!m_accepting_identity.isEmpty()};

    QMenu menu(this);
    if (row->hidden) {
        connect(menu.addAction(tr("Show again")), &QAction::triggered, this, [this] { setSelectedHidden(false); });
    } else {
        switch (row->kind) {
        case Kind::Established:
            break;
        case Kind::Incoming: {
            QString refusal;
            auto* accept{menu.addAction(tr("Accept request"))};
            accept->setEnabled(!busy && m_service.writesAllowed(refusal));
            connect(accept, &QAction::triggered, this, &ContactsPage::acceptSelected);
            break;
        }
        case Kind::Accepted: {
            auto* unlock{menu.addAction(m_model->walletLocked() ? tr("Unlock to finish connecting")
                                                                : tr("Try again to finish connecting"))};
            unlock->setEnabled(!busy);
            connect(unlock, &QAction::triggered, this, &ContactsPage::acceptSelected);
            break;
        }
        case Kind::Outgoing:
            menu.addAction(tr("Waiting for them to accept"))->setEnabled(false);
            break;
        }
    }
    menu.addSeparator();
    if (!row->username.isEmpty()) {
        connect(menu.addAction(tr("Copy username")), &QAction::triggered, this,
                [username = row->username] { GUIUtil::setClipboard(username); });
    }
    connect(menu.addAction(tr("Copy identity ID")), &QAction::triggered, this,
            [id = PlatformUi::identityIdBase58(row->identity_hex)] { GUIUtil::setClipboard(id); });
    if (!row->hidden && (row->kind == Kind::Established || row->kind == Kind::Incoming)) {
        menu.addSeparator();
        connect(menu.addAction(row->kind == Kind::Established ? tr("Hide contact") : tr("Ignore request")),
                &QAction::triggered, this, [this] { setSelectedHidden(true); });
    }
    menu.exec(m_view->viewport()->mapToGlobal(pos));
}
