// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <qt/test/masternodetestutil.h>

#include <bls/bls.h>
#include <chainparams.h>
#include <core_io.h>
#include <evo/dmn_types.h>
#include <evo/netinfo.h>
#include <evo/sharedcollateral.h>
#include <evo/specialtx.h>
#include <interfaces/node.h>
#include <key_io.h>
#include <node/context.h>
#include <script/descriptor.h>
#include <script/standard.h>
#include <sync.h>
#include <test/util/setup_common.h>
#include <util/system.h>
#include <validation.h>
#include <wallet/context.h>
#include <wallet/test/util.h>
#include <wallet/wallet.h>

#include <univalue.h>

#include <utility>

using wallet::AddWallet;
using wallet::CWallet;
using wallet::CreateMockableWalletDatabase;
using wallet::RemoveWallet;
using wallet::WALLET_FLAG_DESCRIPTORS;
using wallet::WalletContext;
using wallet::WalletDescriptor;
using wallet::WalletRescanReserver;

namespace MasternodeTestUtil {

WalletGuard::WalletGuard(WalletContext& context) :
    m_context{context}
{
}

WalletGuard::WalletGuard(WalletContext& context, std::shared_ptr<CWallet> wallet) :
    m_context{context}
{
    keep(std::move(wallet));
}

WalletGuard::~WalletGuard()
{
    for (const auto& wallet : m_wallets) {
        RemoveWallet(m_context, wallet, /*load_on_start=*/std::nullopt);
    }
}

void WalletGuard::keep(std::shared_ptr<CWallet> wallet)
{
    if (wallet) m_wallets.push_back(std::move(wallet));
}

namespace {
//! Everything the three public factories vary; built only here, so no call site
//! has to spell out fields it does not care about.
struct WalletOptions {
    const CKey* key{nullptr};               //!< imported as combo(), so the wallet can spend it
    const TestChain100Setup* setup{nullptr}; //!< also take `key`'s coins: address book + rescan
    bool encrypt{false};
    bool broadcast{false};
};

std::shared_ptr<CWallet> MakeWallet(interfaces::Node& node, WalletContext& context, const std::string& name,
                                    const WalletOptions& options)
{
    auto wallet{std::make_shared<CWallet>(node.context()->chain.get(), name, CreateMockableWalletDatabase())};
    wallet->LoadWallet();
    wallet->SetWalletFlag(WALLET_FLAG_DESCRIPTORS);
    const CBlockIndex* const tip{options.setup == nullptr
                                     ? nullptr
                                     : WITH_LOCK(node.context()->chainman->GetMutex(),
                                                 return node.context()->chainman->ActiveChain().Tip())};
    {
        LOCK(wallet->cs_wallet);
        wallet->SetupDescriptorScriptPubKeyMans("", "");
        if (options.key != nullptr) {
            FlatSigningProvider provider;
            std::string error;
            std::unique_ptr<Descriptor> descriptor{
                Parse("combo(" + EncodeSecret(*options.key) + ")", provider, error, /*require_checksum=*/false)};
            if (!descriptor) return nullptr;
            WalletDescriptor wallet_descriptor(std::move(descriptor), 0, 0, 1, 1);
            if (!wallet->AddWalletDescriptor(wallet_descriptor, provider, "", false)) return nullptr;
            if (options.setup != nullptr) {
                wallet->SetAddressBook(PKHash(options.key->GetPubKey()), "", wallet::AddressPurpose::RECEIVE);
                wallet->SetLastBlockProcessed(tip->nHeight, tip->GetBlockHash());
            }
        }
    }
    if (options.setup != nullptr) {
        WalletRescanReserver reserver(*wallet);
        reserver.reserve();
        if (wallet->ScanForWalletTransactions(Params().GetConsensus().hashGenesisBlock, /*start_height=*/0,
                                              /*max_height=*/{}, reserver, /*fUpdate=*/true, /*save_progress=*/false)
                .status != CWallet::ScanResult::SUCCESS) {
            return nullptr;
        }
    }
    if (options.encrypt && !wallet->EncryptWallet("test passphrase")) return nullptr;
    if (options.broadcast) wallet->SetBroadcastTransactions(true);
    AddWallet(context, wallet);
    return wallet;
}
} // anonymous namespace

std::shared_ptr<CWallet> MakeTestWallet(interfaces::Node& node, WalletContext& context, const std::string& name,
                                        bool broadcast)
{
    return MakeWallet(node, context, name, {nullptr, nullptr, /*encrypt=*/false, broadcast});
}

std::shared_ptr<CWallet> MakeSpendingWallet(interfaces::Node& node, WalletContext& context, const std::string& name,
                                            const CKey& key)
{
    return MakeWallet(node, context, name, {&key, nullptr, /*encrypt=*/false, /*broadcast=*/false});
}

std::shared_ptr<CWallet> MakeCoinbaseWallet(interfaces::Node& node, WalletContext& context,
                                            const TestChain100Setup& setup, const std::string& name, bool encrypt)
{
    return MakeWallet(node, context, name, {&setup.coinbaseKey, &setup, encrypt, /*broadcast=*/false});
}

GuiModels::GuiModels(interfaces::Node& node) :
    options{node},
    client{node, &options},
    ok{options.Init(error)}
{
}

MnShareSession MakeInvitation(std::initializer_list<std::pair<const char*, CAmount>> rows)
{
    MnShareSession session;
    CKey voting_key;
    for (const auto& [name, amount] : rows) {
        MnShareSession::Share share;
        share.label = QString::fromLatin1(name);
        share.amount = amount;
        session.shares().push_back(share);
    }
    session.terms().votingAddress = FreshP2PKHAddress(&voting_key);
    session.terms().earlyPeriodBlocks = 5000;
    session.terms().earlyPenalty = 5 * COIN;
    session.setCoordinatorLabel(QStringLiteral("alice"));
    return session;
}

MnShareSession DraftReply(const MnShareSession& invitation, int share_index, const QString& txid, CKey* owner_key_out)
{
    MnShareSession reply{invitation};
    CKey owner_key;
    CKey refund_key;
    CKey change_key;
    reply.shares()[share_index].ownerAddress = FreshP2PKHAddress(&owner_key);
    reply.shares()[share_index].refundAddress = FreshP2PKHAddress(&refund_key);
    if (owner_key_out != nullptr) *owner_key_out = owner_key;

    MnShareSession::Contribution contribution;
    contribution.label = invitation.shares()[share_index].label;
    MnShareSession::Input input;
    input.txid = txid;
    input.vout = 0;
    contribution.inputs.push_back(input);
    contribution.hasChange = true;
    contribution.changeAddress = FreshP2PKHAddress(&change_key);
    contribution.changeAmount = COIN;
    QString error;
    // A failure here is surfaced by the caller's own absorb assertions
    if (!reply.addContribution(contribution, error)) return invitation;

    MnShareSession normalised{invitation};
    UniValue json{reply.toJson()};
    json.pushKV("revision", invitation.revision());
    QString parse_error;
    if (!normalised.fromJson(json, parse_error)) return invitation;
    return normalised;
}

PreparedRegistration PrepareRegistration(const MnShareSession& session, const CBLSSecretKey& operator_secret)
{
    PreparedRegistration prepared;
    CProRegTx& payload{prepared.payload};
    payload.nVersion = ProTxVersion::ExtAddr;
    payload.nType = MnType::Regular;
    payload.netInfo = NetInfoInterface::MakeNetInfo(payload.nVersion);
    for (const auto& share : session.shares()) {
        payload.shares.emplace_back(
            share.amount, GetScriptForDestination(DecodeDestination(share.refundAddress.toStdString())),
            share.rewardAddress.isEmpty()
                ? CScript()
                : GetScriptForDestination(DecodeDestination(share.rewardAddress.toStdString())),
            ToKeyID(std::get<PKHash>(DecodeDestination(share.ownerAddress.toStdString()))));
    }
    for (const QString& entry : session.terms().coreP2PAddrs.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        payload.netInfo->AddEntry(NetInfoPurpose::CORE_P2P, entry.toStdString());
    }
    payload.vchJoinSigs.assign(payload.shares.size(), CompactSignature{});
    payload.keyIDVoting = ToKeyID(std::get<PKHash>(DecodeDestination(session.terms().votingAddress.toStdString())));
    payload.pubKeyOperator.Set(operator_secret.GetPublicKey(), /*specificLegacyScheme=*/false);
    payload.nOperatorReward = session.terms().operatorReward;
    payload.nEarlyPeriodBlocks = session.terms().earlyPeriodBlocks;
    payload.nEarlyPenalty = session.terms().earlyPenalty;

    CMutableTransaction& tx{prepared.tx};
    if (!DecodeHexTx(tx, session.fundingTxHex().toStdString())) return prepared;
    tx.nVersion = 3;
    tx.nType = TRANSACTION_PROVIDER_REGISTER;
    tx.vout.emplace_back(GetMnType(MnType::Regular).collat_amount, SharedCollateralScript());
    prepared.collateralIndex = static_cast<int>(tx.vout.size() - 1);
    payload.collateralOutpoint = COutPoint(uint256(), static_cast<uint32_t>(prepared.collateralIndex));
    SetTxPayload(tx, payload);
    prepared.consentHash = payload.MakeSharedRegConsentHash(CTransaction(tx));
    prepared.consentHashHex = QString::fromStdString(prepared.consentHash.ToString());
    prepared.txHex = QString::fromStdString(EncodeHexTx(CTransaction(tx)));
    return prepared;
}

QString FreshP2PKHAddress(CKey* key_out)
{
    CKey local;
    CKey& key{key_out != nullptr ? *key_out : local};
    key.MakeNewKey(/*fCompressed=*/true);
    return QString::fromStdString(EncodeDestination(PKHash(key.GetPubKey())));
}

CKeyID TestKeyID(uint8_t marker)
{
    uint160 key_id;
    key_id.begin()[0] = marker;
    return CKeyID{key_id};
}

QString FakeTxid(char c) { return QString(64, QChar::fromLatin1(c)); }

QString FreshOperatorPubKey()
{
    CBLSSecretKey secret;
    secret.MakeNewKey();
    return QString::fromStdString(secret.GetPublicKey().ToString(/*specificLegacyScheme=*/false));
}

} // namespace MasternodeTestUtil
