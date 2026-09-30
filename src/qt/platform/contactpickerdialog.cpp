// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/contactpickerdialog.h>

#include <qt/guiutil.h>
#include <qt/platform/contactsmodel.h>
#include <qt/platform/platformservice.h>
#include <qt/platform/platformui.h>

#include <QDialogButtonBox>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QSortFilterProxyModel>
#include <QTableView>
#include <QVBoxLayout>

ContactPickerDialog::ContactPickerDialog(PlatformService& service, QWidget* parent) :
    QDialog(parent, GUIUtil::dialog_flags),
    m_service(service)
{
    setWindowTitle(tr("Pay a DashPay contact"));
    resize(440, 360);

    auto* layout = new QVBoxLayout(this);
    layout->addWidget(PlatformUi::makeHint(tr("Choose who to pay. The payment goes to a fresh address that only you "
                                              "and this contact can link."),
                                           this));

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

    m_empty = new QWidget(this);
    auto* empty_layout = new QVBoxLayout(m_empty);
    auto* empty_title = new QLabel(tr("No contacts to pay yet."), m_empty);
    empty_title->setAlignment(Qt::AlignCenter);
    auto* empty_hint = PlatformUi::makeHint(tr("Add contacts on the DashPay tab. Once they accept, you can pay them "
                                               "here by username."),
                                            m_empty);
    empty_hint->setAlignment(Qt::AlignCenter);
    empty_layout->addStretch();
    empty_layout->addWidget(empty_title);
    empty_layout->addWidget(empty_hint);
    empty_layout->addStretch();
    layout->addWidget(m_empty);
    m_loading = new QWidget(this);
    auto* loading_layout = new QVBoxLayout(m_loading);
    auto* loading_bar = PlatformUi::makeBusyBar(m_loading);
    loading_bar->show();
    auto* loading_text = PlatformUi::makeHint(tr("Loading contacts…"), m_loading);
    loading_text->setAlignment(Qt::AlignCenter);
    loading_layout->addStretch();
    loading_layout->addWidget(loading_bar);
    loading_layout->addWidget(loading_text);
    loading_layout->addStretch();
    layout->addWidget(m_loading);

    auto* buttons = new QDialogButtonBox(this);
    auto* cancel = buttons->addButton(QDialogButtonBox::Cancel);
    PlatformUi::makeSecondary(cancel);
    m_choose_button = buttons->addButton(tr("Pay"), QDialogButtonBox::AcceptRole);
    m_choose_button->setDefault(true);
    layout->addWidget(buttons);
    setTabOrder(m_view, m_choose_button);
    setTabOrder(m_choose_button, cancel);

    connect(buttons, &QDialogButtonBox::accepted, this, &ContactPickerDialog::chooseCurrent);
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_view, &QTableView::activated, this, &ContactPickerDialog::chooseCurrent);
    connect(m_view->selectionModel(), &QItemSelectionModel::currentChanged, this, &ContactPickerDialog::updateState);
    connect(m_proxy, &QAbstractItemModel::modelReset, this, &ContactPickerDialog::updateState);
    connect(m_proxy, &QAbstractItemModel::rowsInserted, this, &ContactPickerDialog::updateState);
    connect(m_proxy, &QAbstractItemModel::rowsRemoved, this, &ContactPickerDialog::updateState);

    GUIUtil::updateFonts();
    GUIUtil::disableMacFocusRect(this);
    updateState();
    m_model->refresh();
}

void ContactPickerDialog::updateState()
{
    const bool have_contacts{m_proxy->rowCount() > 0};
    // No column of blanks when nobody has a profile.
    m_view->setColumnHidden(ContactsModel::DisplayName, !m_model->hasDisplayNames());
    m_view->setVisible(have_contacts);
    m_empty->setVisible(!have_contacts && m_model->loaded());
    m_loading->setVisible(!have_contacts && !m_model->loaded());
    // The first contact is ready to pay: Enter pays it.
    if (have_contacts && !m_view->currentIndex().isValid()) {
        m_view->setCurrentIndex(m_proxy->index(0, 0));
        m_view->setFocus();
    }
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
