// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/contactspage.h>

#include <base58.h>
#include <qt/guiutil.h>
#include <qt/platform/contactflow.h>

#include <QCheckBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QTableWidget>
#include <QVBoxLayout>

ContactsPage::ContactsPage(QWidget* parent) :
    QWidget(parent)
{
    auto* title{new QLabel(tr("Contacts"), this)};
    GUIUtil::setFont({title}, GUIUtil::FontWeight::Bold);
    m_username = new QLineEdit(this);
    m_username->setObjectName("dashPayContactUsername");
    m_username->setPlaceholderText(tr("Username"));
    m_add = new QPushButton(tr("Add contact"), this);
    m_add->setObjectName("dashPayAddContact");
    m_search = new QPushButton(tr("Search…"), this);
    m_search->setObjectName("dashPaySearchContacts");
    m_refresh = new QPushButton(tr("Refresh"), this);
    m_refresh->setObjectName("dashPayRefreshContacts");
    m_status = new QLabel(this);
    m_status->setObjectName("dashPayContactsStatus");
    m_status->setWordWrap(true);
    m_show_ignored = new QCheckBox(tr("Show ignored requests"), this);
    m_show_ignored->setObjectName("dashPayShowIgnored");
    m_list = new QTableWidget(0, 4, this);
    m_list->setObjectName("dashPayContacts");
    m_list->setHorizontalHeaderLabels({tr("Username"), tr("Status"), {}, {}});
    m_list->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_list->verticalHeader()->hide();
    m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_list->setSelectionMode(QAbstractItemView::NoSelection);

    auto* add_row{new QHBoxLayout};
    add_row->addWidget(m_username);
    add_row->addWidget(m_add);
    add_row->addWidget(m_search);
    add_row->addStretch();
    add_row->addWidget(m_refresh);
    auto* layout{new QVBoxLayout(this)};
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(title);
    layout->addLayout(add_row);
    layout->addWidget(m_status);
    layout->addWidget(m_show_ignored);
    layout->addWidget(m_list);

    const auto add = [this] { m_flow->add(m_username->text().trimmed().toStdString()); };
    connect(m_add, &QPushButton::clicked, this, add);
    connect(m_username, &QLineEdit::returnPressed, this, add);
    connect(m_search, &QPushButton::clicked, this, &ContactsPage::searchRequested);
    connect(m_refresh, &QPushButton::clicked, this, [this] { m_flow->refresh(); });
    connect(m_show_ignored, &QCheckBox::toggled, this, [this] { refresh(m_writable); });
}

void ContactsPage::refresh(bool writable)
{
    if (!m_flow) return;
    m_writable = writable;
    using Step = ContactFlow::Step;
    QString status;
    switch (m_flow->step()) {
    case Step::IDLE:
        status = m_flow->error();
        if (status.isEmpty() && !m_flow->swept()) status = tr("Reading your contacts…");
        break;
    case Step::RESOLVING:
    case Step::FETCHING:
    case Step::PAYING:
        status = tr("Looking up the contact…");
        break;
    case Step::SENDING:
        status = tr("Sending the contact request…");
        break;
    }
    m_status->setText(status);
    m_status->setVisible(!status.isEmpty());
    const bool ready{writable && m_flow->swept()};
    m_username->setVisible(ready);
    m_add->setVisible(ready);
    m_search->setVisible(ready);
    m_refresh->setVisible(m_flow->step() == Step::IDLE);

    auto contacts{m_flow->contacts()};
    const auto ignored{m_flow->ignored()};
    if (ignored.empty() && m_flow->swept()) {
        // Nothing left to show: the next ignored request starts hidden. Not
        // while the contacts are being read again, when nothing is listed.
        const QSignalBlocker blocker{m_show_ignored};
        m_show_ignored->setChecked(false);
    }
    m_show_ignored->setVisible(!ignored.empty());
    const int not_ignored{static_cast<int>(contacts.size())};
    if (m_show_ignored->isChecked()) contacts.insert(contacts.end(), ignored.begin(), ignored.end());
    m_list->setRowCount(static_cast<int>(contacts.size()));
    for (int row = 0; row < m_list->rowCount(); ++row) {
        const ContactFlow::Contact& contact{contacts[row]};
        const bool is_ignored{row >= not_ignored};
        const QString name{QString::fromStdString(contact.username.value_or(EncodeBase58(contact.id)))};
        m_list->setItem(row, 0, new QTableWidgetItem(name));
        QString state;
        switch (contact.state) {
        case ContactFlow::State::INCOMING:
            state = tr("Wants to add you");
            break;
        case ContactFlow::State::ESTABLISHED:
            state = tr("Contact");
            break;
        case ContactFlow::State::OUTGOING:
            state = tr("Request sent");
            break;
        }
        if (is_ignored) state = tr("Ignored");
        m_list->setItem(row, 1, new QTableWidgetItem(state));
        m_list->removeCellWidget(row, 2);
        m_list->removeCellWidget(row, 3);
        const auto button = [&](int column, const QString& text, const char* name,
                                void (ContactFlow::*action)(const platform::Identifier&)) {
            auto* b{new QPushButton(text, m_list)};
            b->setObjectName(name);
            connect(b, &QPushButton::clicked, this, [this, action, id = contact.id] { (m_flow->*action)(id); });
            m_list->setCellWidget(row, column, b);
        };
        if (ready && is_ignored) {
            button(3, tr("Unignore"), "dashPayUnignoreContact", &ContactFlow::unignore);
        } else if (ready && contact.state == ContactFlow::State::INCOMING) {
            button(2, tr("Accept"), "dashPayAcceptContact", &ContactFlow::accept);
            button(3, tr("Ignore"), "dashPayIgnoreContact", &ContactFlow::ignore);
        } else if (ready && contact.state == ContactFlow::State::ESTABLISHED) {
            button(2, tr("Pay"), "dashPayPayContact", &ContactFlow::pay);
        }
    }
    m_list->setVisible(!contacts.empty());
}
