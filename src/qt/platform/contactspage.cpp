// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/contactspage.h>

#include <base58.h>
#include <qt/guiutil.h>
#include <qt/platform/contactflow.h>

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
    m_refresh = new QPushButton(tr("Refresh"), this);
    m_refresh->setObjectName("dashPayRefreshContacts");
    m_status = new QLabel(this);
    m_status->setObjectName("dashPayContactsStatus");
    m_status->setWordWrap(true);
    m_list = new QTableWidget(0, 3, this);
    m_list->setObjectName("dashPayContacts");
    m_list->setHorizontalHeaderLabels({tr("Username"), tr("Status"), {}});
    m_list->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_list->verticalHeader()->hide();
    m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_list->setSelectionMode(QAbstractItemView::NoSelection);

    auto* add_row{new QHBoxLayout};
    add_row->addWidget(m_username);
    add_row->addWidget(m_add);
    add_row->addStretch();
    add_row->addWidget(m_refresh);
    auto* layout{new QVBoxLayout(this)};
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(title);
    layout->addLayout(add_row);
    layout->addWidget(m_status);
    layout->addWidget(m_list);

    const auto add = [this] { m_flow->add(m_username->text().trimmed().toStdString()); };
    connect(m_add, &QPushButton::clicked, this, add);
    connect(m_username, &QLineEdit::returnPressed, this, add);
    connect(m_refresh, &QPushButton::clicked, this, [this] { m_flow->refresh(); });
}

void ContactsPage::refresh(bool writable)
{
    if (!m_flow) return;
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
    m_refresh->setVisible(m_flow->step() == Step::IDLE);

    const auto contacts{m_flow->contacts()};
    m_list->setRowCount(static_cast<int>(contacts.size()));
    for (int row = 0; row < m_list->rowCount(); ++row) {
        const ContactFlow::Contact& contact{contacts[row]};
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
        m_list->setItem(row, 1, new QTableWidgetItem(state));
        m_list->removeCellWidget(row, 2);
        if (ready && contact.state == ContactFlow::State::INCOMING) {
            auto* accept{new QPushButton(tr("Accept"), m_list)};
            accept->setObjectName("dashPayAcceptContact");
            connect(accept, &QPushButton::clicked, this, [this, id = contact.id] { m_flow->accept(id); });
            m_list->setCellWidget(row, 2, accept);
        } else if (ready && contact.state == ContactFlow::State::ESTABLISHED) {
            auto* pay{new QPushButton(tr("Pay"), m_list)};
            pay->setObjectName("dashPayPayContact");
            connect(pay, &QPushButton::clicked, this, [this, id = contact.id] { m_flow->pay(id); });
            m_list->setCellWidget(row, 2, pay);
        }
    }
    m_list->setVisible(!contacts.empty());
}
