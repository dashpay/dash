// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/contactsmodel.h>

#include <qt/guiutil.h>
#include <qt/platform/platformservice.h>

#include <QColor>

#include <algorithm>

ContactsModel::ContactsModel(PlatformService& service, QObject* parent) :
    QAbstractTableModel(parent),
    m_service(service)
{
    connect(&m_service, &PlatformService::contactsUpdated, this,
            [this](const QVector<QPair<QString, QString>>& incoming,
                   const QVector<QPair<QString, QString>>& outgoing) {
                m_incoming = incoming;
                m_outgoing = outgoing;
                rebuild();
            });
}

int ContactsModel::rowCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : m_rows.size();
}

int ContactsModel::columnCount(const QModelIndex& parent) const
{
    return parent.isValid() ? 0 : COLUMN_COUNT;
}

QVariant ContactsModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_rows.size()) return {};
    const Row& r = m_rows[index.row()];
    switch (role) {
    case UsernameRole: return r.username;
    case DisplayNameRole: return r.display_name;
    case IdentityRole: return r.identity_hex;
    case KindRole: return static_cast<int>(r.kind);
    case Qt::ToolTipRole:
        switch (r.kind) {
        case Kind::Established:
            return tr("You are connected. You can send Dash directly to this contact's username.");
        case Kind::Incoming:
            return tr("This person wants to add you as a contact. Accept the request to be able to "
                      "pay each other by username.");
        case Kind::Outgoing:
            return tr("Waiting for this person to accept your contact request.");
        }
        return {};
    case Qt::ForegroundRole:
        if (index.column() == Direction) {
            switch (r.kind) {
            case Kind::Established: return QColor{GUIUtil::getThemedQColor(GUIUtil::ThemedColor::GREEN)};
            case Kind::Incoming: return QColor{GUIUtil::getThemedQColor(GUIUtil::ThemedColor::ORANGE)};
            case Kind::Outgoing: return {};
            }
        }
        return {};
    case Qt::DisplayRole:
        switch (index.column()) {
        case Username: return r.username.isEmpty() ? r.identity_hex.left(16) + "…" : r.username;
        case DisplayName: return r.display_name;
        case Direction:
            switch (r.kind) {
            case Kind::Established: return tr("Contact");
            case Kind::Incoming: return tr("Wants to connect with you");
            case Kind::Outgoing: return tr("Invitation sent — waiting");
            }
            return {};
        }
        return {};
    }
    return {};
}

QVariant ContactsModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole) return {};
    switch (section) {
    case Username: return tr("Username");
    case DisplayName: return tr("Display name");
    case Direction: return tr("Status");
    }
    return {};
}

void ContactsModel::refresh()
{
    m_service.refreshContacts();
}

void ContactsModel::onIncoming(const QVector<QPair<QString, QString>>& contacts) { m_incoming = contacts; rebuild(); }
void ContactsModel::onOutgoing(const QVector<QPair<QString, QString>>& contacts) { m_outgoing = contacts; rebuild(); }

void ContactsModel::rebuild()
{
    beginResetModel();
    m_rows.clear();
    // Requests appearing both incoming and outgoing are established contacts.
    for (const auto& in : m_incoming) {
        bool mutual = false;
        for (const auto& out : m_outgoing) {
            if (out.first == in.first) { mutual = true; break; }
        }
        // Mutual documents alone do not establish a payable contact. The
        // incoming request must also have been decrypted and its 65-byte
        // friendship xpub imported into this wallet.
        const bool have_friendship_key{
            m_service.readRecord("contact/key/" + in.first.toStdString()).size() == 65};
        m_rows.append({in.first, in.second,
                       m_service.contactMetadata(in.first, "display-name"),
                       mutual && have_friendship_key ? Kind::Established : Kind::Incoming});
    }
    for (const auto& out : m_outgoing) {
        bool seen = false;
        for (const auto& r : m_rows) {
            if (r.identity_hex == out.first) { seen = true; break; }
        }
        if (!seen) m_rows.append({out.first, out.second,
                                  m_service.contactMetadata(out.first, "display-name"),
                                  Kind::Outgoing});
    }
    // Requests that need the user's attention first, then contacts, then
    // outgoing requests that are merely waiting on the other side.
    const auto priority = [](Kind kind) {
        switch (kind) {
        case Kind::Incoming: return 0;
        case Kind::Established: return 1;
        case Kind::Outgoing: return 2;
        }
        return 3;
    };
    std::stable_sort(m_rows.begin(), m_rows.end(), [&priority](const Row& a, const Row& b) {
        if (priority(a.kind) != priority(b.kind)) return priority(a.kind) < priority(b.kind);
        return a.username.localeAwareCompare(b.username) < 0;
    });
    endResetModel();
}
