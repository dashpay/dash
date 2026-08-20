// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/contactpickerdialog.h>

#include <qt/platform/contactsmodel.h>
#include <qt/platform/platformservice.h>

#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QTableView>
#include <QVBoxLayout>

ContactPickerDialog::ContactPickerDialog(PlatformService& service, QWidget* parent) :
    QDialog(parent),
    m_service(service)
{
    setWindowTitle(tr("Pay a DashPay contact"));
    resize(440, 360);

    auto* layout = new QVBoxLayout(this);
    auto* hint = new QLabel(tr("Choose who to pay. The payment goes to a fresh address that "
                               "only you and this contact can link to each other."), this);
    hint->setWordWrap(true);
    layout->addWidget(hint);

    m_model = new ContactsModel(m_service, this);
    m_proxy = new QSortFilterProxyModel(this);
    m_proxy->setSourceModel(m_model);
    m_proxy->setFilterRole(ContactsModel::KindRole);
    m_proxy->setFilterFixedString(QString::number(static_cast<int>(ContactsModel::Kind::Established)));
    m_proxy->setFilterKeyColumn(0);

    m_view = new QTableView(this);
    m_view->setModel(m_proxy);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setSelectionMode(QAbstractItemView::SingleSelection);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_view->horizontalHeader()->setStretchLastSection(true);
    m_view->verticalHeader()->hide();
    m_view->hideColumn(ContactsModel::Direction);
    layout->addWidget(m_view);

    m_empty = new QLabel(tr("You do not have any payable contacts yet.\n"
                            "Open the DashPay tab and use \"Find people\" to send a contact "
                            "request. Once it is accepted you can pay them by username."),
                         this);
    m_empty->setWordWrap(true);
    m_empty->setAlignment(Qt::AlignCenter);
    layout->addWidget(m_empty);

    auto* buttons = new QDialogButtonBox(this);
    m_choose_button = buttons->addButton(tr("Pay this contact"), QDialogButtonBox::AcceptRole);
    buttons->addButton(QDialogButtonBox::Cancel);
    layout->addWidget(buttons);

    connect(buttons, &QDialogButtonBox::accepted, this, &ContactPickerDialog::chooseCurrent);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_view, &QTableView::doubleClicked, this, &ContactPickerDialog::chooseCurrent);
    connect(m_view->selectionModel(), &QItemSelectionModel::currentChanged,
            this, &ContactPickerDialog::updateState);
    connect(m_proxy, &QAbstractItemModel::modelReset, this, &ContactPickerDialog::updateState);
    connect(m_proxy, &QAbstractItemModel::rowsInserted, this, &ContactPickerDialog::updateState);
    connect(m_proxy, &QAbstractItemModel::rowsRemoved, this, &ContactPickerDialog::updateState);

    updateState();
    m_model->refresh();
}

void ContactPickerDialog::updateState()
{
    const bool have_contacts{m_proxy->rowCount() > 0};
    m_view->setVisible(have_contacts);
    m_empty->setVisible(!have_contacts);
    const QModelIndex current{m_view->currentIndex()};
    m_choose_button->setEnabled(current.isValid() &&
                                !m_proxy->data(current, ContactsModel::UsernameRole).toString().isEmpty());
}

void ContactPickerDialog::chooseCurrent()
{
    const QModelIndex current{m_view->currentIndex()};
    if (!current.isValid()) return;
    const QString username{m_proxy->data(current, ContactsModel::UsernameRole).toString()};
    if (username.isEmpty()) return;
    m_selected_username = username;
    accept();
}
