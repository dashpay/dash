// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/contactspage.h>

#include <qt/guiutil.h>
#include <qt/platform/contactsmodel.h>
#include <qt/platform/platformservice.h>

#include <QHBoxLayout>
#include <QHeaderView>
#include <QItemSelectionModel>
#include <QLabel>
#include <QMenu>
#include <QPushButton>
#include <QTableView>
#include <QVBoxLayout>

ContactsPage::ContactsPage(PlatformService& service, QWidget* parent) :
    QWidget(parent),
    m_service(service)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* heading = new QLabel(tr("Contacts"), this);
    GUIUtil::setFont({heading}, GUIUtil::FontWeight::Bold, 16);
    layout->addWidget(heading);

    auto* buttons = new QHBoxLayout();
    m_send_button = new QPushButton(tr("Send Dash"), this);
    m_send_button->setToolTip(tr("Pay the selected contact by username"));
    m_accept_button = new QPushButton(tr("Accept request"), this);
    m_accept_button->setToolTip(tr("Connect with this person so you can pay each other by username"));
    auto* refresh = new QPushButton(tr("Refresh"), this);
    buttons->addWidget(m_send_button);
    buttons->addWidget(m_accept_button);
    buttons->addStretch();
    buttons->addWidget(refresh);
    layout->addLayout(buttons);

    m_model = new ContactsModel(m_service, this);
    m_view = new QTableView(this);
    m_view->setModel(m_model);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setSelectionMode(QAbstractItemView::SingleSelection);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_view->setContextMenuPolicy(Qt::CustomContextMenu);
    m_view->horizontalHeader()->setStretchLastSection(true);
    m_view->verticalHeader()->hide();
    layout->addWidget(m_view);

    m_empty = new QLabel(tr("No contacts yet.\nUse \"Find people\" above to look someone up by "
                            "username and send them a contact request. Once they accept, you can "
                            "pay each other by name."),
                         this);
    m_empty->setWordWrap(true);
    m_empty->setAlignment(Qt::AlignCenter);
    layout->addWidget(m_empty);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);

    connect(refresh, &QPushButton::clicked, this, [this] { m_model->refresh(); });
    connect(m_send_button, &QPushButton::clicked, this, &ContactsPage::sendToSelected);
    connect(m_accept_button, &QPushButton::clicked, this, &ContactsPage::acceptSelected);
    connect(m_view->selectionModel(), &QItemSelectionModel::currentChanged,
            this, &ContactsPage::updateActions);
    connect(m_view, &QTableView::doubleClicked, this, &ContactsPage::sendToSelected);
    connect(m_view, &QTableView::customContextMenuRequested, this, &ContactsPage::showContextMenu);
    connect(m_model, &QAbstractItemModel::modelReset, this, &ContactsPage::updateActions);
    connect(&m_service, &PlatformService::contactRequestFinished, this, &ContactsPage::onContactRequestFinished);

    updateActions();
    m_model->refresh();
}

void ContactsPage::updateActions()
{
    const auto* row{m_model->rowAt(m_view->currentIndex().row())};
    m_accept_button->setEnabled(row && row->kind == ContactsModel::Kind::Incoming && m_accepting_identity.isEmpty());
    m_send_button->setEnabled(row && row->kind == ContactsModel::Kind::Established &&
                              !row->username.isEmpty());
    const bool have_rows{m_model->rowCount() > 0};
    m_view->setVisible(have_rows);
    m_empty->setVisible(!have_rows);
}

void ContactsPage::acceptSelected()
{
    const auto* row{m_model->rowAt(m_view->currentIndex().row())};
    if (!row || row->kind != ContactsModel::Kind::Incoming) return;
    m_accepting_identity = row->identity_hex;
    m_status->setStyleSheet({});
    m_status->setText(tr("Accepting the request from %1…").arg(m_service.contactDisplayString(row->identity_hex)));
    updateActions();
    QString error;
    if (!m_service.acceptContact(row->identity_hex, error)) {
        onContactRequestFinished(row->identity_hex, false, error);
    }
}

void ContactsPage::onContactRequestFinished(const QString& id, bool ok, const QString& error)
{
    if (id != m_accepting_identity) return;
    m_accepting_identity.clear();
    if (ok) {
        m_status->setStyleSheet(
            QString{"QLabel { color: %1; }"}.arg(GUIUtil::getThemedQColor(GUIUtil::ThemedColor::GREEN).name()));
        m_status->setText(tr("You and %1 are now contacts.").arg(m_service.contactDisplayString(id)));
    } else {
        m_status->setStyleSheet(
            QString{"QLabel { color: %1; }"}.arg(GUIUtil::getThemedQColor(GUIUtil::ThemedColor::RED).name()));
        m_status->setText(tr("Could not accept the request from %1: %2").arg(m_service.contactDisplayString(id), error));
    }
    updateActions();
    m_model->refresh();
}

void ContactsPage::sendToSelected()
{
    const auto* row{m_model->rowAt(m_view->currentIndex().row())};
    if (!row || row->kind != ContactsModel::Kind::Established || row->username.isEmpty()) return;
    Q_EMIT sendToContact(row->username);
}

void ContactsPage::showContextMenu(const QPoint& pos)
{
    const QModelIndex index{m_view->indexAt(pos)};
    if (!index.isValid()) return;
    m_view->setCurrentIndex(index);
    const auto* row{m_model->rowAt(index.row())};
    if (!row) return;

    QMenu menu(this);
    switch (row->kind) {
    case ContactsModel::Kind::Established: {
        auto* send{menu.addAction(tr("Send Dash to %1").arg(row->username))};
        send->setEnabled(!row->username.isEmpty());
        connect(send, &QAction::triggered, this, &ContactsPage::sendToSelected);
        break;
    }
    case ContactsModel::Kind::Incoming: {
        auto* accept{menu.addAction(tr("Accept contact request"))};
        accept->setEnabled(m_accepting_identity.isEmpty());
        connect(accept, &QAction::triggered, this, &ContactsPage::acceptSelected);
        break;
    }
    case ContactsModel::Kind::Outgoing: {
        menu.addAction(tr("Waiting for them to accept your request"))->setEnabled(false);
        break;
    }
    }
    auto* copy{menu.addAction(tr("Copy identity ID"))};
    connect(copy, &QAction::triggered, this, [row_id = row->identity_hex] {
        GUIUtil::setClipboard(row_id);
    });
    menu.exec(m_view->viewport()->mapToGlobal(pos));
}
