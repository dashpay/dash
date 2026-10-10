// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_CONTACTSMODEL_H
#define BITCOIN_QT_PLATFORM_CONTACTSMODEL_H

#include <QAbstractTableModel>
#include <QPair>
#include <QString>
#include <QVector>

class PlatformService;

/** Table model backing the contacts list: established contacts plus incoming
 *  and outgoing contact requests, refreshed from the DashPay contactRequest
 *  documents via the PlatformService. Contacts the user ignored are left
 *  out unless setShowIgnored() asks for them. */
class ContactsModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum Column {
        Username = 0,
        DisplayName,
        Direction,
        COLUMN_COUNT
    };
    enum class Kind {
        Established,
        Incoming,
        //! They answered our request; importing their keychain needs the
        //! wallet unlocked, and may have failed.
        Accepted,
        Outgoing
    };
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
        //! The user ignored this request or hid this contact.
        bool hidden{false};
    };

    explicit ContactsModel(PlatformService& service, QObject* parent = nullptr);

    int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    int columnCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QVariant headerData(int section, Qt::Orientation orientation, int role) const override;

    const Row* rowAt(int r) const { return (r >= 0 && r < m_rows.size()) ? &m_rows[r] : nullptr; }
    //! The first refresh has answered (with contacts or with an error).
    bool loaded() const { return m_loaded; }
    //! Contacts the user ignored, shown or not.
    int hiddenCount() const { return m_hidden_count; }
    //! Whether any row shows a profile display name.
    bool hasDisplayNames() const;
    void setShowIgnored(bool show);
    //! Re-read themed colours after a theme change.
    void themeChanged();
    //! The wallet must be unlocked before an accepted contact can finish.
    bool walletLocked() const;

public Q_SLOTS:
    void refresh();

private:
    void rebuild();

    PlatformService& m_service;
    QVector<Row> m_rows;
    QVector<QPair<QString, QString>> m_incoming; // (identity hex, username)
    QVector<QPair<QString, QString>> m_outgoing;
    bool m_loaded{false};
    bool m_show_ignored{false};
    int m_hidden_count{0};
};

#endif // BITCOIN_QT_PLATFORM_CONTACTSMODEL_H
