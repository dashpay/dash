// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_QT_PLATFORM_CONTACTFLOW_H
#define BITCOIN_QT_PLATFORM_CONTACTFLOW_H

#include <platform/types.h>

#include <QObject>
#include <QString>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class IdentityFlow;
class PlatformService;

/**
 * The DashPay contacts of the wallet's identity, as the mobile wallets keep
 * them: read from the contact requests on Platform, newest per counterparty,
 * and held in memory only. A request each way is a contact; one from them is
 * waiting to be accepted, one from us waiting for them.
 *
 * Adding a contact (by exact username) and accepting one send our request
 * with one SDK call. The wallet derives the ECDH secret and the receiving
 * chain the request carries, and imports that chain first, so the contact's
 * payments are seen; no private key leaves it.
 *
 * Paying a contact hands the next address of the payment chain their request
 * carries to the Send tab, counted as used before it is shown.
 */
class ContactFlow : public QObject
{
    Q_OBJECT

public:
    //! The DIP-15 account of every receiving chain, as the mobile wallets send it.
    static constexpr uint32_t ACCOUNT{0};

    enum class Step {
        IDLE,
        RESOLVING, //!< resolving the username
        FETCHING,  //!< reading the contact's identity
        SENDING,   //!< from the passphrase to Platform's answer
        PAYING,    //!< reading the identity of the contact to pay
    };
    //! In the order contacts() lists them.
    enum class State {
        INCOMING, //!< they sent a request, we did not
        ESTABLISHED,
        OUTGOING, //!< we sent a request, they did not
    };
    struct Contact {
        platform::Identifier id{};
        State state{State::INCOMING};
        //! Their username, once read; nullopt when unread or they have none.
        std::optional<std::string> username;
        //! When our newest request to them was created; 0 without one.
        uint64_t sent_at_ms{0};
    };

    ContactFlow(PlatformService& service, IdentityFlow& identity_flow);

    Step step() const { return m_step; }
    //! Whether both directions were read since the identity was found.
    bool swept() const { return m_sent && m_received; }
    //! Whether contact requests are being read.
    bool reading() const { return !m_sent_call.expired() || !m_received_call.expired(); }
    //! Incoming first, then established, then outgoing.
    std::vector<Contact> contacts() const;
    //! Why the last action failed, else why the last read did; empty when
    //! neither did.
    const QString& error() const { return m_error.isEmpty() ? m_read_error : m_error; }

    //! Send a contact request to the identity a username resolves to.
    void add(const std::string& username);
    //! Accept the incoming request of a contact.
    void accept(const platform::Identifier& contact);
    //! Read the contact requests again.
    void refresh();
    //! Choose the address to pay an established contact at, and hand it to
    //! payTo().
    void pay(const platform::Identifier& contact);

Q_SIGNALS:
    void changed();
    //! Pay the contact at this address of their payment chain.
    void payTo(const QString& address);

private:
    using Requests = std::map<platform::Identifier, platform::ContactRequest>;

    void update();
    //! Read whichever direction is unread and not being read.
    void sweep();
    void readRequests(platform::ContactDirection direction);
    void readNames();
    void resolve();
    void onResolved(platform::Result<platform::Identifier> result);
    void fetch();
    void onFetched(platform::Result<platform::Identity> result);
    void onSent(const platform::Status& status);
    void prefill(const platform::Identity& theirs);
    void finish(const QString& error = {});

    PlatformService& m_service;
    IdentityFlow& m_identity_flow;
    Step m_step{Step::IDLE};
    //! The username being added, and the contact the request goes to or
    //! the payment is for.
    std::string m_label;
    platform::Identifier m_contact{};
    //! The step's call, until it calls back or is dropped.
    std::weak_ptr<void> m_call;
    //! Our newest request to each recipient, and theirs from each sender.
    std::optional<Requests> m_sent;
    std::optional<Requests> m_received;
    std::weak_ptr<void> m_sent_call;
    std::weak_ptr<void> m_received_call;
    //! Usernames read, by identity; nullopt: none.
    std::map<platform::Identifier, std::optional<std::string>> m_names;
    std::map<platform::Identifier, std::weak_ptr<void>> m_name_calls;
    QString m_error;
    QString m_read_error;
};

#endif // BITCOIN_QT_PLATFORM_CONTACTFLOW_H
