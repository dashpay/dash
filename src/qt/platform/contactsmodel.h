// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_CONTACTSMODEL_H
#define BITCOIN_QT_PLATFORM_CONTACTSMODEL_H

#include <QAbstractTableModel>
#include <QString>
#include <QVector>

class PlatformService;

/** Table model backing the contacts list: established contacts plus incoming
 *  and outgoing contact requests, refreshed from the DashPay contactRequest
 *  documents via the PlatformService. */
class ContactsModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum Column { Username = 0, DisplayName, Direction, COLUMN_COUNT };
    enum class Kind { Established, Incoming, Outgoing };
    enum Roles {
        UsernameRole = Qt::UserRole + 1, //!< QString: DPNS username (may be empty)
        DisplayNameRole,                 //!< QString: profile display name
        IdentityRole,                    //!< QString: identity id, hex
        KindRole,                        //!< int: static_cast<int>(Kind)
    };

    struct Row {
        QString identity_hex;
        QString username;
        QString display_name;
        Kind kind{Kind::Established};
    };

    explicit ContactsModel(PlatformService& service, QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

    const Row* rowAt(int r) const { return (r >= 0 && r < m_rows.size()) ? &m_rows[r] : nullptr; }

public Q_SLOTS:
    void refresh();

private Q_SLOTS:
    void onIncoming(const QVector<QPair<QString, QString>>& contacts);
    void onOutgoing(const QVector<QPair<QString, QString>>& contacts);

private:
    void rebuild();

    PlatformService& m_service;
    QVector<Row> m_rows;
    QVector<QPair<QString, QString>> m_incoming; // (identity hex, username)
    QVector<QPair<QString, QString>> m_outgoing;
};

#endif // BITCOIN_QT_PLATFORM_CONTACTSMODEL_H
