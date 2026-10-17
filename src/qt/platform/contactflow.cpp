// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/contactflow.h>

#include <crypto/common.h>
#include <interfaces/wallet.h>
#include <key_io.h>
#include <logging.h>
#include <platform/client.h>
#include <pubkey.h>
#include <qt/platform/identityflow.h>
#include <qt/platform/platformservice.h>
#include <qt/platform/platformui.h>
#include <qt/walletmodel.h>
#include <script/standard.h>
#include <support/cleanse.h>
#include <tinyformat.h>
#include <uint256.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <wallet/platformtypes.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <set>
#include <tuple>
#include <utility>

namespace {
using platform::IdentityPublicKey;
using Purpose = IdentityPublicKey::Purpose;

const IdentityPublicKey* FindKey(const platform::Identity& identity, uint32_t id)
{
    const auto it{std::find_if(identity.public_keys.begin(), identity.public_keys.end(),
                               [id](const IdentityPublicKey& key) { return key.id == id; })};
    return it == identity.public_keys.end() ? nullptr : &*it;
}

bool Enabled(const IdentityPublicKey& key, Purpose purpose)
{
    return key.purpose == purpose && key.type == IdentityPublicKey::Type::ECDSA_SECP256K1 && key.disabled_at == 0;
}

std::optional<uint32_t> FirstEnabledKey(const platform::Identity& identity, Purpose purpose)
{
    for (const IdentityPublicKey& key : identity.public_keys) {
        if (Enabled(key, purpose)) return key.id;
    }
    return std::nullopt;
}

//! The accountReference over our receiving chain towards `theirs`, which is
//! imported first so their payments are seen. No payment can predate it, so
//! it is born now.
std::optional<uint32_t> ImportedAccountReference(interfaces::Wallet& wallet, uint32_t index,
                                                 const platform::Identifier& ours, const platform::Identifier& theirs,
                                                 uint32_t encryption_key)
{
    const auto chain{wallet.ensureFriendshipReceivingKeychain(
        {ContactFlow::ACCOUNT, uint256{ours}, uint256{theirs}, TicksSinceEpoch<std::chrono::seconds>(NodeClock::now())})};
    wallet::CompactXpub compact;
    if (!chain || !wallet::CompactXpubBytes(chain.value, compact)) return std::nullopt;
    const auto reference{wallet.platformAccountReference({index, encryption_key}, compact)};
    if (!reference) return std::nullopt;
    return reference.value;
}

//! The newest request per counterparty: the latest, then the higher
//! accountReference, as the mobile wallets choose it.
std::map<platform::Identifier, platform::ContactRequest> Newest(std::vector<platform::ContactRequest> requests,
                                                                platform::Identifier platform::ContactRequest::* key)
{
    const auto rank = [](const platform::ContactRequest& request) {
        return std::tie(request.created_at_ms, request.account_reference);
    };
    std::map<platform::Identifier, platform::ContactRequest> newest;
    for (auto& request : requests) {
        const auto [it, added]{newest.try_emplace(request.*key, std::move(request))};
        if (!added && rank(request) > rank(it->second)) it->second = std::move(request);
    }
    return newest;
}

QString NoSuchUser() { return ContactFlow::tr("No DashPay user has that username."); }

//! The payment chain an incoming request carries, decrypted with our key it
//! names, `ours` being the identity at `index`. nullopt unless their key and
//! ours are keys DIP-15 accepts for its ECDH and it decrypts to a valid
//! point. The wallet must be unlocked.
std::optional<wallet::FriendshipXpub> PeerXpub(interfaces::Wallet& wallet, uint32_t index, const platform::Identity& ours,
                                               const platform::Identity& theirs, const platform::ContactRequest& request)
{
    // The keys DIP-15 accepts on receive, as rs-platform-wallet validates them.
    const IdentityPublicKey* their_key{FindKey(theirs, request.sender_key_index)};
    if (!their_key || !(Enabled(*their_key, Purpose::ENCRYPTION) || Enabled(*their_key, Purpose::AUTHENTICATION))) {
        return std::nullopt;
    }
    const IdentityPublicKey* our_key{FindKey(ours, request.recipient_key_index)};
    if (!our_key || our_key->disabled_at != 0 ||
        (our_key->purpose != Purpose::DECRYPTION && our_key->purpose != Purpose::ENCRYPTION &&
         our_key->purpose != Purpose::AUTHENTICATION && our_key->purpose != Purpose::TRANSFER) ||
        (our_key->type != IdentityPublicKey::Type::ECDSA_SECP256K1 &&
         our_key->type != IdentityPublicKey::Type::ECDSA_HASH160)) {
        return std::nullopt;
    }
    const auto secret{wallet.platformECDHSecret({index, request.recipient_key_index}, CPubKey{their_key->data})};
    std::array<uint8_t, 32> shared;
    if (!secret || secret.value.size() != shared.size()) return std::nullopt;
    std::copy(secret.value.begin(), secret.value.end(), shared.begin());
    auto xpub{platform::DecryptContactXpub(shared, request.encrypted_public_key)};
    memory_cleanse(shared.data(), shared.size());
    if (!xpub || !xpub->pubkey.IsFullyValid() || !xpub->pubkey.IsCompressed()) return std::nullopt;
    return xpub;
}

//! A DIP-15 receiver watches this many addresses past the last one paid
//! (key-wallet's DashPay gap limit), so no payment goes further ahead.
constexpr uint32_t RECEIVER_GAP{10};

//! The address to pay `chain` at next, counted as used in the wallet first:
//! the next one after the last handed out, but never before the history
//! floor (one past the last one the wallet's transactions pay, those sent
//! from another wallet on this seed included once a rescan found them) nor
//! RECEIVER_GAP or more past it, where the lowest one not paid yet is used
//! again. nullopt if it could not be counted.
std::optional<CTxDestination> NextPaymentAddress(interfaces::Wallet& wallet, const platform::Identifier& ours,
                                                 const platform::Identifier& theirs, const wallet::FriendshipXpub& chain)
{
    std::set<CScript> paid;
    for (const auto& wtx : wallet.getWalletTxs()) {
        for (const auto& out : wtx.tx->vout) {
            paid.insert(out.scriptPubKey);
        }
    }
    std::vector<CTxDestination> addresses;
    uint32_t floor{0};
    std::optional<uint32_t> unpaid;
    for (uint32_t index{0}; index - floor < RECEIVER_GAP; ++index) {
        CTxDestination& address{addresses.emplace_back()};
        if (!wallet::DeriveFriendshipPaymentDestination(chain, index, address)) return std::nullopt;
        if (paid.count(GetScriptForDestination(address))) {
            floor = index + 1;
        } else if (!unpaid) {
            unpaid = index;
        }
    }
    // One cursor per chain: a newer request with another chain starts its own.
    const CKeyID chain_id{chain.pubkey.GetID()};
    const std::string record{
        strprintf("dashpay/pay/%s/%s/%s", HexStr(ours), HexStr(theirs), HexStr(Span{chain_id}.first(4)))};
    const auto stored{wallet.getPlatformData(record)};
    const auto it{stored.find(record)};
    const uint32_t cursor{it == stored.end() || it->second.size() != 4 ? 0 : ReadLE32(it->second.data())};
    const uint32_t next{std::max(floor, cursor)};
    const uint32_t index{next - floor < RECEIVER_GAP ? next : *unpaid};
    std::vector<unsigned char> value(4);
    WriteLE32(value.data(), std::max(cursor, index + 1));
    if (!wallet.writePlatformData(record, value)) return std::nullopt;
    return addresses[index];
}
} // namespace

ContactFlow::ContactFlow(PlatformService& service, IdentityFlow& identity_flow) :
    m_service(service),
    m_identity_flow(identity_flow)
{
    m_service.addWriter(*this);
    connect(&m_service, &PlatformService::changed, this, &ContactFlow::update);
    connect(&m_identity_flow, &IdentityFlow::changed, this, &ContactFlow::update);
    update();
}

std::vector<ContactFlow::Contact> ContactFlow::contacts() const
{
    std::vector<Contact> contacts;
    if (!swept()) return contacts;
    const auto add = [&](const platform::Identifier& id, State state) {
        const auto name{m_names.find(id)};
        const auto sent{m_sent->find(id)};
        contacts.push_back({id, state, name == m_names.end() ? std::nullopt : name->second,
                            sent == m_sent->end() ? 0 : sent->second.created_at_ms});
    };
    for (const auto& [id, request] : *m_received) {
        add(id, m_sent->count(id) ? State::ESTABLISHED : State::INCOMING);
    }
    for (const auto& [id, request] : *m_sent) {
        if (!m_received->count(id)) add(id, State::OUTGOING);
    }
    std::stable_sort(contacts.begin(), contacts.end(),
                     [](const Contact& a, const Contact& b) { return a.state < b.state; });
    return contacts;
}

void ContactFlow::update()
{
    // The identity is forgotten when a gate closes or another is selected;
    // its contacts go with it.
    if (!m_identity_flow.identity()) {
        m_sent.reset();
        m_received.reset();
        m_sent_call.reset();
        m_received_call.reset();
        m_names.clear();
        m_name_calls.clear();
    }
    // A call the client dropped never calls back.
    if (m_call.expired()) {
        if (m_step == Step::RESOLVING) {
            resolve();
        } else if (m_step == Step::FETCHING || m_step == Step::PAYING) {
            fetch();
        } else if (m_step == Step::SENDING) {
            // Only a closed gate drops a write, and the contacts went with
            // the identity: our requests, read again, say whether it landed.
            m_step = Step::IDLE;
        }
    }
    sweep();
    readNames();
    Q_EMIT changed();
}

void ContactFlow::finish(const QString& error)
{
    m_step = Step::IDLE;
    m_error = error;
    sweep();
    Q_EMIT changed();
}

void ContactFlow::refresh()
{
    if (m_step != Step::IDLE || reading()) return;
    m_sent.reset();
    m_received.reset();
    std::erase_if(m_names, [](const auto& name) { return !name.second; });
    m_error.clear();
    m_read_error.clear();
    sweep();
    Q_EMIT changed();
}

void ContactFlow::sweep()
{
    readRequests(platform::ContactDirection::SENT);
    readRequests(platform::ContactDirection::RECEIVED);
}

void ContactFlow::readRequests(platform::ContactDirection direction)
{
    const bool sent{direction == platform::ContactDirection::SENT};
    const platform::Identity* identity{m_identity_flow.identity()};
    platform::PlatformClient* client{m_service.client()};
    auto& requests{sent ? m_sent : m_received};
    auto& call{sent ? m_sent_call : m_received_call};
    if (!identity || !client || requests || !call.expired()) return;
    client->contactRequests(identity->id, direction, /*since_ms=*/0,
                            PlatformUi::Tracked<std::vector<platform::ContactRequest>>(call, [this, sent, &requests, &call,
                                                                                              id = identity->id](auto result) {
                                if (!m_identity_flow.isSelected(id)) return;
                                // Answered: no longer reading().
                                call.reset();
                                if (result.ok()) {
                                    // Ours by recipient, theirs by sender.
                                    requests = Newest(std::move(result.value), sent ? &platform::ContactRequest::to_user_id
                                                                                    : &platform::ContactRequest::owner);
                                    if (swept()) m_read_error.clear();
                                } else {
                                    m_read_error = PlatformUi::Describe(result.status);
                                }
                                readNames();
                                Q_EMIT changed();
                            }));
}

void ContactFlow::readNames()
{
    platform::PlatformClient* client{m_service.client()};
    if (!client) return;
    for (const Contact& contact : contacts()) {
        auto& call{m_name_calls[contact.id]};
        if (m_names.count(contact.id) || !call.expired()) continue;
        client->namesOfIdentity(contact.id, /*page_limit=*/1, /*max_pages=*/1,
                                PlatformUi::Tracked<platform::Names>(call, [this, id = contact.id](auto result) {
                                    auto& name{m_names[id]};
                                    if (result.ok() && !result.value.names.empty()) {
                                        name = result.value.names.front().label;
                                    }
                                    Q_EMIT changed();
                                }));
    }
}

void ContactFlow::add(const std::string& username)
{
    if (m_step != Step::IDLE || !swept() || !m_service.client()) return;
    m_label = username;
    if (!platform::IsValidUsername(m_label)) return finish(NoSuchUser());
    m_step = Step::RESOLVING;
    m_error.clear();
    resolve();
    Q_EMIT changed();
}

void ContactFlow::resolve()
{
    platform::PlatformClient* client{m_service.client()};
    if (!client) return finish();
    client->resolveName(m_label, PlatformUi::Tracked<platform::Identifier>(m_call, [this](auto result) {
                            onResolved(std::move(result));
                        }));
}

void ContactFlow::onResolved(platform::Result<platform::Identifier> result)
{
    const QString label{QString::fromStdString(m_label)};
    if (result.status.kind == platform::StatusKind::NOT_FOUND) return finish(NoSuchUser());
    if (!result.ok()) return finish(PlatformUi::Describe(result.status));
    if (m_identity_flow.isSelected(result.value)) return finish(tr("“%1” is your own username.").arg(label));
    if (m_sent && m_sent->count(result.value)) {
        return finish(tr("You have already sent “%1” a contact request.").arg(label));
    }
    m_contact = result.value;
    m_step = Step::FETCHING;
    fetch();
    Q_EMIT changed();
}

void ContactFlow::accept(const platform::Identifier& contact)
{
    if (m_step != Step::IDLE || !swept() || !m_received->count(contact) || m_sent->count(contact)) return;
    m_contact = contact;
    m_step = Step::FETCHING;
    m_error.clear();
    fetch();
    Q_EMIT changed();
}

void ContactFlow::fetch()
{
    platform::PlatformClient* client{m_service.client()};
    if (!client) return finish();
    client->getIdentity(m_contact, PlatformUi::Tracked<platform::Identity>(m_call, [this](auto result) {
                            onFetched(std::move(result));
                        }));
}

void ContactFlow::onFetched(platform::Result<platform::Identity> result)
{
    if (result.status.kind == platform::StatusKind::NOT_FOUND) {
        return finish(tr("This DashPay user no longer exists."));
    }
    if (!result.ok()) return finish(PlatformUi::Describe(result.status));
    if (!m_identity_flow.identity()) return finish();
    if (m_step == Step::PAYING) return prefill(result.value);
    const platform::Identity ours{*m_identity_flow.identity()};
    const platform::Identity& theirs{result.value};
    // Theirs: an enabled DECRYPTION key, else ENCRYPTION. Ours: ENCRYPTION.
    auto recipient_key{FirstEnabledKey(theirs, Purpose::DECRYPTION)};
    if (!recipient_key) recipient_key = FirstEnabledKey(theirs, Purpose::ENCRYPTION);
    if (!recipient_key) return finish(tr("This DashPay user can't receive contact requests."));
    const auto encryption_key{FirstEnabledKey(ours, Purpose::ENCRYPTION)};
    const auto signing_key{PlatformUi::DocumentSigningKey(ours)};
    if (!encryption_key || !signing_key) {
        return finish(tr("Your identity has no keys DashPay can send a contact request with."));
    }

    m_step = Step::SENDING;
    Q_EMIT changed();
    WalletModel::UnlockContext unlock{m_service.walletModel().requestUnlock()};
    if (!unlock.isValid()) return finish(tr("The wallet stayed locked, so no contact request was sent."));
    // The passphrase dialog may have let another flow start, or closed a gate.
    if (!m_service.othersIdle(*this)) {
        return finish(tr("Another DashPay request is on its way. Try again once it is done."));
    }
    platform::PlatformClient* client{m_service.client()};
    if (!client || !m_identity_flow.isSelected(ours.id) || !swept()) return finish();

    interfaces::Wallet& wallet{m_service.walletModel().wallet()};
    const uint32_t index{m_identity_flow.index()};
    // Answering their request: only one carrying a chain we can pay.
    if (const auto incoming{m_received->find(theirs.id)};
        incoming != m_received->end() && !PeerXpub(wallet, index, ours, theirs, incoming->second)) {
        return finish(tr("This person's contact request to you carries no valid payment key, so DashPay can't add "
                         "them."));
    }
    const auto reference{ImportedAccountReference(wallet, index, ours.id, theirs.id, *encryption_key)};
    if (!reference) return finish(tr("Dash Core could not derive the contact request's keys from this wallet."));

    platform::ContactRequestInput input;
    input.sender = ours.id;
    input.recipient = theirs.id;
    input.sender_key_index = *encryption_key;
    input.recipient_key_index = *recipient_key;
    input.signing_key_id = *signing_key;
    input.account_reference = *reference;
    client->sendContactRequest(input, {wallet, index, PlatformUi::KeepUnlocked(std::move(unlock))},
                               m_service.trackWrite<platform::Identifier>(PlatformUi::Tracked<platform::Identifier>(
                                   m_call, [this](auto result) { onSent(result.status); })));
}

void ContactFlow::onSent(const platform::Status& status)
{
    if (status.kind != platform::StatusKind::OK) LogPrintf("DashPay: contact request failed: %s\n", status.message);
    // Sent or not, our requests read again say.
    m_sent.reset();
    finish(PlatformUi::Describe(status));
}

void ContactFlow::pay(const platform::Identifier& contact)
{
    if (m_step != Step::IDLE || !swept() || !m_received->count(contact) || !m_sent->count(contact)) return;
    m_contact = contact;
    m_step = Step::PAYING;
    m_error.clear();
    fetch();
    Q_EMIT changed();
}

void ContactFlow::prefill(const platform::Identity& theirs)
{
    WalletModel::UnlockContext unlock{m_service.walletModel().requestUnlock()};
    if (!unlock.isValid()) return finish(tr("The wallet stayed locked, so no payment address was chosen."));
    if (!m_service.othersIdle(*this)) {
        return finish(tr("Another DashPay request is on its way. Try again once it is done."));
    }
    const platform::Identity* ours{m_identity_flow.identity()};
    if (!ours || !swept() || !m_received->count(theirs.id)) return finish();
    interfaces::Wallet& wallet{m_service.walletModel().wallet()};
    const auto chain{PeerXpub(wallet, m_identity_flow.index(), *ours, theirs, m_received->at(theirs.id))};
    if (!chain) {
        return finish(tr("This contact's request to you carries no valid payment key, so DashPay can't pay them."));
    }
    const auto address{NextPaymentAddress(wallet, ours->id, theirs.id, *chain)};
    if (!address) return finish(tr("Dash Core could not choose and record this contact's payment address."));
    finish();
    Q_EMIT payTo(QString::fromStdString(EncodeDestination(*address)));
}
