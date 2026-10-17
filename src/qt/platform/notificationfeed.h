// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_NOTIFICATIONFEED_H
#define BITCOIN_QT_PLATFORM_NOTIFICATIONFEED_H

#include <consensus/amount.h>
#include <platform/types.h>
#include <script/script.h>
#include <wallet/platformtypes.h>

#include <QObject>
#include <QString>

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

class ContactFlow;
class IdentityFlow;
class PlatformService;

/**
 * What is new among the wallet's DashPay contacts, as the mobile wallets' notifications list it: a request waiting
 * for us, a request of ours that was answered, a contact we accepted, and a contact's payment to us.
 *
 * Requests and answers are read from the contacts the contact flow holds and alerted once. The wallet keeps the time
 * of the newest one announced, and a wallet without it announces nothing it already has. A request waits for its
 * sender's username and holds back the later ones.
 *
 * A payment is attributed to the contact whose receiving chain holds the address it pays. Chains can be derived only
 * while the wallet is unlocked, so they are taken when a request imports one and when the contacts are read under
 * the unlock that found the identity, and kept while the identity is the same. Payments are listed for this session
 * only.
 */
class NotificationFeed : public QObject
{
    Q_OBJECT

public:
    enum class Kind {
        REQUEST,  //!< they sent us a request, which waits for us
        ACCEPTED, //!< they answered a request of ours
        ADDED,    //!< we answered theirs
        PAYMENT,  //!< they paid us
    };
    struct Entry {
        Kind kind{Kind::REQUEST};
        platform::Identifier contact{};
        //! Their username, once read; nullopt when unread or they have none.
        std::optional<std::string> username;
        //! Platform's time of the request, for a payment the time it was seen.
        uint64_t time_ms{0};
        //! What a payment paid.
        CAmount amount{0};
        //! Whether their username was looked up.
        bool named{false};
    };

    //! The addresses of a receiving chain a payment is looked for at past the last one a payment was found at.
    static constexpr uint32_t CHAIN_RANGE{100};
    //! More alerts than this at once are one.
    static constexpr size_t MAX_ALERTS{10};

    NotificationFeed(PlatformService& service, IdentityFlow& identity_flow, ContactFlow& contact_flow);

    //! Newest first.
    std::vector<Entry> entries() const;
    //! What the entry says, as the alert and the list word it.
    QString text(const Entry& entry) const;

    //! The wallet received `amount` at `address`: a payment of a contact is
    //! listed and alerted. Nothing else is.
    void received(const QString& address, CAmount amount);

Q_SIGNALS:
    //! A payment was added; the requests and answers change with the contacts.
    void changed();
    //! Something new for the tray.
    void alert(const QString& text);

private:
    //! The receiving chain of a contact, and the addresses of it a payment is looked for at.
    struct Chain {
        wallet::FriendshipXpub xpub;
        std::map<CScript, uint32_t> scripts; //!< by index
        uint32_t next{0};                    //!< the first index not derived yet
    };

    void update();
    //! The receiving chain of every contact that can pay us, where the wallet can derive it.
    void learnChains(const platform::Identifier& ours);
    //! Alert what arrived since the wallet's time of the last announcement.
    void announce(const platform::Identifier& ours);
    //! Derive the addresses of `chain` up to index `end`.
    void extend(Chain& chain, uint32_t end);
    //! Derive the first addresses of `chain`, past those `paid` by the wallet's history.
    void open(Chain& chain, const std::set<CScript>& paid);

    PlatformService& m_service;
    IdentityFlow& m_identity_flow;
    ContactFlow& m_contact_flow;
    //! The identity everything below is of.
    std::optional<platform::Identifier> m_identity;
    //! The time of the newest announcement, read from the wallet once the identity is found.
    std::optional<uint64_t> m_announced;
    std::map<platform::Identifier, Chain> m_chains;
    std::vector<Entry> m_payments;
};

#endif // BITCOIN_QT_PLATFORM_NOTIFICATIONFEED_H
