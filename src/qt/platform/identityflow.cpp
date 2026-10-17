// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/identityflow.h>

#include <crypto/common.h>
#include <interfaces/node.h>
#include <interfaces/wallet.h>
#include <logging.h>
#include <qt/clientmodel.h>
#include <qt/platform/platformservice.h>
#include <qt/platform/platformui.h>
#include <streams.h>
#include <tinyformat.h>
#include <util/strencodings.h>
#include <util/translation.h>
#include <wallet/coincontrol.h>

#include <algorithm>
#include <cstring>
#include <ios>
#include <utility>
#include <vector>

namespace {
using platform::IdentityPublicKey;
using Funding = IdentityFlow::Funding;

//! The asset lock's one credit output.
constexpr uint32_t CREDIT_OUTPUT{0};
//! rs-dpp IdentityAssetLockTransactionOutPointAlreadyConsumedError.
constexpr uint32_t CODE_ASSET_LOCK_CONSUMED{10504};
//! The funding key index of the next top-up.
constexpr const char* NEXT_TOP_UP_RECORD{"dashpay/topup-index"};
//! The id of the identity selected last, as the mobile wallets keep their
//! main identity.
constexpr const char* MAIN_IDENTITY_RECORD{"dashpay/main-identity"};

//! The keys an identity registers, by id as the mobile wallets register
//! them: the SDK's four base keys, then the ENCRYPTION and DECRYPTION pair of
//! contact requests.
struct KeySpec {
    uint32_t id;
    IdentityPublicKey::Purpose purpose;
    IdentityPublicKey::SecurityLevel security_level;
};
constexpr KeySpec REGISTRATION_KEYS[]{
    {0, IdentityPublicKey::Purpose::AUTHENTICATION, IdentityPublicKey::SecurityLevel::MASTER},
    {1, IdentityPublicKey::Purpose::AUTHENTICATION, IdentityPublicKey::SecurityLevel::CRITICAL},
    {2, IdentityPublicKey::Purpose::AUTHENTICATION, IdentityPublicKey::SecurityLevel::HIGH},
    {3, IdentityPublicKey::Purpose::TRANSFER, IdentityPublicKey::SecurityLevel::CRITICAL},
    {4, IdentityPublicKey::Purpose::ENCRYPTION, IdentityPublicKey::SecurityLevel::MEDIUM},
    {5, IdentityPublicKey::Purpose::DECRYPTION, IdentityPublicKey::SecurityLevel::MEDIUM},
};

std::string RegistrationRecord(uint32_t index)
{
    return strprintf("%sidentity/%u", IdentityFlow::FUNDING_PREFIX, index);
}
std::string TopUpRecord(uint32_t index) { return strprintf("%stopup/%u", IdentityFlow::FUNDING_PREFIX, index); }

std::vector<unsigned char> Encode(const Funding& funding)
{
    DataStream stream;
    stream << funding.txid << funding.amount;
    if (funding.top_up) stream << funding.identity;
    return {UCharCast(stream.data()), UCharCast(stream.data() + stream.size())};
}

std::optional<Funding> Decode(const std::string& record, const std::vector<unsigned char>& value)
{
    Funding funding;
    funding.record = record;
    const std::string name{record.substr(std::strlen(IdentityFlow::FUNDING_PREFIX))};
    const bool top_up{name.rfind("topup/", 0) == 0};
    if (!top_up && name.rfind("identity/", 0) != 0) return std::nullopt;
    const auto index{ToIntegral<uint32_t>(name.substr(name.find('/') + 1))};
    if (!index) return std::nullopt;
    if (top_up) {
        funding.top_up = wallet::TopupFundingKey{*index};
    } else {
        funding.index = *index;
    }
    try {
        DataStream stream{MakeUCharSpan(value)};
        stream >> funding.txid >> funding.amount;
        if (top_up) stream >> funding.identity;
    } catch (const std::ios_base::failure&) {
        return std::nullopt;
    }
    return funding;
}

uint32_t NextTopUpIndex(interfaces::Wallet& wallet)
{
    const auto records{wallet.getPlatformData(NEXT_TOP_UP_RECORD)};
    const auto it{records.find(NEXT_TOP_UP_RECORD)};
    return it == records.end() || it->second.size() != 4 ? 0 : ReadLE32(it->second.data());
}

bool SaveFunding(interfaces::Wallet& wallet, const Funding& funding)
{
    if (funding.top_up) {
        std::vector<unsigned char> next(4);
        WriteLE32(next.data(), funding.top_up->funding_index + 1);
        if (!wallet.writePlatformData(NEXT_TOP_UP_RECORD, next)) return false;
    }
    return wallet.writePlatformData(funding.record, Encode(funding));
}

std::pair<CTransactionRef, interfaces::WalletTxStatus> WalletTxOf(interfaces::Wallet& wallet, const uint256& txid)
{
    interfaces::WalletTxStatus status{};
    interfaces::WalletOrderForm order_form;
    bool in_mempool{false};
    int num_blocks{0};
    const CTransactionRef tx{wallet.getWalletTxDetails(txid, status, order_form, in_mempool, num_blocks).tx};
    return {tx, status};
}

std::optional<std::array<uint8_t, 20>> MasterKeyHash(interfaces::Wallet& wallet, uint32_t index)
{
    const auto key{wallet.getPlatformPubKey(wallet::IdentityAuthKey{index, REGISTRATION_KEYS[0].id})};
    if (!key) return std::nullopt;
    const CKeyID hash{key.value.GetID()};
    std::array<uint8_t, 20> out;
    std::copy(hash.begin(), hash.end(), out.begin());
    return out;
}

std::optional<std::vector<IdentityPublicKey>> RegistrationKeys(interfaces::Wallet& wallet, uint32_t index)
{
    std::vector<IdentityPublicKey> keys;
    for (const KeySpec& spec : REGISTRATION_KEYS) {
        const auto pubkey{wallet.getPlatformPubKey(wallet::IdentityAuthKey{index, spec.id})};
        if (!pubkey) return std::nullopt;
        IdentityPublicKey key;
        key.id = spec.id;
        key.purpose = spec.purpose;
        key.security_level = spec.security_level;
        key.data.assign(pubkey.value.begin(), pubkey.value.end());
        keys.push_back(std::move(key));
    }
    return keys;
}

std::optional<platform::AssetLockProof> NodeProof(interfaces::Node& node, interfaces::Wallet& wallet,
                                                  const CTransactionRef& tx)
{
    auto islock{node.llmq().getInstantSendLock(tx->GetHash())};
    if (!islock.empty()) return platform::InstantAssetLockProof{tx, std::move(islock), CREDIT_OUTPUT};
    // The node drops the InstantSend lock once the block is ChainLocked.
    const auto status{WalletTxOf(wallet, tx->GetHash()).second};
    if (!status.is_chainlocked) return std::nullopt;
    return platform::ChainAssetLockProof{static_cast<uint32_t>(status.block_height),
                                         COutPoint{tx->GetHash(), CREDIT_OUTPUT}};
}

std::optional<platform::Identifier> MainIdentity(interfaces::Wallet& wallet)
{
    const auto records{wallet.getPlatformData(MAIN_IDENTITY_RECORD)};
    const auto it{records.find(MAIN_IDENTITY_RECORD)};
    platform::Identifier id;
    if (it == records.end() || it->second.size() != id.size()) return std::nullopt;
    std::copy(it->second.begin(), it->second.end(), id.begin());
    return id;
}

} // namespace

IdentityFlow::IdentityFlow(PlatformService& service, Confirm confirm, Proof proof) :
    m_service(service),
    m_confirm(std::move(confirm)),
    m_proof(std::move(proof))
{
    if (!m_proof) {
        m_proof = [&service](const CTransactionRef& tx) {
            return NodeProof(service.clientModel().node(), service.walletModel().wallet(), tx);
        };
    }
    m_service.addWriter(*this);
    connect(&m_service, &PlatformService::changed, this, &IdentityFlow::serviceChanged);
    connect(&m_service.clientModel(), &ClientModel::instantSendChanged, this, &IdentityFlow::send);
    connect(&m_service.clientModel(), &ClientModel::chainLockChanged, this, &IdentityFlow::send);
    serviceChanged();
}

std::optional<Funding> IdentityFlow::funding() const
{
    for (const auto& [record, value] : m_service.walletModel().wallet().getPlatformData(FUNDING_PREFIX)) {
        if (auto funding{Decode(record, value)}) return funding;
    }
    return std::nullopt;
}

void IdentityFlow::serviceChanged()
{
    // A call the client dropped never calls back: a lookup is made again,
    // and a funding record stays for Continue.
    if ((m_step == Step::LOOKING_UP || m_step == Step::SENDING) && m_call.expired()) {
        if (m_step == Step::LOOKING_UP) m_looked_up = false;
        finish();
    }
    if (m_service.gate() != PlatformService::Gate::NONE) {
        m_looked_up = false;
        m_index.reset();
        m_identities.clear();
        if (m_step == Step::WAITING_FOR_LOCK) finish();
    }
    if (m_step == Step::WAITING_FOR_LOCK) {
        send();
    } else if (!m_looked_up) {
        startLookUp(/*unlock=*/false);
    }
    Q_EMIT changed();
}

void IdentityFlow::finish(const QString& error)
{
    m_step = Step::IDLE;
    m_funding.reset();
    m_tx.reset();
    m_unlock.reset();
    m_error = error;
    Q_EMIT changed();
}

void IdentityFlow::lookUp() { startLookUp(/*unlock=*/true); }

std::optional<std::array<uint8_t, 20>> IdentityFlow::keyHash(uint32_t index)
{
    while (m_key_hashes.size() <= index) {
        const auto hash{MasterKeyHash(m_service.walletModel().wallet(), m_key_hashes.size())};
        if (!hash) return std::nullopt;
        m_key_hashes.push_back(*hash);
    }
    return m_key_hashes[index];
}

void IdentityFlow::startLookUp(bool unlock)
{
    if (m_step != Step::IDLE || !m_service.client()) return;
    std::optional<WalletModel::UnlockContext> context;
    if (!keyHash(0) && unlock) context.emplace(m_service.walletModel().requestUnlock());
    if (!keyHash(0)) {
        if (unlock) finish(tr("The wallet stayed locked, so DashPay could not look up your identity."));
        return;
    }
    // The passphrase dialog runs an event loop, which may have started a call.
    if (m_step != Step::IDLE || !m_service.client()) return;
    // Kept while the scan derives the keys of the indexes it reaches.
    if (context) m_unlock.emplace(std::move(*context));
    m_step = Step::LOOKING_UP;
    m_error.clear();
    probe(/*index=*/0, /*misses=*/0);
    Q_EMIT changed();
}

void IdentityFlow::probe(uint32_t index, uint32_t misses)
{
    platform::PlatformClient* client{m_service.client()};
    // Paused for a rebuild: changed() looks the identities up again.
    if (!client) return;
    client->getIdentityByPublicKeyHash(m_key_hashes.at(index),
                                       PlatformUi::Tracked<platform::Identity>(m_call, [this, index, misses](auto result) {
                                           onProbed(index, misses, std::move(result));
                                       }));
}

void IdentityFlow::onProbed(uint32_t index, uint32_t misses, platform::Result<platform::Identity> result)
{
    // An index nobody answered for may hold an identity.
    if (!result.ok() && result.status.kind != platform::StatusKind::NOT_FOUND) {
        return finish(PlatformUi::Describe(result.status));
    }
    if (result.ok()) {
        misses = 0;
        const auto main{MainIdentity(m_service.walletModel().wallet())};
        const platform::Identity& found{m_identities[index] = std::move(result.value)};
        if (!m_index && (!main || *main == found.id)) m_index = index;
        // The record is erased once the identity it registers is found.
        if (const auto pending{funding()}; pending && !pending->top_up && pending->index == index) {
            m_service.walletModel().wallet().writePlatformData(pending->record, {});
        }
    } else {
        ++misses;
    }
    if (misses < GAP_LIMIT) {
        // A locked wallet can't derive the next index's key: Find my identity
        // looks them up unlocked.
        if (!keyHash(index + 1)) return finish();
        probe(index + 1, misses);
        return Q_EMIT changed();
    }
    // The main identity was not found: the lowest index is, as the mobile wallets choose.
    if (!m_index && !m_identities.empty()) m_index = m_identities.begin()->first;
    m_looked_up = true;
    finish();
}

void IdentityFlow::select(uint32_t index)
{
    platform::PlatformClient* client{m_service.client()};
    const auto it{m_identities.find(index)};
    // Every write signs with the selected identity's keys: none is in flight.
    if (m_step != Step::IDLE || !client || it == m_identities.end() || m_index == index || !m_service.othersIdle(*this)) {
        return;
    }
    const platform::Identifier& id{it->second.id};
    m_service.walletModel().wallet().writePlatformData(MAIN_IDENTITY_RECORD,
                                                       std::vector<unsigned char>(id.begin(), id.end()));
    m_index.reset();
    m_step = Step::LOOKING_UP;
    m_error.clear();
    client->getIdentity(id, PlatformUi::Tracked<platform::Identity>(m_call, [this, index](auto result) {
                            onSelected(index, std::move(result));
                        }));
    Q_EMIT changed();
}

void IdentityFlow::onSelected(uint32_t index, platform::Result<platform::Identity> result)
{
    if (!result.ok()) {
        // Looked up again: the main identity is the one chosen.
        m_looked_up = false;
        finish();
        return startLookUp(/*unlock=*/false);
    }
    m_identities[index] = std::move(result.value);
    m_index = index;
    finish();
}

uint32_t IdentityFlow::nextIndex() const { return m_identities.empty() ? 0 : m_identities.rbegin()->first + 1; }

void IdentityFlow::create()
{
    if (!m_looked_up) return;
    Funding pending;
    pending.index = nextIndex();
    pending.record = RegistrationRecord(pending.index);
    fund(wallet::RegistrationFundingKey{pending.index}, REGISTRATION_AMOUNT, std::move(pending));
}

void IdentityFlow::topUp(CAmount amount)
{
    if (!identity()) return;
    const uint32_t index{NextTopUpIndex(m_service.walletModel().wallet())};
    Funding pending;
    pending.record = TopUpRecord(index);
    pending.top_up = wallet::TopupFundingKey{index};
    pending.identity = identity()->id;
    fund(*pending.top_up, amount, std::move(pending));
}

std::optional<WalletModel::UnlockContext> IdentityFlow::beginFunding()
{
    // Keeps other calls out while the passphrase and confirmation dialogs run their event loops.
    m_step = Step::FUNDING;
    m_error.clear();
    Q_EMIT changed();
    std::optional<WalletModel::UnlockContext> unlock{m_service.walletModel().requestUnlock()};
    if (!unlock->isValid()) {
        finish(tr("The wallet stayed locked, so nothing was sent."));
        return std::nullopt;
    }
    // While unlocked: a later lookup reaches the identity this funds and the indexes after it.
    keyHash(nextIndex() + GAP_LIMIT);
    return unlock;
}

void IdentityFlow::awaitLock(Funding pending, CTransactionRef tx, WalletModel::UnlockContext unlock)
{
    m_funding = std::move(pending);
    m_tx = std::move(tx);
    m_unlock.emplace(std::move(unlock));
    m_step = Step::WAITING_FOR_LOCK;
    Q_EMIT changed();
    send();
}

void IdentityFlow::fund(const wallet::AssetLockFundingKey& key, CAmount amount, Funding pending)
{
    if (m_step != Step::IDLE || !m_service.client() || m_service.writing() || funding()) return;
    auto unlock{beginFunding()};
    if (!unlock) return;
    interfaces::Wallet& wallet{m_service.walletModel().wallet()};
    CAmount fee{0};
    const auto tx{wallet.createAssetLockTransaction(amount, key, wallet::CCoinControl{}, fee)};
    if (!tx) return finish(QString::fromStdString(util::ErrorString(tx).translated));
    if (!m_confirm(amount, fee)) return finish();

    pending.txid = (*tx)->GetHash();
    pending.amount = amount;
    // Before the asset lock is sent: the record is all that resumes it.
    if (!SaveFunding(wallet, pending)) {
        return finish(tr("The wallet could not save the funding payment, so nothing was sent."));
    }
    if (const auto res{wallet.commitTransaction(*tx, {}, {})}; !res) {
        // Abandoning releases its coins; one it can't abandon may still confirm.
        if (wallet.abandonTransaction(pending.txid)) wallet.writePlatformData(pending.record, {});
        return finish(tr("The network did not accept the funding payment: %1")
                          .arg(QString::fromStdString(util::ErrorString(res).translated)));
    }
    awaitLock(std::move(pending), *tx, std::move(*unlock));
}

void IdentityFlow::resume()
{
    auto pending{funding()};
    if (m_step != Step::IDLE || !m_service.client() || m_service.writing() || !pending) return;
    interfaces::Wallet& wallet{m_service.walletModel().wallet()};
    const auto [tx, status]{WalletTxOf(wallet, pending->txid)};
    if (!tx || status.is_abandoned || status.depth_in_main_chain < 0) {
        // Never sent, or its coins went to another payment: Platform can't get it.
        wallet.writePlatformData(pending->record, {});
        return finish(tr("The funding payment was not sent or was replaced, so it bought no credits. Its coins are "
                         "in your wallet."));
    }
    auto unlock{beginFunding()};
    if (!unlock) return;
    awaitLock(std::move(*pending), tx, std::move(*unlock));
}

void IdentityFlow::send()
{
    if (m_step != Step::WAITING_FOR_LOCK) return;
    const auto proof{m_proof(m_tx)};
    platform::PlatformClient* client{m_service.client()};
    if (!proof || !client) return;

    interfaces::Wallet& wallet{m_service.walletModel().wallet()};
    std::optional<std::vector<IdentityPublicKey>> keys;
    if (!m_funding->top_up) {
        keys = RegistrationKeys(wallet, m_funding->index);
        if (!keys) return finish(tr("Dash Core could not derive your identity's keys from this wallet."));
    }
    // A registration signs with the new identity's keys; a top-up with its funding key only.
    platform::Signing signing{wallet, m_funding->index, PlatformUi::KeepUnlocked(std::move(*m_unlock))};
    m_unlock.reset();
    m_step = Step::SENDING;
    if (m_funding->top_up) {
        client->topUpIdentity(m_funding->identity, *proof, *m_funding->top_up, std::move(signing),
                              m_service.trackWrite<uint64_t>(PlatformUi::Tracked<uint64_t>(
                                  m_call, [this, funding = *m_funding](platform::Result<uint64_t> result) {
                                      // The identity it credits, selected or not.
                                      if (result.ok()) {
                                          for (auto& [index, found] : m_identities) {
                                              if (found.id == funding.identity) found.balance = result.value;
                                          }
                                      }
                                      sent(funding, result.status);
                                  })));
    } else {
        client->registerIdentity(std::move(*keys), *proof, std::move(signing),
                                 m_service.trackWrite<platform::Identity>(PlatformUi::Tracked<platform::Identity>(
                                     m_call, [this, funding = *m_funding](platform::Result<platform::Identity> result) {
                                         if (result.ok()) {
                                             m_identities[funding.index] = std::move(result.value);
                                             // Another identity is selected by the user.
                                             if (!m_index) m_index = funding.index;
                                             m_looked_up = true;
                                         }
                                         sent(funding, result.status);
                                     })));
    }
    Q_EMIT changed();
}

void IdentityFlow::sent(const Funding& funding, const platform::Status& status)
{
    interfaces::Wallet& wallet{m_service.walletModel().wallet()};
    if (status.kind == platform::StatusKind::OK) {
        wallet.writePlatformData(funding.record, {});
        return finish();
    }
    if (status.kind == platform::StatusKind::CONSENSUS && status.consensus_code == CODE_ASSET_LOCK_CONSUMED) {
        // An earlier attempt was applied. The lookup finds the identity it
        // created, or the balance a top-up raised.
        if (funding.top_up) wallet.writePlatformData(funding.record, {});
        finish();
        m_looked_up = false;
        return startLookUp(/*unlock=*/false);
    }
    LogPrintf("DashPay: %s failed: %s\n", funding.top_up ? "identity top-up" : "identity registration", status.message);
    finish(PlatformUi::Describe(status));
}
