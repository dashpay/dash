// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_NOTIFICATIONSPAGE_H
#define BITCOIN_QT_PLATFORM_NOTIFICATIONSPAGE_H

#include <QWidget>

class NotificationFeed;

QT_BEGIN_NAMESPACE
class QTableWidget;
QT_END_NAMESPACE

/** The DashPay tab's notifications: the feed's entries, newest first. */
class NotificationsPage : public QWidget
{
    Q_OBJECT

public:
    explicit NotificationsPage(QWidget* parent = nullptr);

    void setFeed(NotificationFeed* feed) { m_feed = feed; }
    //! List the feed's entries; shown only when `shown` and there are some.
    void refresh(bool shown);

private:
    NotificationFeed* m_feed{nullptr};
    QTableWidget* m_list{nullptr};
};

#endif // BITCOIN_QT_PLATFORM_NOTIFICATIONSPAGE_H
