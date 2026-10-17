// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/platform/invitationflow.h>

#include <evo/assetlocktx.h>
#include <evo/specialtx.h>
#include <interfaces/wallet.h>
#include <key.h>
#include <logging.h>
#include <qt/clientmodel.h>
#include <qt/platform/platformservice.h>
#include <qt/platform/platformui.h>
#include <qt/platform/usernameflow.h>
#include <streams.h>
#include <tinyformat.h>
#include <util/strencodings.h>
#include <util/time.h>
#include <util/translation.h>
#include <wallet/coincontrol.h>

#include <chrono>
#include <cstring>
#include <ios>
#include <set>
#include <utility>

namespace {
using Funded = InvitationFlow::Funded;

std::string Record(uint32_t index) { return strprintf("%s%u", InvitationFlow::RECORD_PREFIX, index); }

std::optional<Funded> Decode(const std::string& record, const std::vector<unsigned char>& value)
{
    const auto index{ToIntegral<uint32_t>(record.substr(std::strlen(InvitationFlow::RECORD_PREFIX)))};
    if (!index) return std::nullopt;
    Funded funded;
    funded.index = *index;
    try {
        DataStream stream{MakeUCharSpan(value)};
        stream >> funded.txid >> funded.amount >> funded.created;
    } catch (const std::ios_base::failure&) {
        return std::nullopt;
    }
    return funded;
}

//! The first invitation key no asset lock in the wallet pays, as the SDK's
//! wallet picks the next unused one: it holds for a wallet restored from its
//! seed, and a key handed out twice would let either link take both
//! invitations.
uint32_t NextIndex(interfaces::Wallet& wallet)
{
    std::set<CScript> paid;
    for (const auto& wtx : wallet.getWalletTxs()) {
        if (wtx.tx->nType != TRANSACTION_ASSET_LOCK) continue;
        if (const auto payload{GetTxPayload<CAssetLockPayload>(*wtx.tx)}) {
            for (const CTxOut& out : payload->getCreditOutputs()) {
                paid.insert(out.scriptPubKey);
            }
        }
    }
    for (uint32_t index{0};; ++index) {
        const auto key{wallet.getPlatformPubKey(wallet::InvitationFundingKey{index})};
        if (!key || !paid.count(GetScriptForDestination(PKHash{key.value}))) return index;
    }
}

bool Save(interfaces::Wallet& wallet, const Funded& funded)
{
    DataStream stream;
    stream << funded.txid << funded.amount << funded.created;
    return wallet.writePlatformData(Record(funded.index),
                                    {UCharCast(stream.data()), UCharCast(stream.data() + stream.size())});
}

//! Platform can't be reached, or the invitation's payment is not locked
//! enough yet (REJECTED from the binding for a funding transaction it can't
//! find or that is not ChainLocked yet).
bool RetryLater(const platform::Status& status)
{
    return status.kind == platform::StatusKind::UNAVAILABLE || status.kind == platform::StatusKind::REJECTED;
}

//! A failed write, logged without Platform's message, which may name the
//! invitation's funding outpoint.
void LogFailure(const char* what, const platform::Status& status)
{
    LogPrintf("DashPay: %s failed (status %u, consensus code %u)\n", what, static_cast<unsigned>(status.kind),
              status.consensus_code);
}

//! A claim that may succeed later says so, rather than that the link is bad.
QString ClaimError(const platform::Status& status)
{
    if (RetryLater(status)) {
        return InvitationFlow::tr("Dash Platform can't use this invitation yet. Try again shortly.");
    }
    return PlatformUi::Describe(status);
}
} // namespace

InvitationFlow::InvitationFlow(PlatformService& service, IdentityFlow& identity_flow, const UsernameFlow& username_flow,
                               IdentityFlow::Confirm confirm, Show show, ConfirmClaim confirm_claim) :
    m_service(service),
    m_identity_flow(identity_flow),
    m_username_flow(username_flow),
    m_confirm(std::move(confirm)),
    m_show(std::move(show)),
    m_confirm_claim(std::move(confirm_claim))
{
    m_service.addWriter(*this);
    connect(&m_service, &PlatformService::changed, this, &InvitationFlow::serviceChanged);
    connect(&m_service.clientModel(), &ClientModel::instantSendChanged, this, &InvitationFlow::send);
    connect(&m_service.clientModel(), &ClientModel::chainLockChanged, this, &InvitationFlow::send);
}

std::vector<Funded> InvitationFlow::funded() const
{
    std::vector<Funded> funded;
    for (const auto& [record, value] : m_service.walletModel().wallet().getPlatformData(RECORD_PREFIX)) {
        if (auto invitation{Decode(record, value)}) funded.push_back(*invitation);
    }
    return funded;
}

void InvitationFlow::serviceChanged()
{
    // A call the client dropped never calls back: a claim may be tried again.
    if ((m_step == Step::CHECKING || m_step == Step::CLAIMING || m_step == Step::RECLAIMING) && m_call.expired()) {
        finish({}, /*retryable=*/true);
    }
    // The record stays: the invitation can be reclaimed.
    if (m_step == Step::WAITING_FOR_LOCK && m_service.gate() != PlatformService::Gate::NONE) finish();
}

void InvitationFlow::finish(const QString& error, bool retryable)
{
    m_step = Step::IDLE;
    m_index.reset();
    m_tx.reset();
    m_unlock.reset();
    if (!retryable) m_invitation.reset();
    m_error = error;
    Q_EMIT changed();
}

std::optional<WalletModel::UnlockContext> InvitationFlow::unlock()
{
    std::optional<WalletModel::UnlockContext> unlock{m_service.walletModel().requestUnlock()};
    if (!unlock->isValid()) {
        finish(tr("The wallet stayed locked, so nothing was sent."));
        return std::nullopt;
    }
    // The passphrase dialog may have let another flow start, or closed a gate.
    if (!m_service.othersIdle(*this)) {
        finish(tr("Another DashPay request is on its way. Try again once it is done."));
        return std::nullopt;
    }
    if (!m_service.client()) {
        finish({}, /*retryable=*/true);
        return std::nullopt;
    }
    return unlock;
}

void InvitationFlow::create()
{
    if (m_step != Step::IDLE || !m_service.client() || !m_identity_flow.identity() || !m_username_flow.username()) {
        return;
    }
    m_step = Step::FUNDING;
    m_error.clear();
    m_invitation.reset();
    m_inviter = *m_username_flow.username();
    Q_EMIT changed();
    auto unlocked{unlock()};
    if (!unlocked) return;
    interfaces::Wallet& wallet{m_service.walletModel().wallet()};
    Funded funded;
    funded.index = NextIndex(wallet);
    CAmount fee{0};
    const auto tx{wallet.createAssetLockTransaction(AMOUNT, wallet::InvitationFundingKey{funded.index},
                                                    wallet::CCoinControl{}, fee)};
    if (!tx) return finish(QString::fromStdString(util::ErrorString(tx).translated));
    if (!m_confirm(AMOUNT, fee)) return finish();

    funded.txid = (*tx)->GetHash();
    funded.amount = AMOUNT;
    funded.created = TicksSinceEpoch<std::chrono::seconds>(NodeClock::now());
    // Before the asset lock is sent: the record is all that reclaims it.
    if (!Save(wallet, funded)) {
        return finish(tr("The wallet could not save the invitation, so nothing was sent."));
    }
    if (const auto res{wallet.commitTransaction(*tx, {}, {})}; !res) {
        if (wallet.abandonTransaction(funded.txid)) wallet.writePlatformData(Record(funded.index), {});
        return finish(tr("The network did not accept the invitation's payment: %1")
                          .arg(QString::fromStdString(util::ErrorString(res).translated)));
    }
    m_index = funded.index;
    m_tx = *tx;
    m_unlock.emplace(std::move(*unlocked));
    m_step = Step::WAITING_FOR_LOCK;
    Q_EMIT changed();
    send();
}

void InvitationFlow::send()
{
    if (m_step != Step::WAITING_FOR_LOCK) return;
    const auto proof{m_identity_flow.proof(m_tx)};
    if (!proof) return;
    // A key the wallet can't export is invalid, which the encoder refuses.
    const SecureString link{platform::EncodeInvitation(
        m_service.walletModel().wallet().exportInvitationKey(wallet::InvitationFundingKey{*m_index}).value, *proof,
        {m_inviter, {}, {}})};
    if (link.empty()) {
        return finish(tr("Dash Core could not build the invitation's link. Its funds can be reclaimed."));
    }
    finish();
    m_show(link);
}

void InvitationFlow::claim(const SecureString& link)
{
    if (m_step != Step::IDLE || !m_service.client() || !m_identity_flow.lookedUp() || m_identity_flow.identity()) {
        return;
    }
    m_invitation = platform::ParseInvitation(link);
    if (!m_invitation) return finish(tr("This is not a DashPay invitation link."));
    check();
}

void InvitationFlow::retry()
{
    if (retryable() && !m_identity_flow.identity() && m_service.client()) check();
}

void InvitationFlow::check()
{
    m_step = Step::CHECKING;
    m_error.clear();
    m_service.client()->invitationClaimStatus(m_invitation,
                                              PlatformUi::Tracked<platform::InvitationClaim>(m_call, [this](auto result) {
                                                  onChecked(std::move(result));
                                              }));
    Q_EMIT changed();
}

void InvitationFlow::onChecked(platform::Result<platform::InvitationClaim> result)
{
    if (!result.ok()) return finish(ClaimError(result.status), RetryLater(result.status));
    if (result.value.already_claimed) return claimedAlready();
    if (!m_confirm_claim(static_cast<CAmount>(result.value.amount_duffs), m_invitation->inviter())) return finish();
    auto unlocked{unlock()};
    if (!unlocked) return;
    // The dialogs run event loops, in which the identity may have been found or forgotten.
    if (!m_identity_flow.lookedUp() || m_identity_flow.identity()) return finish();
    interfaces::Wallet& wallet{m_service.walletModel().wallet()};
    auto keys{IdentityFlow::RegistrationKeys(wallet)};
    if (!keys) return finish(tr("Dash Core could not derive your identity's keys from this wallet."));
    m_step = Step::CLAIMING;
    Q_EMIT changed();
    m_service.client()->claimInvitation(
        m_invitation, std::move(*keys),
        {wallet, IdentityFlow::IDENTITY_INDEX, PlatformUi::KeepUnlocked(std::move(*unlocked))},
        m_service.trackWrite<platform::Identity>(PlatformUi::Tracked<platform::Identity>(
            m_call, [this](platform::Result<platform::Identity> result) { onClaimed(std::move(result)); })));
}

void InvitationFlow::onClaimed(platform::Result<platform::Identity> result)
{
    if (result.ok()) {
        m_identity_flow.adopt(std::move(result.value));
        return finish();
    }
    if (PlatformUi::IsAssetLockConsumed(result.status)) return claimedAlready();
    LogFailure("claiming an invitation", result.status);
    finish(ClaimError(result.status), RetryLater(result.status));
}

void InvitationFlow::reclaim(uint32_t index)
{
    if (m_step != Step::IDLE || !m_service.client() || !m_identity_flow.identity()) return;
    std::optional<uint256> txid;
    for (const Funded& invitation : funded()) {
        if (invitation.index == index) txid = invitation.txid;
    }
    if (!txid) return;
    interfaces::Wallet& wallet{m_service.walletModel().wallet()};
    const CTransactionRef tx{IdentityFlow::SentTx(wallet, *txid)};
    if (!tx) {
        wallet.writePlatformData(Record(index), {});
        return finish(tr("The invitation's payment was not sent or was replaced, so it holds no credits. Its coins "
                         "are in your wallet."));
    }
    const auto proof{m_identity_flow.proof(tx)};
    if (!proof) return finish(tr("The network has not locked the invitation's payment yet. Try again shortly."));
    auto unlocked{unlock()};
    if (!unlocked) return;
    // The passphrase dialog runs an event loop, which may have closed a gate.
    const auto& identity{m_identity_flow.identity()};
    if (!identity) return finish();
    m_step = Step::RECLAIMING;
    m_error.clear();
    Q_EMIT changed();
    m_service.client()->topUpIdentity(
        identity->id, *proof, wallet::InvitationFundingKey{index},
        {wallet, IdentityFlow::IDENTITY_INDEX, PlatformUi::KeepUnlocked(std::move(*unlocked))},
        m_service.trackWrite<uint64_t>(PlatformUi::Tracked<uint64_t>(
            m_call, [this, index](platform::Result<uint64_t> result) { onReclaimed(index, std::move(result)); })));
}

void InvitationFlow::claimedAlready()
{
    finish(tr("This invitation's credits were already used."));
    // Perhaps by our own claim or reclaim, whose answer was lost: the
    // identity, looked up again, says.
    m_identity_flow.lookUp();
}

void InvitationFlow::onReclaimed(uint32_t index, platform::Result<uint64_t> result)
{
    const bool consumed{PlatformUi::IsAssetLockConsumed(result.status)};
    if (result.ok() || consumed) m_service.walletModel().wallet().writePlatformData(Record(index), {});
    if (consumed) return claimedAlready();
    if (!result.ok()) {
        LogFailure("reclaiming an invitation", result.status);
        return finish(PlatformUi::Describe(result.status));
    }
    if (auto identity{m_identity_flow.identity()}) {
        identity->balance = result.value;
        m_identity_flow.adopt(std::move(*identity));
    }
    finish();
}
