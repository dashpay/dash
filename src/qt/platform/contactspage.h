// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_CONTACTSPAGE_H
#define BITCOIN_QT_PLATFORM_CONTACTSPAGE_H

#include <QWidget>

class ContactFlow;

QT_BEGIN_NAMESPACE
class QLabel;
class QLineEdit;
class QPushButton;
class QTableWidget;
QT_END_NAMESPACE

/** The DashPay tab's contacts: incoming requests with Accept, contacts with
 *  Pay and our requests waiting for an answer, and adding a contact by
 *  username. */
class ContactsPage : public QWidget
{
    Q_OBJECT

public:
    explicit ContactsPage(QWidget* parent = nullptr);

    void setFlow(ContactFlow* flow) { m_flow = flow; }
    //! Show the flow's contacts; offer adding, accepting and paying only
    //! when `writable`: no DashPay flow, this one included, is busy.
    void refresh(bool writable);

private:
    ContactFlow* m_flow{nullptr};
    QLineEdit* m_username{nullptr};
    QPushButton* m_add{nullptr};
    QPushButton* m_refresh{nullptr};
    QLabel* m_status{nullptr};
    QTableWidget* m_list{nullptr};
};

#endif // BITCOIN_QT_PLATFORM_CONTACTSPAGE_H
