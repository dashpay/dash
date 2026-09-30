// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/usernamesearchdialog.h>

#include <platform/walletrecords.h>
#include <qt/guiutil.h>
#include <qt/platform/platformservice.h>
#include <qt/platform/platformui.h>
#include <util/strencodings.h>

#include <QEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QTableWidget>
#include <QTimer>
#include <QVBoxLayout>

using Severity = PlatformUi::MessageLine::Severity;

namespace {
constexpr int DEBOUNCE_MS{400};
//! A username has at least three characters: nothing shorter is looked up.
constexpr int MIN_SEARCH_LENGTH{3};
enum Column {
    USERNAME = 0,
    PROFILE_NAME,
    NOTE,
    COLUMN_COUNT
};
} // namespace

UsernameSearchDialog::UsernameSearchDialog(PlatformService& service, QWidget* parent) :
    QDialog(parent, GUIUtil::dialog_flags),
    m_service(service)
{
    setWindowTitle(tr("Add a contact"));
    resize(460, 460);
    auto* layout = new QVBoxLayout(this);

    layout->addWidget(PlatformUi::makeHint(tr("Type the username of the person you want to add. They get a contact "
                                              "request, and once they accept, you can pay each other by username."),
                                           this));

    auto* input_label = new QLabel(tr("Username"), this);
    m_input = new QLineEdit(this);
    m_input->setPlaceholderText(tr("Their DashPay username"));
    m_input->installEventFilter(this);
    input_label->setBuddy(m_input);
    layout->addWidget(input_label);
    layout->addWidget(m_input);
    m_busy = PlatformUi::makeBusyBar(this);
    layout->addWidget(m_busy);

    m_table = new QTableWidget(0, COLUMN_COUNT, this);
    m_table->setHorizontalHeaderLabels({tr("Username"), tr("Profile name"), tr("Note")});
    m_table->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    // Columns keep their widths from one search to the next.
    m_table->horizontalHeader()->setSectionResizeMode(USERNAME, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(PROFILE_NAME, QHeaderView::Stretch);
    m_table->horizontalHeader()->setSectionResizeMode(NOTE, QHeaderView::ResizeToContents);
    m_table->setColumnHidden(NOTE, true);
    m_table->verticalHeader()->hide();
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    layout->addWidget(m_table, /*stretch=*/1);

    m_footer = PlatformUi::makeHint({}, this);
    m_footer->hide();
    layout->addWidget(m_footer);
    layout->addWidget(PlatformUi::makeHint(tr("The evonode that answers can see the usernames you look up."), this));
    m_status = new PlatformUi::MessageLine(this);
    layout->addWidget(m_status);

    // Laid out by hand: a button box moves an action button to the far left
    // on macOS.
    auto* buttons = new QHBoxLayout();
    auto* close = new QPushButton(tr("Close"), this);
    PlatformUi::makeSecondary(close);
    close->setAutoDefault(false);
    m_add = new QPushButton(tr("Send contact request"), this);
    buttons->addStretch();
    buttons->addWidget(close);
    buttons->addWidget(m_add);
    layout->addLayout(buttons);

    m_debounce = new QTimer(this);
    m_debounce->setSingleShot(true);
    m_debounce->setInterval(DEBOUNCE_MS);

    connect(m_input, &QLineEdit::textChanged, this, &UsernameSearchDialog::onTextChanged);
    connect(m_debounce, &QTimer::timeout, this, [this] {
        const QString t = m_input->text().trimmed();
        if (t.size() >= MIN_SEARCH_LENGTH) m_service.searchNames(t);
    });
    connect(&m_service, &PlatformService::searchResults, this, &UsernameSearchDialog::onResults);
    connect(&m_service, &PlatformService::searchFailed, this, &UsernameSearchDialog::onSearchFailed);
    connect(m_table, &QTableWidget::currentCellChanged, this, &UsernameSearchDialog::updateActions);
    // Whether the chosen person can receive a request is read when they are
    // chosen, not for every result: one read, not a page of them.
    connect(m_table, &QTableWidget::currentCellChanged, this, [this](int row) {
        const auto* item{m_table->item(row, USERNAME)};
        if (item && (item->flags() & Qt::ItemIsEnabled)) m_service.checkRecipient(item->data(Qt::UserRole).toString());
    });
    connect(&m_service, &PlatformService::recipientChecked, this, [this](const QString& id, bool can_receive) {
        if (!can_receive) markUnavailable(id, tr("Can't receive contact requests"));
    });
    // A request costs a Platform fee: never on the single click some platforms
    // activate rows with. Enter reaches the default button.
    connect(m_table, &QTableWidget::doubleClicked, this, &UsernameSearchDialog::addSelected);
    connect(m_add, &QPushButton::clicked, this, &UsernameSearchDialog::addSelected);
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    connect(&m_service, &PlatformService::contactRequestFinished, this, &UsernameSearchDialog::onContactRequestFinished);
    connect(&m_service, &PlatformService::contactRequestPending, this, &UsernameSearchDialog::onContactRequestPending);
    connect(&m_service, &PlatformService::searchProfileLoaded, this,
            [this](const QString& identity_hex, const QString& display_name) {
                for (int row = 0; row < m_table->rowCount(); ++row) {
                    if (m_table->item(row, USERNAME)->data(Qt::UserRole).toString() == identity_hex) {
                        m_table->item(row, PROFILE_NAME)->setText(display_name);
                    }
                }
            });

    GUIUtil::updateFonts();
    GUIUtil::disableMacFocusRect(this);
    m_input->setFocus();
    updateActions();
}

void UsernameSearchDialog::changeEvent(QEvent* event)
{
    QDialog::changeEvent(event);
    if (event->type() == QEvent::StyleChange) PlatformUi::restyle(this);
}

bool UsernameSearchDialog::eventFilter(QObject* watched, QEvent* event)
{
    // Down from the search field moves to the first row that can be asked.
    if (watched == m_input && event->type() == QEvent::KeyPress && static_cast<QKeyEvent*>(event)->key() == Qt::Key_Down) {
        for (int row = 0; row < m_table->rowCount(); ++row) {
            if (m_table->item(row, USERNAME)->flags() & Qt::ItemIsEnabled) {
                m_table->setFocus();
                m_table->setCurrentCell(row, USERNAME);
                return true;
            }
        }
    }
    return QDialog::eventFilter(watched, event);
}

void UsernameSearchDialog::setBusy(bool busy)
{
    m_busy->setVisible(busy);
    m_add->setDefault(!busy);
}

void UsernameSearchDialog::onTextChanged()
{
    m_debounce->stop();
    m_table->setRowCount(0);
    m_footer->hide();
    m_status->clear();
    const int typed{static_cast<int>(m_input->text().trimmed().size())};
    m_searching = typed >= MIN_SEARCH_LENGTH;
    if (m_searching) {
        m_status->setMessage(Severity::Info, tr("Searching…"));
        m_debounce->start();
    } else if (typed > 0) {
        m_status->setMessage(Severity::Info, tr("Type at least 3 characters."));
    }
    setBusy(m_searching);
    updateActions();
}

void UsernameSearchDialog::onSearchFailed(const QString& prefix, const QString& error, const QString& details)
{
    if (prefix.trimmed() != m_input->text().trimmed()) return;
    m_searching = false;
    setBusy(false);
    m_status->setMessage(Severity::Error, tr("Search didn't work. %1").arg(error), details);
    updateActions();
}

QString UsernameSearchDialog::noteFor(const QString& identity_hex) const
{
    const auto my_id{m_service.myIdentityId()};
    if (my_id && QString::fromStdString(HexStr(*my_id)) == identity_hex) return tr("This is you");
    if (m_service.isEstablished(identity_hex)) return tr("Already a contact");
    if (m_service.sentRequestTo(identity_hex)) return tr("Request sent");
    if (m_service.canReceiveContactRequests(identity_hex) == false) return tr("Can't receive contact requests");
    return {};
}

void UsernameSearchDialog::onResults(const QString& prefix, const QVector<QPair<QString, QString>>& results)
{
    if (prefix.trimmed() != m_input->text().trimmed()) return;
    m_searching = false;
    setBusy(false);
    m_table->setRowCount(0);
    for (const auto& [username, identity_hex] : results) {
        const int row{m_table->rowCount()};
        m_table->insertRow(row);
        // Someone ignored can still be asked: asking brings them back.
        const bool selectable{noteFor(identity_hex).isEmpty()};
        const QString note{selectable && m_service.isHidden(identity_hex) ? tr("Ignored") : noteFor(identity_hex)};
        const QString display_name{m_service.contactMetadata(identity_hex, platform::records::CONTACT_DISPLAY_NAME_PREFIX)};
        for (const auto& [column, text] :
             {std::pair{USERNAME, username}, std::pair{PROFILE_NAME, display_name}, std::pair{NOTE, note}}) {
            auto* item{new QTableWidgetItem(text)};
            item->setData(Qt::UserRole, identity_hex);
            if (column == NOTE) item->setForeground(GUIUtil::getThemedQColor(GUIUtil::ThemedColor::UNCONFIRMED));
            if (!selectable) item->setFlags(item->flags() & ~Qt::ItemIsSelectable & ~Qt::ItemIsEnabled);
            m_table->setItem(row, column, item);
        }
    }
    if (results.isEmpty()) {
        m_status->setMessage(Severity::Info, tr("No usernames start with “%1”.").arg(prefix.trimmed()));
    } else {
        m_status->clear();
    }
    updateNoteColumn();
    m_footer->setText(
        tr("Showing the first %1 matches. Type more letters to narrow the search.").arg(PlatformService::SEARCH_PAGE_SIZE));
    m_footer->setVisible(results.size() >= static_cast<int>(PlatformService::SEARCH_PAGE_SIZE));
    updateActions();
}

void UsernameSearchDialog::updateActions()
{
    const auto* item{m_table->item(m_table->currentRow(), USERNAME)};
    const bool valid_selection{item && (item->flags() & Qt::ItemIsEnabled)};
    m_add->setEnabled(valid_selection && !m_searching && m_pending_identity.isEmpty());
}

void UsernameSearchDialog::addSelected()
{
    const auto* item{m_table->item(m_table->currentRow(), USERNAME)};
    if (!item || !(item->flags() & Qt::ItemIsEnabled) || !m_pending_identity.isEmpty()) return;
    m_pending_identity = item->data(Qt::UserRole).toString();
    m_input->setEnabled(false);
    m_table->setEnabled(false);
    setBusy(true);
    updateActions();
    m_status->setMessage(Severity::Info, tr("Sending a contact request to %1… This can take up to 30 seconds.")
                                             .arg(m_service.contactDisplayString(m_pending_identity)));
    QString error;
    if (!m_service.sendContactRequest(m_pending_identity, error)) {
        onContactRequestFinished(m_pending_identity, false, error, {});
    }
}

void UsernameSearchDialog::markUnavailable(const QString& id, const QString& note)
{
    for (int row = 0; row < m_table->rowCount(); ++row) {
        if (m_table->item(row, USERNAME)->data(Qt::UserRole).toString() != id) continue;
        m_table->item(row, NOTE)->setText(note);
        for (int column = 0; column < COLUMN_COUNT; ++column) {
            auto* item{m_table->item(row, column)};
            item->setFlags(item->flags() & ~Qt::ItemIsSelectable & ~Qt::ItemIsEnabled);
        }
        if (m_table->currentRow() == row) {
            m_table->clearSelection();
            m_table->setCurrentCell(-1, -1);
        }
        break;
    }
    updateNoteColumn();
    updateActions();
}

void UsernameSearchDialog::updateNoteColumn()
{
    bool any_note{false};
    for (int row = 0; row < m_table->rowCount() && !any_note; ++row) {
        any_note = !m_table->item(row, NOTE)->text().isEmpty();
    }
    m_table->setColumnHidden(NOTE, !any_note);
}

void UsernameSearchDialog::finishSending()
{
    m_input->setEnabled(true);
    m_table->setEnabled(true);
    setBusy(false);
    updateActions();
}

void UsernameSearchDialog::onContactRequestPending(const QString& id)
{
    if (m_pending_identity != id) return;
    // Sent: waiting for Platform is no reason to hold the dialog.
    m_pending_identity.clear();
    m_confirming_identity = id;
    markUnavailable(id, tr("Request sent"));
    m_status->setMessage(Severity::Info, tr("Sent to %1 — waiting for Dash Platform to confirm. It shows in your "
                                            "contacts as a sent request once it does; there is nothing more to do.")
                                             .arg(m_service.contactDisplayString(id)));
    finishSending();
}

void UsernameSearchDialog::onContactRequestFinished(const QString& id, bool ok, const QString& error, const QString& details)
{
    if (m_confirming_identity == id) {
        // Only a confirmation ends the wait here; a request Platform
        // accepted for broadcast is never reported as not sent.
        m_confirming_identity.clear();
        if (!ok) return;
        m_status->setMessage(Severity::Success, tr("Contact request sent to %1. They'll appear in your contacts as "
                                                   "soon as they accept.")
                                                    .arg(m_service.contactDisplayString(id)));
        return;
    }
    if (m_pending_identity != id) return;
    m_pending_identity.clear();
    if (ok) {
        markUnavailable(id, tr("Request sent"));
        m_status->setMessage(Severity::Success, tr("Contact request sent to %1. They'll appear in your contacts as "
                                                   "soon as they accept.")
                                                    .arg(m_service.contactDisplayString(id)));
    } else {
        // Only reached before Platform accepted the request for broadcast.
        m_status->setMessage(Severity::Error, tr("The contact request was not sent. %1").arg(error), details);
    }
    finishSending();
}
