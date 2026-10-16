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

//! Every identity is index 0 until the UI offers more than one.
constexpr uint32_t IDENTITY_INDEX{0};
//! The asset lock's one credit output.
constexpr uint32_t CREDIT_OUTPUT{0};
//! rs-dpp IdentityAssetLockTransactionOutPointAlreadyConsumedError.
constexpr uint32_t CODE_ASSET_LOCK_CONSUMED{10504};
//! The funding key index of the next top-up.
constexpr const char* NEXT_TOP_UP_RECORD{"dashpay/topup-index"};

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

std::string RegistrationRecord() { return strprintf("%sidentity/%u", IdentityFlow::FUNDING_PREFIX, IDENTITY_INDEX); }
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
    if (name.rfind("topup/", 0) == 0) {
        const auto index{ToIntegral<uint32_t>(name.substr(std::strlen("topup/")))};
        if (!index) return std::nullopt;
        funding.top_up = wallet::TopupFundingKey{*index};
    } else if (record != RegistrationRecord()) {
        return std::nullopt;
    }
    try {
        DataStream stream{MakeUCharSpan(value)};
        stream >> funding.txid >> funding.amount;
        if (funding.top_up) stream >> funding.identity;
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

std::optional<std::array<uint8_t, 20>> MasterKeyHash(interfaces::Wallet& wallet)
{
    const auto key{wallet.getPlatformPubKey(wallet::IdentityAuthKey{IDENTITY_INDEX, REGISTRATION_KEYS[0].id})};
    if (!key) return std::nullopt;
    const CKeyID hash{key.value.GetID()};
    std::array<uint8_t, 20> out;
    std::copy(hash.begin(), hash.end(), out.begin());
    return out;
}

std::optional<std::vector<IdentityPublicKey>> RegistrationKeys(interfaces::Wallet& wallet)
{
    std::vector<IdentityPublicKey> keys;
    for (const KeySpec& spec : REGISTRATION_KEYS) {
        const auto pubkey{wallet.getPlatformPubKey(wallet::IdentityAuthKey{IDENTITY_INDEX, spec.id})};
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

//! Keeps the wallet unlocked until the client has signed the write.
class WalletUnlock final : public platform::UnlockScope
{
public:
    explicit WalletUnlock(WalletModel::UnlockContext unlock) :
        m_unlock(std::move(unlock))
    {
    }

private:
    WalletModel::UnlockContext m_unlock;
};
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

template <typename T>
platform::PlatformClient::Callback<T> IdentityFlow::tracked(std::function<void(platform::Result<T>)> fn)
{
    auto call{std::make_shared<int>(0)};
    m_call = call;
    return [call, fn = std::move(fn)](platform::Result<T> result) { fn(std::move(result)); };
}

void IdentityFlow::serviceChanged()
{
    // A call the client dropped never calls back; a funding record stays for Continue.
    if ((m_step == Step::LOOKING_UP || m_step == Step::SENDING) && m_call.expired()) finish();
    if (m_service.gate() != PlatformService::Gate::NONE) {
        m_looked_up = false;
        m_identity.reset();
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

void IdentityFlow::startLookUp(bool unlock)
{
    if (m_step != Step::IDLE || !m_service.client()) return;
    if (!m_key_hash) {
        std::optional<WalletModel::UnlockContext> context;
        if (unlock) context.emplace(m_service.walletModel().requestUnlock());
        m_key_hash = MasterKeyHash(m_service.walletModel().wallet());
    }
    if (!m_key_hash) {
        if (unlock) finish(tr("The wallet stayed locked, so DashPay could not look up your identity."));
        return;
    }
    // The passphrase dialog runs an event loop, which may have started a call.
    platform::PlatformClient* client{m_service.client()};
    if (m_step != Step::IDLE || !client) return;
    m_step = Step::LOOKING_UP;
    m_error.clear();
    client->getIdentityByPublicKeyHash(*m_key_hash, tracked<platform::Identity>(
                                                        [this](auto result) { onLookedUp(std::move(result)); }));
    Q_EMIT changed();
}

void IdentityFlow::onLookedUp(platform::Result<platform::Identity> result)
{
    if (!result.ok() && result.status.kind != platform::StatusKind::NOT_FOUND) {
        return finish(PlatformUi::Describe(result.status));
    }
    m_looked_up = true;
    m_identity.reset();
    if (result.ok()) {
        m_identity = std::move(result.value);
        // The record is erased once the identity is found.
        if (const auto pending{funding()}; pending && !pending->top_up) {
            m_service.walletModel().wallet().writePlatformData(pending->record, {});
        }
    }
    finish();
}

void IdentityFlow::create()
{
    if (!m_looked_up || m_identity) return;
    Funding pending;
    pending.record = RegistrationRecord();
    fund(wallet::RegistrationFundingKey{IDENTITY_INDEX}, REGISTRATION_AMOUNT, std::move(pending));
}

void IdentityFlow::topUp(CAmount amount)
{
    if (!m_identity) return;
    const uint32_t index{NextTopUpIndex(m_service.walletModel().wallet())};
    Funding pending;
    pending.record = TopUpRecord(index);
    pending.top_up = wallet::TopupFundingKey{index};
    pending.identity = m_identity->id;
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
    if (!m_key_hash) m_key_hash = MasterKeyHash(m_service.walletModel().wallet());
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
    if (m_step != Step::IDLE || !m_service.client() || funding()) return;
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
    if (m_step != Step::IDLE || !m_service.client() || !pending) return;
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
        keys = RegistrationKeys(wallet);
        if (!keys) return finish(tr("Dash Core could not derive your identity's keys from this wallet."));
    }
    platform::Signing signing{wallet, IDENTITY_INDEX, std::make_unique<WalletUnlock>(std::move(*m_unlock))};
    m_unlock.reset();
    m_step = Step::SENDING;
    if (m_funding->top_up) {
        client->topUpIdentity(m_funding->identity, *proof, *m_funding->top_up, std::move(signing),
                              m_service.trackWrite<uint64_t>(
                                  tracked<uint64_t>([this, funding = *m_funding](platform::Result<uint64_t> result) {
                                      if (result.ok() && m_identity) m_identity->balance = result.value;
                                      sent(funding, result.status);
                                  })));
    } else {
        client->registerIdentity(std::move(*keys), *proof, std::move(signing),
                                 m_service.trackWrite<platform::Identity>(tracked<platform::Identity>(
                                     [this, funding = *m_funding](platform::Result<platform::Identity> result) {
                                         if (result.ok()) {
                                             m_identity = std::move(result.value);
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
