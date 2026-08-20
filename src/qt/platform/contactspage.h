// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_CONTACTSPAGE_H
#define BITCOIN_QT_PLATFORM_CONTACTSPAGE_H

#include <QString>
#include <QWidget>

class ContactsModel;
class PlatformService;

QT_BEGIN_NAMESPACE
class QLabel;
class QPoint;
class QTableView;
class QPushButton;
QT_END_NAMESPACE

/** Contacts list: established contacts and pending requests, with accept /
 *  pay actions. Finding new contacts and editing the profile live on the
 *  surrounding dashboard (PlatformPage). */
class ContactsPage : public QWidget
{
    Q_OBJECT

public:
    explicit ContactsPage(PlatformService& service, QWidget* parent = nullptr);

Q_SIGNALS:
    //! Ask the wallet view to open the Send tab with this username prefilled.
    void sendToContact(const QString& username);

private Q_SLOTS:
    void acceptSelected();
    void sendToSelected();
    void updateActions();
    void showContextMenu(const QPoint& pos);
    void onContactRequestFinished(const QString& id, bool ok, const QString& error);

private:
    PlatformService& m_service;
    ContactsModel* m_model{nullptr};
    QTableView* m_view{nullptr};
    QLabel* m_empty{nullptr};
    QLabel* m_status{nullptr};
    QPushButton* m_accept_button{nullptr};
    QPushButton* m_send_button{nullptr};
    QString m_accepting_identity;
};

#endif // BITCOIN_QT_PLATFORM_CONTACTSPAGE_H
