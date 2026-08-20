// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_CONTACTPICKERDIALOG_H
#define BITCOIN_QT_PLATFORM_CONTACTPICKERDIALOG_H

#include <QDialog>
#include <QString>

class ContactsModel;
class PlatformService;

QT_BEGIN_NAMESPACE
class QLabel;
class QPushButton;
class QSortFilterProxyModel;
class QTableView;
QT_END_NAMESPACE

/** Modal picker over the established (payable) contacts. On accept,
 *  selectedUsername() holds the chosen contact's DashPay username. */
class ContactPickerDialog : public QDialog
{
    Q_OBJECT

public:
    explicit ContactPickerDialog(PlatformService& service, QWidget* parent = nullptr);

    QString selectedUsername() const { return m_selected_username; }

private Q_SLOTS:
    void updateState();
    void chooseCurrent();

private:
    PlatformService& m_service;
    ContactsModel* m_model{nullptr};
    QSortFilterProxyModel* m_proxy{nullptr};
    QTableView* m_view{nullptr};
    QLabel* m_empty{nullptr};
    QPushButton* m_choose_button{nullptr};
    QString m_selected_username;
};

#endif // BITCOIN_QT_PLATFORM_CONTACTPICKERDIALOG_H
