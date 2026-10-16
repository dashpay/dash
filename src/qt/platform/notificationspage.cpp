// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/notificationspage.h>

#include <qt/guiutil.h>
#include <qt/platform/notificationfeed.h>

#include <QDateTime>
#include <QHeaderView>
#include <QLabel>
#include <QTableWidget>
#include <QVBoxLayout>

NotificationsPage::NotificationsPage(QWidget* parent) :
    QWidget(parent)
{
    auto* title{new QLabel(tr("Notifications"), this)};
    GUIUtil::setFont({title}, GUIUtil::FontWeight::Bold);
    m_list = new QTableWidget(0, 2, this);
    m_list->setObjectName("dashPayNotifications");
    m_list->setHorizontalHeaderLabels({tr("Notification"), tr("Date")});
    m_list->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    m_list->verticalHeader()->hide();
    m_list->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_list->setSelectionMode(QAbstractItemView::NoSelection);
    auto* layout{new QVBoxLayout(this)};
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(title);
    layout->addWidget(m_list);
}

void NotificationsPage::refresh(bool shown)
{
    const auto entries{shown && m_feed ? m_feed->entries() : std::vector<NotificationFeed::Entry>{}};
    m_list->setRowCount(static_cast<int>(entries.size()));
    for (int row = 0; row < m_list->rowCount(); ++row) {
        m_list->setItem(row, 0, new QTableWidgetItem(m_feed->text(entries[row])));
        m_list->setItem(row, 1,
                        new QTableWidgetItem(GUIUtil::dateTimeStr(QDateTime::fromMSecsSinceEpoch(entries[row].time_ms))));
    }
    setVisible(!entries.empty());
}
