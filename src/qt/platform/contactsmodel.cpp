// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/contactsmodel.h>

#include <platform/walletrecords.h>
#include <qt/guiutil.h>
#include <qt/platform/platformservice.h>
#include <qt/platform/platformui.h>
#include <qt/walletmodel.h>

#include <QColor>
#include <QSet>

#include <algorithm>

namespace {
//! Characters of a Base58 identity id shown for a contact without a username.
constexpr int SHORT_ID_LENGTH{8};
} // namespace

ContactsModel::ContactsModel(PlatformService& service, QObject* parent) :
    QAbstractTableModel(parent),
    m_service(service)
{
    connect(&m_service, &PlatformService::contactsUpdated, this,
            [this](const QVector<QPair<QString, QString>>& incoming, const QVector<QPair<QString, QString>>& outgoing) {
                m_incoming = incoming;
                m_outgoing = outgoing;
                m_loaded = true;
                rebuild();
            });
    connect(&m_service, &PlatformService::contactsRefreshFailed, this, [this] {
        if (m_loaded) return;
        m_loaded = true;
        rebuild();
    });
    connect(&m_service, &PlatformService::hiddenContactsChanged, this, &ContactsModel::rebuild);
    // An accepted contact reads "unlock to finish" only while locked.
    connect(&m_service.walletModel(), &WalletModel::encryptionStatusChanged, this, [this] {
        if (m_rows.isEmpty()) return;
        Q_EMIT dataChanged(index(0, 0), index(m_rows.size() - 1, COLUMN_COUNT - 1), {Qt::DisplayRole, Qt::ToolTipRole});
    });
}

bool ContactsModel::walletLocked() const
{
    if (m_service.stopped()) return false;
    const auto status{m_service.walletModel().getEncryptionStatus()};
    return status == WalletModel::Locked || status == WalletModel::UnlockedForMixingOnly;
}

int ContactsModel::rowCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : m_rows.size(); }

int ContactsModel::columnCount(const QModelIndex& parent) const { return parent.isValid() ? 0 : COLUMN_COUNT; }

QVariant ContactsModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() >= m_rows.size()) return {};
    const Row& r = m_rows[index.row()];
    const QString name{r.username.isEmpty() ? tr("this person") : r.username};
    const QString sentence_name{r.username.isEmpty() ? tr("This person") : r.username};
    const QString accepted_error{r.kind == Kind::Accepted ? m_service.acceptedError(r.identity_hex) : QString{}};
    // Our request (or reply) to them was sent and Platform is confirming it.
    const bool confirming{r.identity_hex == m_service.pendingContactRequest()};
    switch (role) {
    case UsernameRole:
        return r.username;
    case DisplayNameRole:
        return r.display_name;
    case IdentityRole:
        return r.identity_hex;
    case KindRole:
        return static_cast<int>(r.kind);
    case Qt::ToolTipRole:
        if (r.hidden) return tr("Hidden on this computer. Use “Show again” to bring it back.");
        switch (r.kind) {
        case Kind::Established:
            return tr("You're connected. To pay %1, open Send and type their username or press @.").arg(name);
        case Kind::Incoming:
            return tr("%1 sent you a contact request. Accept it to pay each other by username.").arg(sentence_name);
        case Kind::Accepted:
            if (confirming) {
                return tr("Your reply to %1 was sent. Dash Platform is confirming it, which can take a few minutes.").arg(name);
            }
            if (walletLocked()) {
                return tr("%1 accepted your request. Unlock the wallet to finish connecting; nothing more is sent.")
                    .arg(sentence_name);
            }
            if (!accepted_error.isEmpty()) {
                return tr("%1 accepted your request, but Dash Core could not finish connecting. %2")
                    .arg(sentence_name, accepted_error);
            }
            return tr("%1 accepted your request. Dash Core is finishing connecting; nothing more is sent.").arg(sentence_name);
        case Kind::Outgoing:
            if (confirming) {
                return tr("Your request was sent. Dash Platform is confirming it, which can take a few minutes.");
            }
            return tr("Waiting for %1 to accept your request.").arg(name);
        }
        return {};
    case Qt::ForegroundRole:
        if (index.column() == Username && r.username.isEmpty()) {
            return QColor{GUIUtil::getThemedQColor(GUIUtil::ThemedColor::UNCONFIRMED)};
        }
        if (index.column() == Direction) {
            if (r.hidden) return QColor{GUIUtil::getThemedQColor(GUIUtil::ThemedColor::UNCONFIRMED)};
            switch (r.kind) {
            case Kind::Established:
                return QColor{GUIUtil::getThemedQColor(GUIUtil::ThemedColor::GREEN)};
            case Kind::Incoming:
            case Kind::Accepted:
                return QColor{GUIUtil::getThemedQColor(GUIUtil::ThemedColor::ORANGE)};
            case Kind::Outgoing:
                return QColor{GUIUtil::getThemedQColor(GUIUtil::ThemedColor::UNCONFIRMED)};
            }
        }
        return {};
    case Qt::TextAlignmentRole:
        return int{Qt::AlignLeft | Qt::AlignVCenter};
    case Qt::DisplayRole:
        switch (index.column()) {
        case Username:
            if (!r.username.isEmpty()) return r.username;
            return tr("Unknown user %1…").arg(PlatformUi::identityIdBase58(r.identity_hex).left(SHORT_ID_LENGTH));
        case DisplayName:
            return r.display_name;
        case Direction:
            // Hiding a contact is not ignoring a request.
            if (r.hidden) return r.kind == Kind::Established ? tr("Hidden") : tr("Ignored");
            switch (r.kind) {
            case Kind::Established:
                return tr("Connected");
            case Kind::Incoming:
                return tr("Wants to connect");
            case Kind::Accepted:
                if (confirming) return tr("Accepted — confirming…");
                if (walletLocked()) return tr("Accepted — unlock to finish");
                return accepted_error.isEmpty() ? tr("Accepted — finishing…") : tr("Accepted — couldn't finish");
            case Kind::Outgoing:
                return confirming ? tr("Request sent — confirming…") : tr("Request sent");
            }
            return {};
        }
        return {};
    }
    return {};
}

QVariant ContactsModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal) return {};
    // Headers line up with their cells whatever the theme centers them to.
    if (role == Qt::TextAlignmentRole) return int{Qt::AlignLeft | Qt::AlignVCenter};
    if (role != Qt::DisplayRole) return {};
    switch (section) {
    case Username:
        return tr("Username");
    case DisplayName:
        return tr("Profile name");
    case Direction:
        return tr("Status");
    }
    return {};
}

bool ContactsModel::hasDisplayNames() const
{
    return std::any_of(m_rows.begin(), m_rows.end(), [](const Row& row) { return !row.display_name.isEmpty(); });
}

void ContactsModel::refresh() { m_service.refreshContacts(); }

void ContactsModel::setShowIgnored(bool show)
{
    if (m_show_ignored == show) return;
    m_show_ignored = show;
    rebuild();
}

void ContactsModel::themeChanged()
{
    if (m_rows.isEmpty()) return;
    Q_EMIT dataChanged(index(0, 0), index(m_rows.size() - 1, COLUMN_COUNT - 1), {Qt::ForegroundRole});
}

void ContactsModel::rebuild()
{
    beginResetModel();
    m_rows.clear();
    m_hidden_count = 0;
    const auto add = [this](const QString& id, const QString& username, Kind kind) {
        const bool hidden{m_service.isHidden(id)};
        if (hidden) ++m_hidden_count;
        if (hidden && !m_show_ignored) return;
        m_rows.append({id, username, m_service.contactMetadata(id, platform::records::CONTACT_DISPLAY_NAME_PREFIX),
                       kind, hidden});
    };
    QSet<QString> outgoing_ids;
    for (const auto& out : m_outgoing) {
        outgoing_ids.insert(out.first);
    }
    // Requests appearing both incoming and outgoing are established
    // contacts once the incoming one was decrypted and its friendship xpub
    // imported into this wallet; until then the contact has accepted. One
    // row per person: the service lists one request per sender, and our own
    // re-sends (DIP-15) to one person are one row.
    QSet<QString> listed;
    for (const auto& in : m_incoming) {
        listed.insert(in.first);
        add(in.first, in.second,
            !outgoing_ids.contains(in.first)    ? Kind::Incoming
            : m_service.isEstablished(in.first) ? Kind::Established
                                                : Kind::Accepted);
    }
    for (const auto& out : m_outgoing) {
        if (listed.contains(out.first)) continue;
        listed.insert(out.first);
        add(out.first, out.second, Kind::Outgoing);
    }
    // Requests that need the user's attention first, then contacts, then
    // outgoing requests that are merely waiting on the other side, then
    // what the user chose to ignore.
    const auto priority = [](const Row& row) {
        if (row.hidden) return 3;
        switch (row.kind) {
        case Kind::Incoming:
        case Kind::Accepted:
            return 0;
        case Kind::Established:
            return 1;
        case Kind::Outgoing:
            return 2;
        }
        return 3;
    };
    std::stable_sort(m_rows.begin(), m_rows.end(), [&priority](const Row& a, const Row& b) {
        if (priority(a) != priority(b)) return priority(a) < priority(b);
        return a.username.localeAwareCompare(b.username) < 0;
    });
    endResetModel();
}
