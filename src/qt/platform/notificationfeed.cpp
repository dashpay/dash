// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/notificationfeed.h>

#include <base58.h>
#include <crypto/common.h>
#include <interfaces/wallet.h>
#include <key_io.h>
#include <logging.h>
#include <qt/bitcoinunits.h>
#include <qt/optionsmodel.h>
#include <qt/platform/contactflow.h>
#include <qt/platform/identityflow.h>
#include <qt/platform/platformservice.h>
#include <qt/walletmodel.h>
#include <script/standard.h>
#include <tinyformat.h>
#include <uint256.h>
#include <util/strencodings.h>
#include <util/time.h>

#include <algorithm>
#include <chrono>

namespace {
using Contacts = std::vector<ContactFlow::Contact>;

uint64_t NowMs() { return TicksSinceEpoch<std::chrono::milliseconds>(NodeClock::now()); }

//! Where the wallet keeps the time of the newest request `ours` announced.
std::string AnnouncedRecord(const platform::Identifier& ours) { return strprintf("dashpay/notified/%s", HexStr(ours)); }

//! The requests and answers among the contacts, in no order.
std::vector<NotificationFeed::Entry> Events(const Contacts& contacts)
{
    using Kind = NotificationFeed::Kind;
    std::vector<NotificationFeed::Entry> events;
    for (const ContactFlow::Contact& contact : contacts) {
        if (contact.state == ContactFlow::State::OUTGOING) continue;
        NotificationFeed::Entry entry{Kind::REQUEST,          contact.id, contact.username,
                                      contact.received_at_ms, 0,          contact.name_read};
        if (contact.state == ContactFlow::State::ESTABLISHED) {
            // The newer request is the one that made the contact.
            const bool they_answered{contact.received_at_ms >= contact.sent_at_ms};
            entry.kind = they_answered ? Kind::ACCEPTED : Kind::ADDED;
            entry.time_ms = they_answered ? contact.received_at_ms : contact.sent_at_ms;
        }
        events.push_back(std::move(entry));
    }
    return events;
}

//! `entry` with its contact's username, as the contacts have it now.
NotificationFeed::Entry Named(NotificationFeed::Entry entry, const Contacts& contacts)
{
    const auto contact{std::find_if(contacts.begin(), contacts.end(),
                                    [&](const ContactFlow::Contact& c) { return c.id == entry.contact; })};
    if (contact != contacts.end()) {
        entry.username = contact->username;
        entry.named = contact->name_read;
    }
    return entry;
}
} // namespace

NotificationFeed::NotificationFeed(PlatformService& service, IdentityFlow& identity_flow, ContactFlow& contact_flow) :
    m_service(service),
    m_identity_flow(identity_flow),
    m_contact_flow(contact_flow)
{
    connect(&m_contact_flow, &ContactFlow::changed, this, &NotificationFeed::update);
    // The chain of a request just sent: the wallet is unlocked for it now, and no longer by the time it is read back.
    connect(&m_contact_flow, &ContactFlow::receivingChain, this,
            [this](const platform::Identifier& contact, const wallet::FriendshipXpub& chain) {
                m_chains.emplace(contact, Chain{chain, {}});
            });
}

std::vector<NotificationFeed::Entry> NotificationFeed::entries() const
{
    const auto contacts{m_contact_flow.contacts()};
    auto entries{Events(contacts)};
    for (const Entry& payment : m_payments) {
        entries.push_back(Named(payment, contacts));
    }
    std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) { return a.time_ms > b.time_ms; });
    return entries;
}

QString NotificationFeed::text(const Entry& entry) const
{
    const QString name{QString::fromStdString(entry.username.value_or(EncodeBase58(entry.contact)))};
    switch (entry.kind) {
    case Kind::REQUEST:
        return tr("%1 has sent you a contact request").arg(name);
    case Kind::ACCEPTED:
        return tr("%1 accepted your contact request").arg(name);
    case Kind::ADDED:
        return tr("You added %1 as a contact").arg(name);
    case Kind::PAYMENT:
        return tr("%1 paid you %2")
            .arg(name,
                 BitcoinUnits::formatWithUnit(m_service.walletModel().getOptionsModel()->getDisplayUnit(), entry.amount));
    }
    return {};
}

void NotificationFeed::update()
{
    // A closed gate forgets the identity, not what is known of it: the
    // wallet can't derive the chains again while it is locked.
    const auto& identity{m_identity_flow.identity()};
    if (!identity) return;
    if (m_identity != identity->id) {
        m_identity = identity->id;
        m_announced.reset();
        m_chains.clear();
        m_payments.clear();
    }
    if (m_contact_flow.swept()) {
        learnChains(identity->id);
        announce(identity->id);
    }
}

void NotificationFeed::learnChains(const platform::Identifier& ours)
{
    interfaces::Wallet& wallet{m_service.walletModel().wallet()};
    for (const ContactFlow::Contact& contact : m_contact_flow.contacts()) {
        // Every contact we sent a request to can pay us, answered or not.
        if (contact.state == ContactFlow::State::INCOMING || m_chains.count(contact.id)) continue;
        const auto chain{wallet.getFriendshipReceivingXpub({ContactFlow::ACCOUNT, uint256{ours}, uint256{contact.id}, 0})};
        if (chain) m_chains.emplace(contact.id, Chain{chain.value, {}});
    }
}

void NotificationFeed::announce(const platform::Identifier& ours)
{
    interfaces::Wallet& wallet{m_service.walletModel().wallet()};
    const std::string record{AnnouncedRecord(ours)};
    const auto write = [&](uint64_t time_ms) {
        m_announced = time_ms;
        std::vector<unsigned char> value(8);
        WriteLE64(value.data(), time_ms);
        // Not kept, the next restart announces them again.
        if (!wallet.writePlatformData(record, value)) {
            LogPrintf("DashPay: could not save the time of the last notification\n");
        }
    };

    // What the user did themselves is only listed.
    auto fresh{Events(m_contact_flow.contacts())};
    std::erase_if(fresh, [](const Entry& entry) { return entry.kind == Kind::ADDED; });
    std::stable_sort(fresh.begin(), fresh.end(), [](const Entry& a, const Entry& b) { return a.time_ms < b.time_ms; });
    if (!m_announced) {
        const auto stored{wallet.getPlatformData(record)};
        const auto it{stored.find(record)};
        if (it != stored.end() && it->second.size() == 8) {
            m_announced = ReadLE64(it->second.data());
        } else {
            // First time: what the contacts already hold is not news.
            write(fresh.empty() ? 0 : fresh.back().time_ms);
        }
    }
    std::erase_if(fresh, [&](const Entry& entry) { return entry.time_ms <= *m_announced; });

    // Requests made at the same time are announced together, or the later of them would be lost to the time just
    // saved; a name still being read holds back its time and every later one.
    const auto unread{std::find_if(fresh.begin(), fresh.end(), [](const Entry& entry) { return !entry.named; })};
    if (unread != fresh.end()) {
        std::erase_if(fresh, [held = unread->time_ms](const Entry& entry) { return entry.time_ms >= held; });
    }
    if (fresh.empty()) return;
    if (fresh.size() > MAX_ALERTS) {
        Q_EMIT alert(tr("You have %1 new DashPay notifications").arg(fresh.size()));
    } else {
        for (const Entry& entry : fresh) {
            Q_EMIT alert(text(entry));
        }
    }
    write(fresh.back().time_ms);
}

void NotificationFeed::extend(Chain& chain, uint32_t end)
{
    for (; chain.next < end; ++chain.next) {
        CTxDestination address;
        if (wallet::DeriveFriendshipPaymentDestination(chain.xpub, chain.next, address)) {
            chain.scripts.emplace(GetScriptForDestination(address), chain.next);
        }
    }
}

void NotificationFeed::open(Chain& chain, const std::set<CScript>& paid)
{
    // As far as the payer got in earlier sessions: CHAIN_RANGE addresses past the last one the wallet's history pays.
    uint32_t floor{0};
    while (chain.next - floor < CHAIN_RANGE) {
        CTxDestination address;
        if (wallet::DeriveFriendshipPaymentDestination(chain.xpub, chain.next, address)) {
            const CScript script{GetScriptForDestination(address)};
            chain.scripts.emplace(script, chain.next);
            if (paid.count(script)) floor = chain.next + 1;
        }
        ++chain.next;
    }
}

void NotificationFeed::received(const QString& address, CAmount amount)
{
    if (amount <= 0) return;
    const CTxDestination destination{DecodeDestination(address.toStdString())};
    if (!IsValidDestination(destination)) return;
    const CScript script{GetScriptForDestination(destination)};
    std::optional<std::set<CScript>> paid;
    for (auto& [contact, chain] : m_chains) {
        if (chain.next == 0) {
            if (!paid) {
                paid.emplace();
                for (const auto& wtx : m_service.walletModel().wallet().getWalletTxs()) {
                    for (const auto& out : wtx.tx->vout) {
                        paid->insert(out.scriptPubKey);
                    }
                }
            }
            open(chain, *paid);
        }
        const auto found{chain.scripts.find(script)};
        if (found == chain.scripts.end()) continue;
        // Slide the range past the match.
        extend(chain, found->second + 1 + CHAIN_RANGE);
        const Entry payment{Kind::PAYMENT, contact, std::nullopt, NowMs(), amount, false};
        m_payments.push_back(payment);
        Q_EMIT alert(text(Named(payment, m_contact_flow.contacts())));
        Q_EMIT changed();
        return;
    }
}
