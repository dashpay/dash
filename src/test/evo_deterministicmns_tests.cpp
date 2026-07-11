// Copyright (c) 2018-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/setup_common.h>

#include <chainparams.h>
#include <clientversion.h>
#include <consensus/merkle.h>
#include <consensus/validation.h>
#include <deploymentstatus.h>
#include <evo/deterministicmns.h>
#include <evo/providertx.h>
#include <evo/simplifiedmns.h>
#include <evo/specialtx.h>
#include <evo/specialtxman.h>
#include <llmq/context.h>
#include <messagesigner.h>
#include <mempool_args.h>
#include <netbase.h>
#include <node/transaction.h>
#include <policy/policy.h>
#include <pow.h>
#include <script/interpreter.h>
#include <script/sign.h>
#include <script/signingprovider.h>
#include <script/standard.h>
#include <streams.h>
#include <test/util/txmempool.h>
#include <txmempool.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <optional>
#include <vector>

using node::GetTransaction;

using SimpleUTXOMap = std::map<COutPoint, std::pair<int, CAmount>>;

static SimpleUTXOMap BuildSimpleUtxoMap(const std::vector<CTransactionRef>& txs)
{
    SimpleUTXOMap utxos;
    for (size_t i = 0; i < txs.size(); i++) {
        auto& tx = txs[i];
        for (size_t j = 0; j < tx->vout.size(); j++) {
            utxos.emplace(COutPoint(tx->GetHash(), j), std::make_pair((int)i + 1, tx->vout[j].nValue));
        }
    }
    return utxos;
}

static std::vector<COutPoint> SelectUTXOs(const CChain& active_chain, SimpleUTXOMap& utoxs, CAmount amount, CAmount& changeRet)
{
    changeRet = 0;

    std::vector<COutPoint> selectedUtxos;
    CAmount selectedAmount = 0;
    while (!utoxs.empty()) {
        bool found = false;
        for (auto it = utoxs.begin(); it != utoxs.end(); ++it) {
            if (active_chain.Height() - it->second.first < 101) {
                continue;
            }

            found = true;
            selectedAmount += it->second.second;
            selectedUtxos.emplace_back(it->first);
            utoxs.erase(it);
            break;
        }
        BOOST_REQUIRE(found);
        if (selectedAmount >= amount) {
            changeRet = selectedAmount - amount;
            break;
        }
    }

    return selectedUtxos;
}

static void FundTransaction(const CChain& active_chain, CMutableTransaction& tx, SimpleUTXOMap& utoxs, const CScript& scriptPayout, CAmount amount, const CKey& coinbaseKey)
{
    CAmount change;
    auto inputs = SelectUTXOs(active_chain, utoxs, amount, change);
    for (size_t i = 0; i < inputs.size(); i++) {
        tx.vin.emplace_back(CTxIn(inputs[i]));
    }
    tx.vout.emplace_back(CTxOut(amount, scriptPayout));
    if (change != 0) {
        tx.vout.emplace_back(CTxOut(change, scriptPayout));
    }
}

static void SignTransaction(const CTxMemPool& mempool, CMutableTransaction& tx, const CKey& coinbaseKey)
{
    FillableSigningProvider tempKeystore;
    tempKeystore.AddKeyPubKey(coinbaseKey, coinbaseKey.GetPubKey());

    for (size_t i = 0; i < tx.vin.size(); i++) {
        uint256 hashBlock;
        CTransactionRef txFrom = GetTransaction(/*block_index=*/nullptr, &mempool, tx.vin[i].prevout.hash,
                                                Params().GetConsensus(), hashBlock);
        BOOST_REQUIRE(txFrom);
        BOOST_REQUIRE(SignSignature(tempKeystore, *txFrom, tx, i, SIGHASH_ALL));
    }
}

static CMutableTransaction CreateProRegTx(const CChain& active_chain, const CTxMemPool& mempool, SimpleUTXOMap& utxos, int port, const CScript& scriptPayout, const CKey& coinbaseKey, CKey& ownerKeyRet, CBLSSecretKey& operatorKeyRet)
{
    ownerKeyRet.MakeNewKey(true);
    operatorKeyRet.MakeNewKey();

    CProRegTx proTx;
    proTx.nVersion = ProTxVersion::GetMax(!bls::bls_legacy_scheme, /*is_extended_addr=*/false);
    proTx.netInfo = NetInfoInterface::MakeNetInfo(proTx.nVersion);
    proTx.collateralOutpoint.n = 0;
    BOOST_CHECK_EQUAL(proTx.netInfo->AddEntry(NetInfoPurpose::CORE_P2P, strprintf("1.1.1.1:%d", port)),
                      NetInfoStatus::Success);
    proTx.keyIDOwner = ownerKeyRet.GetPubKey().GetID();
    proTx.pubKeyOperator.Set(operatorKeyRet.GetPublicKey(), bls::bls_legacy_scheme.load());
    proTx.keyIDVoting = ownerKeyRet.GetPubKey().GetID();
    proTx.scriptPayout = scriptPayout;

    CMutableTransaction tx;
    tx.nVersion = 3;
    tx.nType = TRANSACTION_PROVIDER_REGISTER;
    FundTransaction(active_chain, tx, utxos, scriptPayout, dmn_types::Regular.collat_amount, coinbaseKey);
    proTx.inputsHash = CalcTxInputsHash(CTransaction(tx));
    SetTxPayload(tx, proTx);
    SignTransaction(mempool, tx, coinbaseKey);

    return tx;
}

static CMutableTransaction CreateProUpServTx(const CChain& active_chain, const CTxMemPool& mempool, SimpleUTXOMap& utxos, const uint256& proTxHash, const CBLSSecretKey& operatorKey, int port, const CScript& scriptOperatorPayout, const CKey& coinbaseKey)
{
    CProUpServTx proTx;
    proTx.nVersion = ProTxVersion::GetMax(!bls::bls_legacy_scheme, /*is_extended_addr=*/false);
    proTx.netInfo = NetInfoInterface::MakeNetInfo(proTx.nVersion);
    proTx.proTxHash = proTxHash;
    BOOST_CHECK_EQUAL(proTx.netInfo->AddEntry(NetInfoPurpose::CORE_P2P, strprintf("1.1.1.1:%d", port)),
                      NetInfoStatus::Success);
    proTx.scriptOperatorPayout = scriptOperatorPayout;

    CMutableTransaction tx;
    tx.nVersion = 3;
    tx.nType = TRANSACTION_PROVIDER_UPDATE_SERVICE;
    FundTransaction(active_chain, tx, utxos, GetScriptForDestination(PKHash(coinbaseKey.GetPubKey())), 1 * COIN, coinbaseKey);
    proTx.inputsHash = CalcTxInputsHash(CTransaction(tx));
    proTx.sig = operatorKey.Sign(::SerializeHash(proTx), bls::bls_legacy_scheme);
    SetTxPayload(tx, proTx);
    SignTransaction(mempool, tx, coinbaseKey);

    return tx;
}

static CMutableTransaction CreateProUpRegTx(const CChain& active_chain, const CTxMemPool& mempool, SimpleUTXOMap& utxos, const uint256& proTxHash, const CKey& mnKey, const CBLSPublicKey& pubKeyOperator, const CKeyID& keyIDVoting, const CScript& scriptPayout, const CKey& coinbaseKey)
{
    CProUpRegTx proTx;
    proTx.nVersion = ProTxVersion::GetMax(!bls::bls_legacy_scheme, /*is_extended_addr=*/false);
    proTx.proTxHash = proTxHash;
    proTx.pubKeyOperator.Set(pubKeyOperator, bls::bls_legacy_scheme.load());
    proTx.keyIDVoting = keyIDVoting;
    proTx.scriptPayout = scriptPayout;

    CMutableTransaction tx;
    tx.nVersion = 3;
    tx.nType = TRANSACTION_PROVIDER_UPDATE_REGISTRAR;
    FundTransaction(active_chain, tx, utxos, GetScriptForDestination(PKHash(coinbaseKey.GetPubKey())), 1 * COIN, coinbaseKey);
    proTx.inputsHash = CalcTxInputsHash(CTransaction(tx));
    CHashSigner::SignHash(::SerializeHash(proTx), mnKey, proTx.vchSig);
    SetTxPayload(tx, proTx);
    SignTransaction(mempool, tx, coinbaseKey);

    return tx;
}

static CMutableTransaction CreateProUpRevTx(const CChain& active_chain, const CTxMemPool& mempool, SimpleUTXOMap& utxos, const uint256& proTxHash, const CBLSSecretKey& operatorKey, const CKey& coinbaseKey)
{
    CProUpRevTx proTx;
    proTx.nVersion = ProTxVersion::GetMax(!bls::bls_legacy_scheme, /*is_extended_addr=*/false);
    proTx.proTxHash = proTxHash;

    CMutableTransaction tx;
    tx.nVersion = 3;
    tx.nType = TRANSACTION_PROVIDER_UPDATE_REVOKE;
    FundTransaction(active_chain, tx, utxos, GetScriptForDestination(PKHash(coinbaseKey.GetPubKey())), 1 * COIN, coinbaseKey);
    proTx.inputsHash = CalcTxInputsHash(CTransaction(tx));
    proTx.sig = operatorKey.Sign(::SerializeHash(proTx), bls::bls_legacy_scheme);
    SetTxPayload(tx, proTx);
    SignTransaction(mempool, tx, coinbaseKey);

    return tx;
}

template<typename ProTx>
static CMutableTransaction MalleateProTxPayout(const CMutableTransaction& tx)
{
    auto opt_protx = GetTxPayload<ProTx>(tx);
    BOOST_REQUIRE(opt_protx.has_value());
    auto& protx = *opt_protx;

    CKey key;
    key.MakeNewKey(false);
    protx.scriptPayout = GetScriptForDestination(PKHash(key.GetPubKey()));

    CMutableTransaction tx2 = tx;
    SetTxPayload(tx2, protx);

    return tx2;
}

static CScript GenerateRandomAddress()
{
    CKey key;
    key.MakeNewKey(false);
    return GetScriptForDestination(PKHash(key.GetPubKey()));
}

static CDeterministicMNCPtr FindPayoutDmn(CDeterministicMNManager& dmnman, const CBlock& block)
{
    auto dmnList = dmnman.GetListAtChainTip();

    for (const auto& txout : block.vtx[0]->vout) {
        CDeterministicMNCPtr found;
        dmnList.ForEachMNShared(/*onlyValid=*/true, [&](const auto& dmn) {
            if (found == nullptr && txout.scriptPubKey == dmn->pdmnState->scriptPayout) {
                found = dmn;
            }
        });
        if (found != nullptr) {
            return found;
        }
    }
    return nullptr;
}

static bool CheckTransactionSignature(const CTxMemPool& mempool, const CMutableTransaction& tx)
{
    for (unsigned int i = 0; i < tx.vin.size(); i++) {
        const auto& txin = tx.vin[i];
        uint256 hashBlock;
        CTransactionRef txFrom = GetTransaction(/*block_index=*/nullptr, &mempool, txin.prevout.hash,
                                                Params().GetConsensus(), hashBlock);
        BOOST_REQUIRE(txFrom);

        CAmount amount = txFrom->vout[txin.prevout.n].nValue;
        if (!VerifyScript(txin.scriptSig, txFrom->vout[txin.prevout.n].scriptPubKey, STANDARD_SCRIPT_VERIFY_FLAGS, MutableTransactionSignatureChecker(&tx, i, amount, MissingDataBehavior::ASSERT_FAIL))) {
            return false;
        }
    }
    return true;
}

void FuncDIP3Activation(TestChainSetup& setup)
{
    auto& chainman = *Assert(setup.m_node.chainman.get());
    auto& dmnman = *Assert(setup.m_node.dmnman);
    auto tip_index    = [&] { return WITH_LOCK(::cs_main, return chainman.ActiveChain().Tip()); };
    auto tip_height   = [&] { return WITH_LOCK(::cs_main, return chainman.ActiveChain().Height()); };
    auto tip_hash     = [&] { return WITH_LOCK(::cs_main, return chainman.ActiveChain().Tip()->GetBlockHash()); };
    auto sync_dmn_tip = [&] { dmnman.UpdatedBlockTip(tip_index()); };

    auto utxos = BuildSimpleUtxoMap(setup.m_coinbase_txns);
    CKey ownerKey;
    CBLSSecretKey operatorKey;
    CTxDestination payoutDest = DecodeDestination("yRq1Ky1AfFmf597rnotj7QRxsDUKePVWNF");
    auto tx = WITH_LOCK(::cs_main, return CreateProRegTx(chainman.ActiveChain(), *(setup.m_node.mempool), utxos, 1, GetScriptForDestination(payoutDest), setup.coinbaseKey, ownerKey, operatorKey));
    std::vector<CMutableTransaction> txns = {tx};

    const CScript coinbase_pk = GetScriptForRawPubKey(setup.coinbaseKey.GetPubKey());
    int nHeight = tip_height();

    // We start one block before DIP3 activation, so mining a block with a DIP3 transaction should fail
    auto block = std::make_shared<CBlock>(setup.CreateBlock(txns, coinbase_pk, chainman.ActiveChainstate()));
    chainman.ProcessNewBlock(block, true, nullptr);
    BOOST_CHECK_EQUAL(tip_height(), nHeight);
    BOOST_REQUIRE(block->GetHash() != tip_hash());
    BOOST_REQUIRE(!dmnman.GetListAtChainTip().HasMN(tx.GetHash()));

    // This block should activate DIP3
    setup.CreateAndProcessBlock({}, coinbase_pk);
    BOOST_CHECK_EQUAL(tip_height(), nHeight + 1);
    // Mining a block with a DIP3 transaction should succeed now
    block = std::make_shared<CBlock>(setup.CreateBlock(txns, coinbase_pk, chainman.ActiveChainstate()));
    BOOST_REQUIRE(chainman.ProcessNewBlock(block, true, nullptr));
    sync_dmn_tip();
    BOOST_CHECK_EQUAL(tip_height(), nHeight + 2);
    BOOST_CHECK_EQUAL(block->GetHash(), tip_hash());
    BOOST_REQUIRE(dmnman.GetListAtChainTip().HasMN(tx.GetHash()));
};

void FuncV19Activation(TestChainSetup& setup)
{
    auto& chainman = *Assert(setup.m_node.chainman.get());
    auto& dmnman = *Assert(setup.m_node.dmnman);
    auto tip_index    = [&] { return WITH_LOCK(::cs_main, return chainman.ActiveChain().Tip()); };
    auto tip_height   = [&] { return WITH_LOCK(::cs_main, return chainman.ActiveChain().Height()); };
    auto sync_dmn_tip = [&] { dmnman.UpdatedBlockTip(tip_index()); };

    BOOST_REQUIRE(!DeploymentActiveAfter(tip_index(), chainman.GetConsensus(), Consensus::DEPLOYMENT_V19));

    // create
    auto utxos = BuildSimpleUtxoMap(setup.m_coinbase_txns);
    CKey owner_key;
    CBLSSecretKey operator_key;
    CKey collateral_key;
    collateral_key.MakeNewKey(false);
    auto collateralScript = GetScriptForDestination(PKHash(collateral_key.GetPubKey()));
    auto tx_reg = WITH_LOCK(::cs_main, return CreateProRegTx(chainman.ActiveChain(), *(setup.m_node.mempool), utxos, 1, collateralScript, setup.coinbaseKey, owner_key, operator_key));
    auto tx_reg_hash = tx_reg.GetHash();

    const CScript coinbase_pk = GetScriptForRawPubKey(setup.coinbaseKey.GetPubKey());
    int nHeight = tip_height();

    auto block = std::make_shared<CBlock>(setup.CreateBlock({tx_reg}, coinbase_pk, chainman.ActiveChainstate()));
    BOOST_REQUIRE(chainman.ProcessNewBlock(block, true, nullptr));
    BOOST_REQUIRE(!DeploymentActiveAfter(tip_index(), chainman.GetConsensus(), Consensus::DEPLOYMENT_V19));
    ++nHeight;
    BOOST_CHECK_EQUAL(tip_height(), nHeight);
    sync_dmn_tip();
    dmnman.DoMaintenance();
    auto tip_list = dmnman.GetListAtChainTip();
    BOOST_REQUIRE(tip_list.HasMN(tx_reg_hash));
    auto pindex_create = tip_index();
    auto base_list = dmnman.GetListForBlock(pindex_create);
    std::vector<CDeterministicMNListDiff> diffs;

    // update
    CBLSSecretKey operator_key_new;
    operator_key_new.MakeNewKey();
    auto tx_upreg = WITH_LOCK(::cs_main, return CreateProUpRegTx(chainman.ActiveChain(), *(setup.m_node.mempool), utxos, tx_reg_hash, owner_key, operator_key_new.GetPublicKey(), owner_key.GetPubKey().GetID(), collateralScript, setup.coinbaseKey));

    block = std::make_shared<CBlock>(setup.CreateBlock({tx_upreg}, coinbase_pk, chainman.ActiveChainstate()));
    BOOST_REQUIRE(chainman.ProcessNewBlock(block, true, nullptr));
    BOOST_REQUIRE(!DeploymentActiveAfter(tip_index(), chainman.GetConsensus(), Consensus::DEPLOYMENT_V19));
    ++nHeight;
    BOOST_CHECK_EQUAL(tip_height(), nHeight);
    sync_dmn_tip();
    dmnman.DoMaintenance();
    tip_list = dmnman.GetListAtChainTip();
    BOOST_REQUIRE(tip_list.HasMN(tx_reg_hash));
    diffs.push_back(base_list.BuildDiff(tip_list));

    // spend
    CMutableTransaction tx_spend;
    COutPoint collateralOutpoint(tx_reg_hash, 0);
    tx_spend.vin.emplace_back(collateralOutpoint);
    tx_spend.vout.emplace_back(999.99 * COIN, collateralScript);

    FillableSigningProvider signing_provider;
    signing_provider.AddKeyPubKey(collateral_key, collateral_key.GetPubKey());
    BOOST_REQUIRE(SignSignature(signing_provider, CTransaction(tx_reg), tx_spend, 0, SIGHASH_ALL));
    block = std::make_shared<CBlock>(setup.CreateBlock({tx_spend}, coinbase_pk, chainman.ActiveChainstate()));
    BOOST_REQUIRE(chainman.ProcessNewBlock(block, true, nullptr));
    BOOST_REQUIRE(!DeploymentActiveAfter(tip_index(), chainman.GetConsensus(), Consensus::DEPLOYMENT_V19));
    ++nHeight;
    BOOST_CHECK_EQUAL(tip_height(), nHeight);
    sync_dmn_tip();
    dmnman.DoMaintenance();
    diffs.push_back(tip_list.BuildDiff(dmnman.GetListAtChainTip()));
    tip_list = dmnman.GetListAtChainTip();
    BOOST_REQUIRE(!tip_list.HasMN(tx_reg_hash));
    BOOST_REQUIRE(dmnman.GetListForBlock(pindex_create).HasMN(tx_reg_hash));

    // mine another block so that it's not the last one before V19
    setup.CreateAndProcessBlock({}, coinbase_pk);
    BOOST_REQUIRE(!DeploymentActiveAfter(tip_index(), chainman.GetConsensus(), Consensus::DEPLOYMENT_V19));
    ++nHeight;
    BOOST_CHECK_EQUAL(tip_height(), nHeight);
    sync_dmn_tip();
    dmnman.DoMaintenance();
    diffs.push_back(tip_list.BuildDiff(dmnman.GetListAtChainTip()));
    tip_list = dmnman.GetListAtChainTip();
    BOOST_REQUIRE(!tip_list.HasMN(tx_reg_hash));
    BOOST_REQUIRE(dmnman.GetListForBlock(pindex_create).HasMN(tx_reg_hash));

    // this block should activate V19
    setup.CreateAndProcessBlock({}, coinbase_pk);
    BOOST_REQUIRE(DeploymentActiveAfter(tip_index(), chainman.GetConsensus(), Consensus::DEPLOYMENT_V19));
    ++nHeight;
    BOOST_CHECK_EQUAL(tip_height(), nHeight);
    sync_dmn_tip();
    dmnman.DoMaintenance();
    diffs.push_back(tip_list.BuildDiff(dmnman.GetListAtChainTip()));
    tip_list = dmnman.GetListAtChainTip();
    BOOST_REQUIRE(!tip_list.HasMN(tx_reg_hash));
    BOOST_REQUIRE(dmnman.GetListForBlock(pindex_create).HasMN(tx_reg_hash));

    // check mn list/diff
    CDeterministicMNListDiff dummy_diff = base_list.BuildDiff(tip_list);
    CDeterministicMNList dummy_list{base_list};
    dummy_list.ApplyDiff(tip_index(), dummy_diff);
    // Lists should match
    BOOST_REQUIRE(dummy_list == tip_list);

    // mine 10 more blocks
    for (int i = 0; i < 10; ++i)
    {
        setup.CreateAndProcessBlock({}, coinbase_pk);
        BOOST_REQUIRE(
            DeploymentActiveAfter(tip_index(), chainman.GetConsensus(), Consensus::DEPLOYMENT_V19));
        BOOST_CHECK_EQUAL(tip_height(), nHeight + 1 + i);
        sync_dmn_tip();
        dmnman.DoMaintenance();
        diffs.push_back(tip_list.BuildDiff(dmnman.GetListAtChainTip()));
        tip_list = dmnman.GetListAtChainTip();
        BOOST_REQUIRE(!tip_list.HasMN(tx_reg_hash));
        BOOST_REQUIRE(dmnman.GetListForBlock(pindex_create).HasMN(tx_reg_hash));
    }

    // check mn list/diff
    const CBlockIndex* v19_index = WITH_LOCK(::cs_main, return chainman.ActiveChain().Tip()->GetAncestor(Params().GetConsensus().V19Height));
    auto v19_list = dmnman.GetListForBlock(v19_index);
    dummy_diff = v19_list.BuildDiff(tip_list);
    dummy_list = v19_list;
    dummy_list.ApplyDiff(tip_index(), dummy_diff);
    BOOST_REQUIRE(dummy_list == tip_list);

    // NOTE: this fails on v19/v19.1 with errors like:
    // "RemoveMN: Can't delete a masternode ... with a pubKeyOperator=..."
    dummy_diff = base_list.BuildDiff(tip_list);
    dummy_list = base_list;
    dummy_list.ApplyDiff(tip_index(), dummy_diff);
    BOOST_REQUIRE(dummy_list == tip_list);

    dummy_list = base_list;
    for (const auto& diff : diffs) {
        dummy_list.ApplyDiff(tip_index(), diff);
    }
    BOOST_REQUIRE(dummy_list == tip_list);
};

void FuncProUpRegTxVersionHandlingBeforeV24(TestChainSetup& setup)
{
    auto& chainman = *Assert(setup.m_node.chainman.get());
    auto& dmnman = *Assert(setup.m_node.dmnman);
    auto tip_index    = [&] { return WITH_LOCK(::cs_main, return chainman.ActiveChain().Tip()); };
    auto sync_dmn_tip = [&] { dmnman.UpdatedBlockTip(tip_index()); };
    const CScript coinbase_pk = GetScriptForRawPubKey(setup.coinbaseKey.GetPubKey());

    BOOST_REQUIRE(!DeploymentActiveAfter(tip_index(), chainman.GetConsensus(), Consensus::DEPLOYMENT_V19));
    BOOST_REQUIRE(bls::bls_legacy_scheme.load());

    auto utxos = BuildSimpleUtxoMap(setup.m_coinbase_txns);
    CKey owner_key;
    CBLSSecretKey operator_key;
    auto tx_reg = WITH_LOCK(::cs_main, return CreateProRegTx(chainman.ActiveChain(), *(setup.m_node.mempool), utxos, 1, GenerateRandomAddress(),
                                 setup.coinbaseKey, owner_key, operator_key));
    const auto proTxHash = tx_reg.GetHash();
    setup.CreateAndProcessBlock({tx_reg}, coinbase_pk);
    sync_dmn_tip();

    auto dmn = dmnman.GetListAtChainTip().GetMN(proTxHash);
    BOOST_REQUIRE(dmn);
    BOOST_CHECK_EQUAL(dmn->pdmnState->nVersion, ProTxVersion::LegacyBLS);

    while (!DeploymentActiveAfter(tip_index(), chainman.GetConsensus(), Consensus::DEPLOYMENT_V19)) {
        setup.CreateAndProcessBlock({}, coinbase_pk);
        sync_dmn_tip();
    }
    BOOST_REQUIRE(!DeploymentActiveAfter(tip_index(), chainman, Consensus::DEPLOYMENT_V24));
    BOOST_REQUIRE(!bls::bls_legacy_scheme.load());

    const auto payoutScript = GenerateRandomAddress();
    auto tx_upreg = WITH_LOCK(::cs_main, return CreateProUpRegTx(chainman.ActiveChain(), *(setup.m_node.mempool), utxos, proTxHash, owner_key,
                                     operator_key.GetPublicKey(), owner_key.GetPubKey().GetID(), payoutScript,
                                     setup.coinbaseKey));
    const auto opt_upreg = GetTxPayload<CProUpRegTx>(tx_upreg);
    BOOST_REQUIRE(opt_upreg);
    BOOST_CHECK_EQUAL(opt_upreg->nVersion, ProTxVersion::BasicBLS);

    setup.CreateAndProcessBlock({tx_upreg}, coinbase_pk);
    sync_dmn_tip();

    dmn = dmnman.GetListAtChainTip().GetMN(proTxHash);
    BOOST_REQUIRE(dmn);
    BOOST_CHECK(dmn->pdmnState->scriptPayout == payoutScript);
    BOOST_CHECK_EQUAL(dmn->pdmnState->nVersion, ProTxVersion::LegacyBLS);
    BOOST_REQUIRE(!dmn->pdmnState->IsBanned());

    CBLSSecretKey operator_key_new;
    operator_key_new.MakeNewKey();
    const auto payoutScript2 = GenerateRandomAddress();
    auto tx_upreg2 = WITH_LOCK(::cs_main, return CreateProUpRegTx(chainman.ActiveChain(), *(setup.m_node.mempool), utxos, proTxHash, owner_key,
                                      operator_key_new.GetPublicKey(), owner_key.GetPubKey().GetID(), payoutScript2,
                                      setup.coinbaseKey));
    setup.CreateAndProcessBlock({tx_upreg2}, coinbase_pk);
    sync_dmn_tip();

    dmn = dmnman.GetListAtChainTip().GetMN(proTxHash);
    BOOST_REQUIRE(dmn);
    BOOST_CHECK(dmn->pdmnState->scriptPayout == payoutScript2);
    BOOST_CHECK_EQUAL(dmn->pdmnState->nVersion, ProTxVersion::BasicBLS);
    BOOST_CHECK(!dmn->pdmnState->pubKeyOperator.IsLegacy());
    BOOST_REQUIRE(dmn->pdmnState->IsBanned());

    CBLSSecretKey operator_key_legacy;
    operator_key_legacy.MakeNewKey();
    const auto payoutScript3 = GenerateRandomAddress();
    CProUpRegTx proTxLegacy;
    proTxLegacy.nVersion = ProTxVersion::LegacyBLS;
    proTxLegacy.proTxHash = proTxHash;
    proTxLegacy.pubKeyOperator.Set(operator_key_legacy.GetPublicKey(), /*legacy=*/true);
    proTxLegacy.keyIDVoting = owner_key.GetPubKey().GetID();
    proTxLegacy.scriptPayout = payoutScript3;

    CMutableTransaction tx_upreg3;
    tx_upreg3.nVersion = 3;
    tx_upreg3.nType = TRANSACTION_PROVIDER_UPDATE_REGISTRAR;
    WITH_LOCK(::cs_main, FundTransaction(chainman.ActiveChain(), tx_upreg3, utxos, GetScriptForDestination(PKHash(setup.coinbaseKey.GetPubKey())),
                    1 * COIN, setup.coinbaseKey));
    proTxLegacy.inputsHash = CalcTxInputsHash(CTransaction(tx_upreg3));
    CHashSigner::SignHash(::SerializeHash(proTxLegacy), owner_key, proTxLegacy.vchSig);
    SetTxPayload(tx_upreg3, proTxLegacy);
    SignTransaction(*(setup.m_node.mempool), tx_upreg3, setup.coinbaseKey);

    setup.CreateAndProcessBlock({tx_upreg3}, coinbase_pk);
    sync_dmn_tip();

    dmn = dmnman.GetListAtChainTip().GetMN(proTxHash);
    BOOST_REQUIRE(dmn);
    BOOST_CHECK(dmn->pdmnState->scriptPayout == payoutScript3);
    // Pre-v24, an operator-changing registrar update adopts the tx version even when that lowers the
    // stored version: the v1 update downgrades the state to LegacyBLS and re-encodes the new operator
    // key with the legacy scheme. This must match already-deployed consensus; version-bump hardening
    // only applies once v24 is active.
    BOOST_CHECK_EQUAL(dmn->pdmnState->nVersion, ProTxVersion::LegacyBLS);
    BOOST_CHECK(dmn->pdmnState->pubKeyOperator.IsLegacy());
    BOOST_CHECK(dmn->pdmnState->pubKeyOperator.Get() == operator_key_legacy.GetPublicKey());
};

void FuncProUpRegTxV4OnLegacyRejected(TestChainSetup& setup)
{
    auto& chainman = *Assert(setup.m_node.chainman.get());
    auto& dmnman = *Assert(setup.m_node.dmnman);
    auto tip_index    = [&] { return WITH_LOCK(::cs_main, return chainman.ActiveChain().Tip()); };
    auto sync_dmn_tip = [&] { dmnman.UpdatedBlockTip(tip_index()); };
    const CScript coinbase_pk = GetScriptForRawPubKey(setup.coinbaseKey.GetPubKey());

    BOOST_REQUIRE(!DeploymentActiveAfter(tip_index(), chainman.GetConsensus(), Consensus::DEPLOYMENT_V19));
    BOOST_REQUIRE(!DeploymentActiveAfter(tip_index(), chainman, Consensus::DEPLOYMENT_V24));

    auto utxos = BuildSimpleUtxoMap(setup.m_coinbase_txns);
    CKey owner_key;
    CBLSSecretKey operator_key;
    auto tx_reg = WITH_LOCK(::cs_main, return CreateProRegTx(chainman.ActiveChain(), *(setup.m_node.mempool), utxos, 1, GenerateRandomAddress(),
                                 setup.coinbaseKey, owner_key, operator_key));
    const auto proTxHash = tx_reg.GetHash();
    setup.CreateAndProcessBlock({tx_reg}, coinbase_pk);
    sync_dmn_tip();

    auto dmn = dmnman.GetListAtChainTip().GetMN(proTxHash);
    BOOST_REQUIRE(dmn);
    BOOST_CHECK_EQUAL(dmn->pdmnState->nVersion, ProTxVersion::LegacyBLS);

    for (int i = 0; i < 2000 && !DeploymentActiveAfter(tip_index(), chainman, Consensus::DEPLOYMENT_V24); ++i) {
        setup.CreateAndProcessBlock({}, coinbase_pk);
        sync_dmn_tip();
    }
    BOOST_REQUIRE(DeploymentActiveAfter(tip_index(), chainman.GetConsensus(), Consensus::DEPLOYMENT_V19));
    BOOST_REQUIRE(DeploymentActiveAfter(tip_index(), chainman, Consensus::DEPLOYMENT_V24));
    BOOST_REQUIRE(!bls::bls_legacy_scheme.load());
    BOOST_REQUIRE_EQUAL(dmnman.GetListAtChainTip().GetMN(proTxHash)->pdmnState->nVersion, ProTxVersion::LegacyBLS);

    CProUpRegTx proTx;
    proTx.nVersion = ProTxVersion::MultiPayout;
    proTx.proTxHash = proTxHash;
    proTx.pubKeyOperator.Set(operator_key.GetPublicKey(), bls::bls_legacy_scheme.load());
    proTx.keyIDVoting = owner_key.GetPubKey().GetID();
    proTx.payouts = {{GenerateRandomAddress(), CMasternodePayoutShare::MAX_REWARD}};

    CMutableTransaction tx;
    tx.nVersion = 3;
    tx.nType = TRANSACTION_PROVIDER_UPDATE_REGISTRAR;
    WITH_LOCK(::cs_main, FundTransaction(chainman.ActiveChain(), tx, utxos, GetScriptForDestination(PKHash(setup.coinbaseKey.GetPubKey())),
                    1 * COIN, setup.coinbaseKey));
    proTx.inputsHash = CalcTxInputsHash(CTransaction(tx));
    CHashSigner::SignHash(::SerializeHash(proTx), owner_key, proTx.vchSig);
    SetTxPayload(tx, proTx);
    SignTransaction(*(setup.m_node.mempool), tx, setup.coinbaseKey);

    TxValidationState val_state;
    {
        LOCK(cs_main);
        BOOST_CHECK(!CheckProUpRegTx(CTransaction(tx), chainman.ActiveChain().Tip(), dmnman,
                                     chainman.ActiveChainstate().CoinsTip(), chainman, val_state, /*check_sigs=*/true));
    }
    BOOST_CHECK_EQUAL(val_state.GetRejectReason(), "bad-protx-version-upgrade");
};

void FuncProUpRegTxV2CannotBypassV4PayoutCollateralReuse(TestChainSetup& setup)
{
    auto& chainman = *Assert(setup.m_node.chainman.get());
    auto& dmnman = *Assert(setup.m_node.dmnman);
    auto tip_index    = [&] { return WITH_LOCK(::cs_main, return chainman.ActiveChain().Tip()); };
    auto sync_dmn_tip = [&] { dmnman.UpdatedBlockTip(tip_index()); };
    const CScript coinbase_pk = GetScriptForRawPubKey(setup.coinbaseKey.GetPubKey());

    for (int i = 0; i < 2000 && !DeploymentActiveAfter(tip_index(), chainman, Consensus::DEPLOYMENT_V24); ++i) {
        setup.CreateAndProcessBlock({}, coinbase_pk);
        sync_dmn_tip();
    }
    BOOST_REQUIRE(DeploymentActiveAfter(tip_index(), chainman.GetConsensus(), Consensus::DEPLOYMENT_V19));
    BOOST_REQUIRE(DeploymentActiveAfter(tip_index(), chainman, Consensus::DEPLOYMENT_V24));
    BOOST_REQUIRE(!bls::bls_legacy_scheme.load());

    auto utxos = BuildSimpleUtxoMap(setup.m_coinbase_txns);
    CKey owner_key;
    CKey collateral_key;
    CBLSSecretKey operator_key;
    owner_key.MakeNewKey(true);
    collateral_key.MakeNewKey(true);
    operator_key.MakeNewKey();

    const auto script_collateral = GetScriptForDestination(PKHash(collateral_key.GetPubKey()));
    const auto script_payout = GenerateRandomAddress();

    CMutableTransaction tx_collateral;
    WITH_LOCK(::cs_main, FundTransaction(chainman.ActiveChain(), tx_collateral, utxos, script_collateral, dmn_types::Regular.collat_amount,
                    setup.coinbaseKey));
    SignTransaction(*(setup.m_node.mempool), tx_collateral, setup.coinbaseKey);
    setup.CreateAndProcessBlock({tx_collateral}, coinbase_pk);
    sync_dmn_tip();

    CProRegTx pro_reg;
    pro_reg.nVersion = ProTxVersion::MultiPayout;
    pro_reg.netInfo = NetInfoInterface::MakeNetInfo(pro_reg.nVersion);
    BOOST_CHECK_EQUAL(pro_reg.netInfo->AddEntry(NetInfoPurpose::CORE_P2P, "1.1.1.1:9999"), NetInfoStatus::Success);
    pro_reg.keyIDOwner = owner_key.GetPubKey().GetID();
    pro_reg.pubKeyOperator.Set(operator_key.GetPublicKey(), bls::bls_legacy_scheme.load());
    pro_reg.keyIDVoting = owner_key.GetPubKey().GetID();
    pro_reg.payouts = {{script_payout, CMasternodePayoutShare::MAX_REWARD}};
    for (size_t i = 0; i < tx_collateral.vout.size(); ++i) {
        if (tx_collateral.vout[i].nValue == dmn_types::Regular.collat_amount) {
            pro_reg.collateralOutpoint = COutPoint(tx_collateral.GetHash(), i);
            break;
        }
    }

    CMutableTransaction tx_reg;
    tx_reg.nVersion = 3;
    tx_reg.nType = TRANSACTION_PROVIDER_REGISTER;
    WITH_LOCK(::cs_main, FundTransaction(chainman.ActiveChain(), tx_reg, utxos, GetScriptForDestination(PKHash(setup.coinbaseKey.GetPubKey())),
                    1 * COIN, setup.coinbaseKey));
    pro_reg.inputsHash = CalcTxInputsHash(CTransaction(tx_reg));
    CMessageSigner::SignMessage(pro_reg.MakeSignString(), pro_reg.vchSig, collateral_key);
    SetTxPayload(tx_reg, pro_reg);
    SignTransaction(*(setup.m_node.mempool), tx_reg, setup.coinbaseKey);
    const auto proTxHash = tx_reg.GetHash();
    setup.CreateAndProcessBlock({tx_reg}, coinbase_pk);
    sync_dmn_tip();

    auto dmn = dmnman.GetListAtChainTip().GetMN(proTxHash);
    BOOST_REQUIRE(dmn);
    BOOST_CHECK_EQUAL(dmn->pdmnState->nVersion, ProTxVersion::MultiPayout);
    BOOST_CHECK_EQUAL(dmn->pdmnState->payouts.size(), 1U);
    BOOST_CHECK(dmn->pdmnState->payouts.front().scriptPayout == script_payout);

    CProUpRegTx pro_upreg;
    pro_upreg.nVersion = ProTxVersion::BasicBLS;
    pro_upreg.proTxHash = proTxHash;
    pro_upreg.pubKeyOperator.Set(operator_key.GetPublicKey(), bls::bls_legacy_scheme.load());
    pro_upreg.keyIDVoting = owner_key.GetPubKey().GetID();
    pro_upreg.scriptPayout = script_collateral;

    CMutableTransaction tx_upreg;
    tx_upreg.nVersion = 3;
    tx_upreg.nType = TRANSACTION_PROVIDER_UPDATE_REGISTRAR;
    WITH_LOCK(::cs_main, FundTransaction(chainman.ActiveChain(), tx_upreg, utxos, GetScriptForDestination(PKHash(setup.coinbaseKey.GetPubKey())),
                    1 * COIN, setup.coinbaseKey));
    pro_upreg.inputsHash = CalcTxInputsHash(CTransaction(tx_upreg));
    CHashSigner::SignHash(::SerializeHash(pro_upreg), owner_key, pro_upreg.vchSig);
    SetTxPayload(tx_upreg, pro_upreg);
    SignTransaction(*(setup.m_node.mempool), tx_upreg, setup.coinbaseKey);

    TxValidationState val_state;
    {
        LOCK(cs_main);
        BOOST_CHECK(!CheckProUpRegTx(CTransaction(tx_upreg), chainman.ActiveChain().Tip(), dmnman,
                                     chainman.ActiveChainstate().CoinsTip(), chainman, val_state, /*check_sigs=*/true));
    }
    BOOST_CHECK_EQUAL(val_state.GetRejectReason(), "bad-protx-payee-reuse");
}

// Coinbase validation matches each expected masternode payment against a DISTINCT coinbase output
// only after v24. This exercises the exact activation boundary with a SINGLE masternode whose
// owner payout and operator payout resolve to the same script AND amount (an even 50% operator
// reward), so the coinbase carries two identical outputs. The "merge cheat" folds those two
// identical outputs into one, redirecting the freed value to a miner output so the total coinbase
// value is unchanged; this underpays the masternode.
//
//   Pre-v24:  existence-only matching ACCEPTS the merge cheat.
//   Post-v24: strict multiplicity matching REJECTS it (and the faithful block connects).
//
// The duplicate is NOT a multi-payout (v4) phenomenon: multi-payout owner shares are forced to
// distinct scripts (bad-protx-payee-dup) and can never collide. It is a plain owner-payout ==
// operator-payout script collision, which every ProTx version supports -- here a v2 (BasicBLS)
// MN, the max version eligible while v24 is still pending, kept across the boundary.
void FuncMNPaymentMultiplicityV24Boundary(TestChainSetup& setup)
{
    auto& chainman = *Assert(setup.m_node.chainman.get());
    auto& dmnman = *Assert(setup.m_node.dmnman);
    auto tip_index    = [&] { return WITH_LOCK(::cs_main, return chainman.ActiveChain().Tip()); };
    auto tip_hash     = [&] { return WITH_LOCK(::cs_main, return chainman.ActiveChain().Tip()->GetBlockHash()); };
    auto sync_dmn_tip = [&] { dmnman.UpdatedBlockTip(tip_index()); };
    const CScript coinbase_pk = GetScriptForRawPubKey(setup.coinbaseKey.GetPubKey());
    const auto& consensus = chainman.GetConsensus();

    // Start in the pre-v24 window: v19 active (basic BLS scheme) so we can register a v2 MN, but
    // v24 -- and thus strict multiplicity matching -- not yet active.
    BOOST_REQUIRE(DeploymentActiveAfter(tip_index(), consensus, Consensus::DEPLOYMENT_V19));
    BOOST_REQUIRE(!DeploymentActiveAfter(tip_index(), chainman, Consensus::DEPLOYMENT_V24));
    BOOST_REQUIRE(!bls::bls_legacy_scheme.load());

    auto utxos = BuildSimpleUtxoMap(setup.m_coinbase_txns);

    // A single script that BOTH an owner payout and the operator payout will point at.
    CKey payout_key;
    payout_key.MakeNewKey(true);
    const CScript shared_script = GetScriptForDestination(PKHash(payout_key.GetPubKey()));

    CKey owner_key;
    CKey collateral_key;
    CBLSSecretKey operator_key;
    owner_key.MakeNewKey(true);
    collateral_key.MakeNewKey(true);
    operator_key.MakeNewKey();
    const auto script_collateral = GetScriptForDestination(PKHash(collateral_key.GetPubKey()));

    // Fund the collateral.
    CMutableTransaction tx_collateral;
    WITH_LOCK(::cs_main, FundTransaction(chainman.ActiveChain(), tx_collateral, utxos, script_collateral, dmn_types::Regular.collat_amount,
                    setup.coinbaseKey));
    SignTransaction(*(setup.m_node.mempool), tx_collateral, setup.coinbaseKey);
    setup.CreateAndProcessBlock({tx_collateral}, coinbase_pk);
    sync_dmn_tip();

    // Register a plain v2 (BasicBLS) MN -- deliberately NOT multi-payout, and the max version
    // eligible while v24 is pending -- with a single legacy owner payout -> shared_script and a
    // 50% operator reward, so the owner share and the operator share are equal whenever the
    // masternode reward is even.
    CProRegTx pro_reg;
    pro_reg.nVersion = ProTxVersion::BasicBLS;
    pro_reg.netInfo = NetInfoInterface::MakeNetInfo(pro_reg.nVersion);
    // Legacy MnNetInfo (v2) on regtest must not use the mainnet port; a small port works.
    BOOST_CHECK_EQUAL(pro_reg.netInfo->AddEntry(NetInfoPurpose::CORE_P2P, "1.1.1.1:1"), NetInfoStatus::Success);
    pro_reg.keyIDOwner = owner_key.GetPubKey().GetID();
    pro_reg.pubKeyOperator.Set(operator_key.GetPublicKey(), bls::bls_legacy_scheme.load());
    pro_reg.keyIDVoting = owner_key.GetPubKey().GetID();
    pro_reg.nOperatorReward = 5000; // 50%
    pro_reg.scriptPayout = shared_script;
    for (size_t i = 0; i < tx_collateral.vout.size(); ++i) {
        if (tx_collateral.vout[i].nValue == dmn_types::Regular.collat_amount) {
            pro_reg.collateralOutpoint = COutPoint(tx_collateral.GetHash(), i);
            break;
        }
    }

    CMutableTransaction tx_reg;
    tx_reg.nVersion = 3;
    tx_reg.nType = TRANSACTION_PROVIDER_REGISTER;
    WITH_LOCK(::cs_main, FundTransaction(chainman.ActiveChain(), tx_reg, utxos, GetScriptForDestination(PKHash(setup.coinbaseKey.GetPubKey())),
                    1 * COIN, setup.coinbaseKey));
    pro_reg.inputsHash = CalcTxInputsHash(CTransaction(tx_reg));
    CMessageSigner::SignMessage(pro_reg.MakeSignString(), pro_reg.vchSig, collateral_key);
    SetTxPayload(tx_reg, pro_reg);
    SignTransaction(*(setup.m_node.mempool), tx_reg, setup.coinbaseKey);
    const auto proTxHash = tx_reg.GetHash();
    setup.CreateAndProcessBlock({tx_reg}, coinbase_pk);
    sync_dmn_tip();

    auto dmn = dmnman.GetListAtChainTip().GetMN(proTxHash);
    BOOST_REQUIRE(dmn);
    BOOST_CHECK_EQUAL(dmn->pdmnState->nVersion, ProTxVersion::BasicBLS);

    // Point the operator payout at the same script as the owner payout. Nothing checks the
    // operator payout script against the owner payout script, in any version, so this collision
    // is accepted.
    CProUpServTx pro_ups;
    pro_ups.nVersion = ProTxVersion::BasicBLS;
    pro_ups.proTxHash = proTxHash;
    pro_ups.netInfo = NetInfoInterface::MakeNetInfo(pro_ups.nVersion);
    BOOST_CHECK_EQUAL(pro_ups.netInfo->AddEntry(NetInfoPurpose::CORE_P2P, "1.1.1.1:1"), NetInfoStatus::Success);
    pro_ups.scriptOperatorPayout = shared_script;

    CMutableTransaction tx_ups;
    tx_ups.nVersion = 3;
    tx_ups.nType = TRANSACTION_PROVIDER_UPDATE_SERVICE;
    WITH_LOCK(::cs_main, FundTransaction(chainman.ActiveChain(), tx_ups, utxos, GetScriptForDestination(PKHash(setup.coinbaseKey.GetPubKey())),
                    1 * COIN, setup.coinbaseKey));
    pro_ups.inputsHash = CalcTxInputsHash(CTransaction(tx_ups));
    pro_ups.sig = operator_key.Sign(::SerializeHash(pro_ups), bls::bls_legacy_scheme);
    SetTxPayload(tx_ups, pro_ups);
    SignTransaction(*(setup.m_node.mempool), tx_ups, setup.coinbaseKey);
    setup.CreateAndProcessBlock({tx_ups}, coinbase_pk);
    sync_dmn_tip();
    BOOST_REQUIRE(dmnman.GetListAtChainTip().GetMN(proTxHash)->pdmnState->scriptOperatorPayout == shared_script);

    // Two identical coinbase outputs paying shared_script (the owner/operator collision).
    auto find_duplicate = [&](const CBlock& b) -> std::optional<CTxOut> {
        const auto& vout = b.vtx[0]->vout;
        for (size_t i = 0; i < vout.size(); ++i) {
            if (vout[i].scriptPubKey != shared_script) continue;
            for (size_t j = i + 1; j < vout.size(); ++j) {
                if (vout[j] == vout[i]) return vout[i];
            }
        }
        return std::nullopt;
    };

    // Mine until a block's (pre-operator) masternode reward is even: only then does the 50%
    // operator share equal the single owner share, so the owner and operator outputs -- both
    // paying shared_script -- collide into two identical coinbase outputs. The reward is constant
    // within a subsidy/reallocation epoch, so this can take up to a full epoch; intermediate
    // blocks are processed to advance the chain.
    auto mine_until_duplicate = [&]() -> std::pair<CBlock, CTxOut> {
        for (int i = 0; i < 2000; ++i) {
            CBlock good = setup.CreateBlock({}, coinbase_pk, chainman.ActiveChainstate());
            if (auto dup = find_duplicate(good)) return {good, *dup};
            BOOST_REQUIRE(chainman.ProcessNewBlock(std::make_shared<CBlock>(good), /*force_processing=*/true, nullptr));
            sync_dmn_tip();
        }
        BOOST_REQUIRE_MESSAGE(false, "expected owner/operator collision to yield a duplicate coinbase output");
        return {}; // unreachable, BOOST_REQUIRE above aborts the test
    };

    // Build the "merge cheat" sibling of `good`: drop one of the two identical outputs and fold
    // its value into a non-masternode (miner) output. Total coinbase value is unchanged (so
    // IsBlockValueValid still passes), but only ONE distinct output now pays shared_script.
    auto build_merge_cheat = [&](const CBlock& good, const CTxOut& dup) -> CBlock {
        CBlock merged = good;
        CMutableTransaction cb(*merged.vtx[0]);
        bool kept_first = false;
        for (auto it = cb.vout.begin(); it != cb.vout.end();) {
            if (*it == dup && kept_first) {
                it = cb.vout.erase(it);
            } else {
                if (*it == dup) kept_first = true;
                ++it;
            }
        }
        bool redirected = false;
        for (auto& o : cb.vout) {
            if (o.scriptPubKey != shared_script && !o.scriptPubKey.IsUnspendable()) {
                o.nValue += dup.nValue;
                redirected = true;
                break;
            }
        }
        BOOST_REQUIRE(redirected);
        merged.vtx[0] = MakeTransactionRef(std::move(cb));
        merged.hashMerkleRoot = BlockMerkleRoot(merged);
        merged.nNonce = 0;
        while (!CheckProofOfWork(merged.GetHash(), merged.nBits, consensus)) ++merged.nNonce;
        return merged;
    };

    // ---- Pre-v24: legacy existence-only matching ACCEPTS the merge cheat (masternode underpaid).
    {
        const auto [good, dup] = mine_until_duplicate();
        // The collision must be reached while v24 is still pending, otherwise this phase would be
        // testing post-v24 behaviour by accident.
        BOOST_REQUIRE(!DeploymentActiveAfter(tip_index(), chainman, Consensus::DEPLOYMENT_V24));
        const CBlock merged = build_merge_cheat(good, dup);
        BOOST_REQUIRE(chainman.ProcessNewBlock(std::make_shared<CBlock>(merged), /*force_processing=*/true, nullptr));
        BOOST_CHECK_EQUAL(tip_hash(), merged.GetHash());
        sync_dmn_tip();
    }

    // ---- Mine across v24 activation (the same v2 MN is kept; its collision is version-independent).
    for (int i = 0; i < 2000 && !DeploymentActiveAfter(tip_index(), chainman, Consensus::DEPLOYMENT_V24); ++i) {
        setup.CreateAndProcessBlock({}, coinbase_pk);
        sync_dmn_tip();
    }
    BOOST_REQUIRE(DeploymentActiveAfter(tip_index(), chainman, Consensus::DEPLOYMENT_V24));

    // ---- Post-v24: strict multiplicity matching REJECTS the same merge (two expected outputs, one
    // distinct actual). Rejection is observed as the tip not advancing.
    {
        const auto [good, dup] = mine_until_duplicate();
        const CBlock merged = build_merge_cheat(good, dup);
        const uint256 tip_before = tip_hash();
        chainman.ProcessNewBlock(std::make_shared<CBlock>(merged), /*force_processing=*/true, nullptr);
        BOOST_CHECK(tip_hash() == tip_before);
        BOOST_CHECK(tip_hash() != merged.GetHash());

        // The faithful block (both identical outputs present) connects.
        BOOST_REQUIRE(chainman.ProcessNewBlock(std::make_shared<CBlock>(good), /*force_processing=*/true, nullptr));
        BOOST_CHECK_EQUAL(tip_hash(), good.GetHash());
    }
}

static std::shared_ptr<NetInfoInterface> DeserializeCoreP2PExtNetInfo(const std::vector<NetInfoEntry>& entries)
{
    // Hand-roll ExtNetInfo's serialization (version byte followed by a map of purpose to entries)
    // to construct states that AddEntry() would refuse to produce
    CDataStream ds(SER_DISK, CLIENT_VERSION);
    ds << uint8_t{1};                      // m_version (ExtNetInfo::CURRENT_VERSION)
    WriteCompactSize(ds, 1);               // m_data map size (one purpose)
    ds << NetInfoPurpose::CORE_P2P;        // map key (purpose)
    WriteCompactSize(ds, entries.size());  // entry count for this purpose
    for (const auto& entry : entries) {
        ds << entry;
    }

    auto net_info{std::make_shared<ExtNetInfo>()};
    ds >> *net_info;
    return net_info;
}

static CTransaction BuildExtNetInfoProRegTx(std::shared_ptr<NetInfoInterface> net_info)
{
    CKey owner_key;
    owner_key.MakeNewKey(true);
    CBLSSecretKey operator_key;
    operator_key.MakeNewKey();

    CProRegTx pro_reg;
    pro_reg.nVersion = ProTxVersion::ExtAddr;
    pro_reg.netInfo = std::move(net_info);
    pro_reg.keyIDOwner = owner_key.GetPubKey().GetID();
    pro_reg.pubKeyOperator.Set(operator_key.GetPublicKey(), bls::bls_legacy_scheme.load());
    pro_reg.keyIDVoting = owner_key.GetPubKey().GetID();
    pro_reg.scriptPayout = GenerateRandomAddress();

    CMutableTransaction tx;
    tx.nVersion = 3;
    tx.nType = TRANSACTION_PROVIDER_REGISTER;
    pro_reg.inputsHash = CalcTxInputsHash(CTransaction(tx));
    SetTxPayload(tx, pro_reg);
    return CTransaction(tx);
}

void FuncProRegTxRejectsInvalidDeserializedExtNetInfo(TestChainSetup& setup)
{
    auto& chainman = *Assert(setup.m_node.chainman.get());
    auto& dmnman = *Assert(setup.m_node.dmnman);
    auto tip_index    = [&] { return WITH_LOCK(::cs_main, return chainman.ActiveChain().Tip()); };
    auto sync_dmn_tip = [&] { dmnman.UpdatedBlockTip(tip_index()); };
    const CScript coinbase_pk = GetScriptForRawPubKey(setup.coinbaseKey.GetPubKey());

    for (int i = 0; i < 2000 && !DeploymentActiveAfter(tip_index(), chainman, Consensus::DEPLOYMENT_V24); ++i) {
        setup.CreateAndProcessBlock({}, coinbase_pk);
        sync_dmn_tip();
    }
    BOOST_REQUIRE(DeploymentActiveAfter(tip_index(), chainman.GetConsensus(), Consensus::DEPLOYMENT_V19));
    BOOST_REQUIRE(DeploymentActiveAfter(tip_index(), chainman, Consensus::DEPLOYMENT_V24));
    BOOST_REQUIRE(!bls::bls_legacy_scheme.load());

    auto check_reject_reason = [&](std::shared_ptr<NetInfoInterface> net_info, const std::string& reject_reason) {
        TxValidationState state;
        {
            LOCK(cs_main);
            BOOST_CHECK(!CheckProRegTx(BuildExtNetInfoProRegTx(std::move(net_info)), chainman.ActiveChain().Tip(),
                                       dmnman, chainman.ActiveChainstate().CoinsTip(), chainman, state,
                                       /*check_sigs=*/false));
        }
        BOOST_CHECK_EQUAL(state.GetResult(), TxValidationResult::TX_BAD_SPECIAL);
        BOOST_CHECK_EQUAL(state.GetRejectReason(), reject_reason);
    };

    const uint16_t port{9998};
    const NetInfoEntry p2p_entry{LookupNumeric("1.1.1.1", port)};
    BOOST_REQUIRE(p2p_entry.IsTriviallyValid());
    check_reject_reason(DeserializeCoreP2PExtNetInfo({p2p_entry, p2p_entry}), "bad-protx-dup-netinfo-entry");

    std::vector<NetInfoEntry> too_many_entries;
    for (size_t i = 1; i <= MAX_ENTRIES_EXTNETINFO + 1; ++i) {
        const NetInfoEntry entry{LookupNumeric(strprintf("1.1.1.%d", i), port)};
        BOOST_REQUIRE(entry.IsTriviallyValid());
        too_many_entries.emplace_back(entry);
    }
    check_reject_reason(DeserializeCoreP2PExtNetInfo(too_many_entries), "bad-protx-netinfo-maxlimit");

    DomainPort domain;
    BOOST_REQUIRE_EQUAL(domain.Set("example.com", 443), DomainPort::Status::Success);
    const NetInfoEntry domain_entry{domain};
    BOOST_REQUIRE(domain_entry.IsTriviallyValid());
    check_reject_reason(DeserializeCoreP2PExtNetInfo({domain_entry}), "bad-protx-netinfo-entry");
}

void FuncDIP3Protx(TestChainSetup& setup)
{
    auto& chainman = *Assert(setup.m_node.chainman.get());
    auto& dmnman = *Assert(setup.m_node.dmnman);
    auto tip_index    = [&] { return WITH_LOCK(::cs_main, return chainman.ActiveChain().Tip()); };
    auto tip_height   = [&] { return WITH_LOCK(::cs_main, return chainman.ActiveChain().Height()); };
    auto sync_dmn_tip = [&] { dmnman.UpdatedBlockTip(tip_index()); };

    auto utxos = BuildSimpleUtxoMap(setup.m_coinbase_txns);

    const CScript coinbase_pk = GetScriptForRawPubKey(setup.coinbaseKey.GetPubKey());
    int nHeight = tip_height();
    int port = 1;

    std::vector<uint256> dmnHashes;
    std::map<uint256, CKey> ownerKeys;
    std::map<uint256, CBLSSecretKey> operatorKeys;

    // register one MN per block
    for (size_t i = 0; i < 6; i++) {
        CKey ownerKey;
        CBLSSecretKey operatorKey;
        auto tx = WITH_LOCK(::cs_main, return CreateProRegTx(chainman.ActiveChain(), *(setup.m_node.mempool), utxos, port++, GenerateRandomAddress(), setup.coinbaseKey, ownerKey, operatorKey));
        dmnHashes.emplace_back(tx.GetHash());
        ownerKeys.emplace(tx.GetHash(), ownerKey);
        operatorKeys.emplace(tx.GetHash(), operatorKey);

        // also verify that payloads are not malleable after they have been signed
        // the form of ProRegTx we use here is one with a collateral included, so there is no signature inside the
        // payload itself. This means, we need to rely on script verification, which takes the hash of the extra payload
        // into account
        auto tx2 = MalleateProTxPayout<CProRegTx>(tx);
        TxValidationState dummy_state;
        // Technically, the payload is still valid...
        {
            LOCK(cs_main);
            BOOST_REQUIRE(CheckProRegTx(CTransaction(tx), chainman.ActiveChain().Tip(), dmnman,
                                        chainman.ActiveChainstate().CoinsTip(), chainman, dummy_state, true));
            BOOST_REQUIRE(CheckProRegTx(CTransaction(tx2), chainman.ActiveChain().Tip(), dmnman,
                                        chainman.ActiveChainstate().CoinsTip(), chainman, dummy_state, true));
        }
        // But the signature should not verify anymore
        BOOST_REQUIRE(CheckTransactionSignature(*(setup.m_node.mempool), tx));
        BOOST_REQUIRE(!CheckTransactionSignature(*(setup.m_node.mempool), tx2));

        setup.CreateAndProcessBlock({tx}, coinbase_pk);
        sync_dmn_tip();

        BOOST_CHECK_EQUAL(tip_height(), nHeight + 1);
        BOOST_REQUIRE(dmnman.GetListAtChainTip().HasMN(tx.GetHash()));

        nHeight++;
    }

    int DIP0003EnforcementHeightBackup = Params().GetConsensus().DIP0003EnforcementHeight;
    const_cast<Consensus::Params&>(Params().GetConsensus()).DIP0003EnforcementHeight = tip_height() + 1;
    setup.CreateAndProcessBlock({}, coinbase_pk);
    sync_dmn_tip();
    nHeight++;

    // check MN reward payments
    for (size_t i = 0; i < 20; i++) {
        auto dmnExpectedPayee = dmnman.GetListAtChainTip().GetMNPayee(tip_index());
        BOOST_ASSERT(dmnExpectedPayee);

        CBlock block = setup.CreateAndProcessBlock({}, coinbase_pk);
        sync_dmn_tip();
        BOOST_REQUIRE(!block.vtx.empty());

        auto dmnPayout = FindPayoutDmn(dmnman, block);
        BOOST_REQUIRE(dmnPayout != nullptr);
        BOOST_CHECK_EQUAL(dmnPayout->proTxHash.ToString(), dmnExpectedPayee->proTxHash.ToString());

        nHeight++;
    }

    // register multiple MNs per block
    for (size_t i = 0; i < 3; i++) {
        std::vector<CMutableTransaction> txns;
        for (size_t j = 0; j < 3; j++) {
            CKey ownerKey;
            CBLSSecretKey operatorKey;
            auto tx = WITH_LOCK(::cs_main, return CreateProRegTx(chainman.ActiveChain(), *(setup.m_node.mempool), utxos, port++, GenerateRandomAddress(), setup.coinbaseKey, ownerKey, operatorKey));
            dmnHashes.emplace_back(tx.GetHash());
            ownerKeys.emplace(tx.GetHash(), ownerKey);
            operatorKeys.emplace(tx.GetHash(), operatorKey);
            txns.emplace_back(tx);
        }
        setup.CreateAndProcessBlock(txns, coinbase_pk);
        sync_dmn_tip();
        BOOST_CHECK_EQUAL(tip_height(), nHeight + 1);

        for (size_t j = 0; j < 3; j++) {
            BOOST_REQUIRE(dmnman.GetListAtChainTip().HasMN(txns[j].GetHash()));
        }

        nHeight++;
    }

    // test ProUpServTx
    auto tx = WITH_LOCK(::cs_main, return CreateProUpServTx(chainman.ActiveChain(), *(setup.m_node.mempool), utxos, dmnHashes[0], operatorKeys[dmnHashes[0]], 1000, CScript(), setup.coinbaseKey));
    setup.CreateAndProcessBlock({tx}, coinbase_pk);
    sync_dmn_tip();
    BOOST_CHECK_EQUAL(tip_height(), nHeight + 1);
    nHeight++;

    auto dmn = dmnman.GetListAtChainTip().GetMN(dmnHashes[0]);
    BOOST_REQUIRE(dmn != nullptr && dmn->pdmnState->netInfo->GetPrimary().GetPort() == 1000);

    // test ProUpRevTx
    tx = WITH_LOCK(::cs_main, return CreateProUpRevTx(chainman.ActiveChain(), *(setup.m_node.mempool), utxos, dmnHashes[0], operatorKeys[dmnHashes[0]], setup.coinbaseKey));
    setup.CreateAndProcessBlock({tx}, coinbase_pk);
    sync_dmn_tip();
    BOOST_CHECK_EQUAL(tip_height(), nHeight + 1);
    nHeight++;

    dmn = dmnman.GetListAtChainTip().GetMN(dmnHashes[0]);
    BOOST_REQUIRE(dmn != nullptr && dmn->pdmnState->GetBannedHeight() == nHeight);

    // test that the revoked MN does not get paid anymore
    for (size_t i = 0; i < 20; i++) {
        auto dmnExpectedPayee = dmnman.GetListAtChainTip().GetMNPayee(tip_index());
        BOOST_REQUIRE(dmnExpectedPayee && dmnExpectedPayee->proTxHash != dmnHashes[0]);

        CBlock block = setup.CreateAndProcessBlock({}, coinbase_pk);
        sync_dmn_tip();
        BOOST_REQUIRE(!block.vtx.empty());

        auto dmnPayout = FindPayoutDmn(dmnman, block);
        BOOST_REQUIRE(dmnPayout != nullptr);
        BOOST_CHECK_EQUAL(dmnPayout->proTxHash.ToString(), dmnExpectedPayee->proTxHash.ToString());

        nHeight++;
    }

    // test reviving the MN
    CBLSSecretKey newOperatorKey;
    newOperatorKey.MakeNewKey();
    dmn = dmnman.GetListAtChainTip().GetMN(dmnHashes[0]);
    tx = WITH_LOCK(::cs_main, return CreateProUpRegTx(chainman.ActiveChain(), *(setup.m_node.mempool), utxos, dmnHashes[0], ownerKeys[dmnHashes[0]], newOperatorKey.GetPublicKey(), ownerKeys[dmnHashes[0]].GetPubKey().GetID(), dmn->pdmnState->scriptPayout, setup.coinbaseKey));
    // check malleability protection again, but this time by also relying on the signature inside the ProUpRegTx
    auto tx2 = MalleateProTxPayout<CProUpRegTx>(tx);
    TxValidationState dummy_state;
    {
        LOCK(cs_main);
        BOOST_REQUIRE(CheckProUpRegTx(CTransaction(tx), chainman.ActiveChain().Tip(), dmnman,
                                      chainman.ActiveChainstate().CoinsTip(), chainman, dummy_state, true));
        BOOST_REQUIRE(!CheckProUpRegTx(CTransaction(tx2), chainman.ActiveChain().Tip(), dmnman,
                                       chainman.ActiveChainstate().CoinsTip(), chainman, dummy_state, true));
    }
    BOOST_REQUIRE(CheckTransactionSignature(*(setup.m_node.mempool), tx));
    BOOST_REQUIRE(!CheckTransactionSignature(*(setup.m_node.mempool), tx2));
    // now process the block
    setup.CreateAndProcessBlock({tx}, coinbase_pk);
    sync_dmn_tip();
    BOOST_CHECK_EQUAL(tip_height(), nHeight + 1);
    nHeight++;

    tx = WITH_LOCK(::cs_main, return CreateProUpServTx(chainman.ActiveChain(), *(setup.m_node.mempool), utxos, dmnHashes[0], newOperatorKey, 100, CScript(), setup.coinbaseKey));
    setup.CreateAndProcessBlock({tx}, coinbase_pk);
    sync_dmn_tip();
    BOOST_CHECK_EQUAL(tip_height(), nHeight + 1);
    nHeight++;

    dmn = dmnman.GetListAtChainTip().GetMN(dmnHashes[0]);
    BOOST_REQUIRE(dmn != nullptr && dmn->pdmnState->netInfo->GetPrimary().GetPort() == 100);
    BOOST_REQUIRE(dmn != nullptr && !dmn->pdmnState->IsBanned());

    // test that the revived MN gets payments again
    bool foundRevived = false;
    for (size_t i = 0; i < 20; i++) {
        auto dmnExpectedPayee = dmnman.GetListAtChainTip().GetMNPayee(tip_index());
        BOOST_ASSERT(dmnExpectedPayee);
        if (dmnExpectedPayee->proTxHash == dmnHashes[0]) {
            foundRevived = true;
        }

        CBlock block = setup.CreateAndProcessBlock({}, coinbase_pk);
        sync_dmn_tip();
        BOOST_REQUIRE(!block.vtx.empty());

        auto dmnPayout = FindPayoutDmn(dmnman, block);
        BOOST_REQUIRE(dmnPayout != nullptr);
        BOOST_CHECK_EQUAL(dmnPayout->proTxHash.ToString(), dmnExpectedPayee->proTxHash.ToString());

        nHeight++;
    }
    BOOST_REQUIRE(foundRevived);

    const_cast<Consensus::Params&>(Params().GetConsensus()).DIP0003EnforcementHeight = DIP0003EnforcementHeightBackup;
}

void FuncTestMempoolReorg(TestChainSetup& setup)
{
    auto& chainman = *Assert(setup.m_node.chainman.get());
    auto tip_index  = [&] { return WITH_LOCK(::cs_main, return chainman.ActiveChain().Tip()); };
    auto tip_height = [&] { return WITH_LOCK(::cs_main, return chainman.ActiveChain().Height()); };
    auto tip_hash   = [&] { return WITH_LOCK(::cs_main, return chainman.ActiveChain().Tip()->GetBlockHash()); };

    const CScript coinbase_pk = GetScriptForRawPubKey(setup.coinbaseKey.GetPubKey());
    int nHeight = tip_height();
    auto utxos = BuildSimpleUtxoMap(setup.m_coinbase_txns);

    CKey ownerKey;
    CKey payoutKey;
    CKey collateralKey;
    CBLSSecretKey operatorKey;

    ownerKey.MakeNewKey(true);
    payoutKey.MakeNewKey(true);
    collateralKey.MakeNewKey(true);
    operatorKey.MakeNewKey();

    auto scriptPayout = GetScriptForDestination(PKHash(payoutKey.GetPubKey()));
    auto scriptCollateral = GetScriptForDestination(PKHash(collateralKey.GetPubKey()));

    // Create a MN with an external collateral
    CMutableTransaction tx_collateral;
    WITH_LOCK(::cs_main, FundTransaction(chainman.ActiveChain(), tx_collateral, utxos, scriptCollateral, dmn_types::Regular.collat_amount, setup.coinbaseKey));
    SignTransaction(*(setup.m_node.mempool), tx_collateral, setup.coinbaseKey);

    auto block = std::make_shared<CBlock>(setup.CreateBlock({tx_collateral}, coinbase_pk, chainman.ActiveChainstate()));
    BOOST_REQUIRE(chainman.ProcessNewBlock(block, true, nullptr));
    setup.m_node.dmnman->UpdatedBlockTip(tip_index());
    BOOST_CHECK_EQUAL(tip_height(), nHeight + 1);
    BOOST_CHECK_EQUAL(block->GetHash(), tip_hash());

    CProRegTx payload;
    payload.nVersion = ProTxVersion::GetMax(!bls::bls_legacy_scheme, /*is_extended_addr=*/false);
    payload.netInfo = NetInfoInterface::MakeNetInfo(payload.nVersion);
    BOOST_CHECK_EQUAL(payload.netInfo->AddEntry(NetInfoPurpose::CORE_P2P, "1.1.1.1:1"), NetInfoStatus::Success);
    payload.keyIDOwner = ownerKey.GetPubKey().GetID();
    payload.pubKeyOperator.Set(operatorKey.GetPublicKey(), bls::bls_legacy_scheme.load());
    payload.keyIDVoting = ownerKey.GetPubKey().GetID();
    payload.scriptPayout = scriptPayout;

    for (size_t i = 0; i < tx_collateral.vout.size(); ++i) {
        if (tx_collateral.vout[i].nValue == dmn_types::Regular.collat_amount) {
            payload.collateralOutpoint = COutPoint(tx_collateral.GetHash(), i);
            break;
        }
    }

    CMutableTransaction tx_reg;
    tx_reg.nVersion = 3;
    tx_reg.nType = TRANSACTION_PROVIDER_REGISTER;
    WITH_LOCK(::cs_main, FundTransaction(chainman.ActiveChain(), tx_reg, utxos, scriptPayout, dmn_types::Regular.collat_amount, setup.coinbaseKey));
    payload.inputsHash = CalcTxInputsHash(CTransaction(tx_reg));
    CMessageSigner::SignMessage(payload.MakeSignString(), payload.vchSig, collateralKey);
    SetTxPayload(tx_reg, payload);
    SignTransaction(*(setup.m_node.mempool), tx_reg, setup.coinbaseKey);

    CTxMemPool testPool{MemPoolOptionsForTest(setup.m_node)};
    if (setup.m_node.dmnman) {
        testPool.ConnectManagers(setup.m_node.dmnman.get(), setup.m_node.llmq_ctx->isman.get());
    }
    TestMemPoolEntryHelper entry;
    LOCK2(cs_main, testPool.cs);

    // Create ProUpServ and test block reorg which double-spend ProRegTx
    auto tx_up_serv = CreateProUpServTx(chainman.ActiveChain(), *(setup.m_node.mempool), utxos, tx_reg.GetHash(), operatorKey, 2, CScript(), setup.coinbaseKey);
    testPool.addUnchecked(entry.FromTx(tx_up_serv));
    // A disconnected block would insert ProRegTx back into mempool
    testPool.addUnchecked(entry.FromTx(tx_reg));
    BOOST_CHECK_EQUAL(testPool.size(), 2U);

    // Create a tx that will double-spend ProRegTx
    CMutableTransaction tx_reg_ds;
    tx_reg_ds.vin = tx_reg.vin;
    tx_reg_ds.vout.emplace_back(0, CScript() << OP_RETURN);
    SignTransaction(*(setup.m_node.mempool), tx_reg_ds, setup.coinbaseKey);

    // Check mempool as if a new block with tx_reg_ds was connected instead of the old one with tx_reg
    std::vector<CTransactionRef> block_reorg;
    block_reorg.emplace_back(std::make_shared<CTransaction>(tx_reg_ds));
    testPool.removeForBlock(block_reorg, nHeight + 2);
    BOOST_CHECK_EQUAL(testPool.size(), 0U);
}

void FuncTestMempoolDualProregtx(TestChainSetup& setup)
{
    auto& chainman = *Assert(setup.m_node.chainman.get());

    auto utxos = BuildSimpleUtxoMap(setup.m_coinbase_txns);

    // Create a MN
    CKey ownerKey1;
    CBLSSecretKey operatorKey1;
    auto tx_reg1 = WITH_LOCK(::cs_main, return CreateProRegTx(chainman.ActiveChain(), *(setup.m_node.mempool), utxos, 1, GenerateRandomAddress(), setup.coinbaseKey, ownerKey1, operatorKey1));

    // Create a MN with an external collateral that references tx_reg1
    CKey ownerKey;
    CKey payoutKey;
    CKey collateralKey;
    CBLSSecretKey operatorKey;

    ownerKey.MakeNewKey(true);
    payoutKey.MakeNewKey(true);
    collateralKey.MakeNewKey(true);
    operatorKey.MakeNewKey();

    auto scriptPayout = GetScriptForDestination(PKHash(payoutKey.GetPubKey()));

    CProRegTx payload;
    payload.nVersion = ProTxVersion::GetMax(!bls::bls_legacy_scheme, /*is_extended_addr=*/false);
    payload.netInfo = NetInfoInterface::MakeNetInfo(payload.nVersion);
    BOOST_CHECK_EQUAL(payload.netInfo->AddEntry(NetInfoPurpose::CORE_P2P, "1.1.1.1:2"), NetInfoStatus::Success);
    payload.keyIDOwner = ownerKey.GetPubKey().GetID();
    payload.pubKeyOperator.Set(operatorKey.GetPublicKey(), bls::bls_legacy_scheme.load());
    payload.keyIDVoting = ownerKey.GetPubKey().GetID();
    payload.scriptPayout = scriptPayout;

    for (size_t i = 0; i < tx_reg1.vout.size(); ++i) {
        if (tx_reg1.vout[i].nValue == dmn_types::Regular.collat_amount) {
            payload.collateralOutpoint = COutPoint(tx_reg1.GetHash(), i);
            break;
        }
    }

    CMutableTransaction tx_reg2;
    tx_reg2.nVersion = 3;
    tx_reg2.nType = TRANSACTION_PROVIDER_REGISTER;
    WITH_LOCK(::cs_main, FundTransaction(chainman.ActiveChain(), tx_reg2, utxos, scriptPayout, dmn_types::Regular.collat_amount, setup.coinbaseKey));
    payload.inputsHash = CalcTxInputsHash(CTransaction(tx_reg2));
    CMessageSigner::SignMessage(payload.MakeSignString(), payload.vchSig, collateralKey);
    SetTxPayload(tx_reg2, payload);
    SignTransaction(*(setup.m_node.mempool), tx_reg2, setup.coinbaseKey);

    CTxMemPool testPool{MemPoolOptionsForTest(setup.m_node)};
    if (setup.m_node.dmnman) {
        testPool.ConnectManagers(setup.m_node.dmnman.get(), setup.m_node.llmq_ctx->isman.get());
    }
    TestMemPoolEntryHelper entry;
    LOCK2(cs_main, testPool.cs);

    testPool.addUnchecked(entry.FromTx(tx_reg1));
    BOOST_CHECK_EQUAL(testPool.size(), 1U);
    BOOST_CHECK(testPool.existsProviderTxConflict(CTransaction(tx_reg2)));
}

void FuncVerifyDB(TestChainSetup& setup)
{
    auto& chainman = *Assert(setup.m_node.chainman.get());
    auto& dmnman = *Assert(setup.m_node.dmnman);
    auto tip_index    = [&] { return WITH_LOCK(::cs_main, return chainman.ActiveChain().Tip()); };
    auto tip_height   = [&] { return WITH_LOCK(::cs_main, return chainman.ActiveChain().Height()); };
    auto tip_hash     = [&] { return WITH_LOCK(::cs_main, return chainman.ActiveChain().Tip()->GetBlockHash()); };
    auto sync_dmn_tip = [&] { dmnman.UpdatedBlockTip(tip_index()); };

    const CScript coinbase_pk = GetScriptForRawPubKey(setup.coinbaseKey.GetPubKey());
    int nHeight = tip_height();
    auto utxos = BuildSimpleUtxoMap(setup.m_coinbase_txns);

    CKey ownerKey;
    CKey payoutKey;
    CKey collateralKey;
    CBLSSecretKey operatorKey;

    ownerKey.MakeNewKey(true);
    payoutKey.MakeNewKey(true);
    collateralKey.MakeNewKey(true);
    operatorKey.MakeNewKey();

    auto scriptPayout = GetScriptForDestination(PKHash(payoutKey.GetPubKey()));
    auto scriptCollateral = GetScriptForDestination(PKHash(collateralKey.GetPubKey()));

    // Create a MN with an external collateral
    CMutableTransaction tx_collateral;
    WITH_LOCK(::cs_main, FundTransaction(chainman.ActiveChain(), tx_collateral, utxos, scriptCollateral, dmn_types::Regular.collat_amount, setup.coinbaseKey));
    SignTransaction(*(setup.m_node.mempool), tx_collateral, setup.coinbaseKey);

    auto block = std::make_shared<CBlock>(setup.CreateBlock({tx_collateral}, coinbase_pk, chainman.ActiveChainstate()));
    BOOST_REQUIRE(chainman.ProcessNewBlock(block, true, nullptr));
    sync_dmn_tip();
    BOOST_CHECK_EQUAL(tip_height(), nHeight + 1);
    BOOST_CHECK_EQUAL(block->GetHash(), tip_hash());

    CProRegTx payload;
    payload.nVersion = ProTxVersion::GetMax(!bls::bls_legacy_scheme, /*is_extended_addr=*/false);
    payload.netInfo = NetInfoInterface::MakeNetInfo(payload.nVersion);
    BOOST_CHECK_EQUAL(payload.netInfo->AddEntry(NetInfoPurpose::CORE_P2P, "1.1.1.1:1"), NetInfoStatus::Success);
    payload.keyIDOwner = ownerKey.GetPubKey().GetID();
    payload.pubKeyOperator.Set(operatorKey.GetPublicKey(), bls::bls_legacy_scheme.load());
    payload.keyIDVoting = ownerKey.GetPubKey().GetID();
    payload.scriptPayout = scriptPayout;

    for (size_t i = 0; i < tx_collateral.vout.size(); ++i) {
        if (tx_collateral.vout[i].nValue == dmn_types::Regular.collat_amount) {
            payload.collateralOutpoint = COutPoint(tx_collateral.GetHash(), i);
            break;
        }
    }

    CMutableTransaction tx_reg;
    tx_reg.nVersion = 3;
    tx_reg.nType = TRANSACTION_PROVIDER_REGISTER;
    WITH_LOCK(::cs_main, FundTransaction(chainman.ActiveChain(), tx_reg, utxos, scriptPayout, dmn_types::Regular.collat_amount, setup.coinbaseKey));
    payload.inputsHash = CalcTxInputsHash(CTransaction(tx_reg));
    CMessageSigner::SignMessage(payload.MakeSignString(), payload.vchSig, collateralKey);
    SetTxPayload(tx_reg, payload);
    SignTransaction(*(setup.m_node.mempool), tx_reg, setup.coinbaseKey);

    auto tx_reg_hash = tx_reg.GetHash();

    block = std::make_shared<CBlock>(setup.CreateBlock({tx_reg}, coinbase_pk, chainman.ActiveChainstate()));
    BOOST_REQUIRE(chainman.ProcessNewBlock(block, true, nullptr));
    sync_dmn_tip();
    BOOST_CHECK_EQUAL(tip_height(), nHeight + 2);
    BOOST_CHECK_EQUAL(block->GetHash(), tip_hash());
    BOOST_REQUIRE(dmnman.GetListAtChainTip().HasMN(tx_reg_hash));

    // Now spend the collateral while updating the same MN
    SimpleUTXOMap collateral_utxos;
    collateral_utxos.emplace(payload.collateralOutpoint, std::make_pair(1, 1000));
    auto proUpRevTx = WITH_LOCK(::cs_main, return CreateProUpRevTx(chainman.ActiveChain(), *(setup.m_node.mempool), collateral_utxos, tx_reg_hash, operatorKey, collateralKey));

    block = std::make_shared<CBlock>(setup.CreateBlock({proUpRevTx}, coinbase_pk, chainman.ActiveChainstate()));
    BOOST_REQUIRE(chainman.ProcessNewBlock(block, true, nullptr));
    sync_dmn_tip();
    BOOST_CHECK_EQUAL(tip_height(), nHeight + 3);
    BOOST_CHECK_EQUAL(block->GetHash(), tip_hash());
    BOOST_REQUIRE(!dmnman.GetListAtChainTip().HasMN(tx_reg_hash));

    // Verify db consistency
    LOCK(cs_main);
    BOOST_REQUIRE(CVerifyDB().VerifyDB(chainman.ActiveChainstate(), Params().GetConsensus(),
                                       chainman.ActiveChainstate().CoinsTip(), *(setup.m_node.evodb), 4, 2));
}

static CDeterministicMNCPtr create_mock_mn(uint64_t internal_id)
{
    // Create a mock MN
    CKey ownerKey;
    ownerKey.MakeNewKey(true);
    CBLSSecretKey operatorKey;
    operatorKey.MakeNewKey();

    auto dmnState = std::make_shared<CDeterministicMNState>();
    dmnState->confirmedHash = GetRandHash();
    dmnState->keyIDOwner = ownerKey.GetPubKey().GetID();
    dmnState->pubKeyOperator.Set(operatorKey.GetPublicKey(), bls::bls_legacy_scheme.load());
    dmnState->keyIDVoting = ownerKey.GetPubKey().GetID();
    dmnState->netInfo = NetInfoInterface::MakeNetInfo(
        ProTxVersion::GetMax(!bls::bls_legacy_scheme, /*is_extended_addr=*/false));
    BOOST_CHECK_EQUAL(dmnState->netInfo->AddEntry(NetInfoPurpose::CORE_P2P, "1.1.1.1:1"), NetInfoStatus::Success);

    auto dmn = std::make_shared<CDeterministicMN>(internal_id, MnType::Regular);
    dmn->proTxHash = GetRandHash();
    dmn->collateralOutpoint = COutPoint(GetRandHash(), 0);
    dmn->nOperatorReward = 0;
    dmn->pdmnState = dmnState;

    return dmn;
}

static void SmlCache(TestChainSetup& setup)
{
    BOOST_CHECK(setup.m_node.dmnman != nullptr);

    // Create empty list and verify SML cache
    CDeterministicMNList emptyList(uint256(), 0, 0);
    auto sml_empty = emptyList.to_sml();

    // Should return the same cached object
    BOOST_CHECK(sml_empty == emptyList.to_sml());

    // Should contain empty list
    BOOST_CHECK_EQUAL(sml_empty->mnList.size(), 0);

    // Copy list should return the same cached object
    CDeterministicMNList mn_list_1(emptyList);
    BOOST_CHECK(sml_empty == mn_list_1.to_sml());

    CDeterministicMNList mn_list_2;
    // Assigning list should return the same cached object
    mn_list_2 = emptyList;
    BOOST_CHECK(sml_empty == mn_list_2.to_sml());

    auto dmn = create_mock_mn(1);

    // Add MN - should invalidate cache
    mn_list_1.AddMN(dmn, true);
    auto sml_add = mn_list_1.to_sml();

    // Cache should be invalidated, so different pointer but equal content after regeneration
    BOOST_CHECK(sml_empty != sml_add); // Different pointer (cache invalidated)

    BOOST_CHECK_EQUAL(sml_add->mnList.size(), 1); // Should contain the added MN

    {
        // Remove MN - should invalidate cache
        CDeterministicMNList mn_list(mn_list_1);
        BOOST_CHECK(mn_list_1.to_sml() == mn_list.to_sml());

        mn_list.RemoveMN(dmn->proTxHash);
        auto sml_remove = mn_list.to_sml();

        // Cache should be invalidated
        BOOST_CHECK(sml_remove != sml_add);
        BOOST_CHECK(sml_remove != sml_empty);
        BOOST_CHECK_EQUAL(sml_remove->mnList.size(), 0); // Should be empty after removal
    }

    // Start with a list containing one MN mn_list_1
    // Test 1: Update with same SML entry data - cache should NOT be invalidated
    auto unchangedState = std::make_shared<CDeterministicMNState>(*dmn->pdmnState);
    unchangedState->nPoSePenalty += 10;
    mn_list_1.UpdateMN(*dmn, unchangedState);

    // Cache should NOT be invalidated since SML entry didn't change
    BOOST_CHECK(sml_add == mn_list_1.to_sml()); // Same pointer (cache preserved)

    // Test 2: Update with different SML entry data - cache SHOULD be invalidated
    auto changedState = std::make_shared<CDeterministicMNState>(*unchangedState);
    changedState->pubKeyOperator.Set(CBLSPublicKey{}, bls::bls_legacy_scheme.load());
    mn_list_1.UpdateMN(*dmn, changedState);

    // Cache should be invalidated since SML entry changed
    BOOST_CHECK(sml_add != mn_list_1.to_sml());
    BOOST_CHECK_EQUAL(mn_list_1.to_sml()->mnList.size(), 1); // Still one MN but with updated data
}

BOOST_AUTO_TEST_SUITE(evo_dip3_activation_tests)

BOOST_AUTO_TEST_CASE(deterministic_mn_list_diff_serialization_is_canonical)
{
    CDeterministicMNListDiff forward;
    forward.updatedMNs.emplace(1, CDeterministicMNStateDiff{});
    forward.updatedMNs.emplace(2, CDeterministicMNStateDiff{});

    CDeterministicMNListDiff reverse;
    reverse.updatedMNs.emplace(2, CDeterministicMNStateDiff{});
    reverse.updatedMNs.emplace(1, CDeterministicMNStateDiff{});

    CDataStream forward_stream{SER_DISK, CLIENT_VERSION};
    CDataStream reverse_stream{SER_DISK, CLIENT_VERSION};
    forward_stream << forward;
    reverse_stream << reverse;

    BOOST_CHECK_EQUAL_COLLECTIONS(forward_stream.begin(), forward_stream.end(),
                                  reverse_stream.begin(), reverse_stream.end());
}

struct TestChainDIP3BeforeActivationSetup : public TestChainSetup {
    TestChainDIP3BeforeActivationSetup() :
        TestChainSetup(430)
    {
    }
};

struct TestChainDIP3Setup : public TestChainDIP3BeforeActivationSetup {
    TestChainDIP3Setup()
    {
        // Activate DIP3 here
        CreateAndProcessBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
    }
};

struct TestChainV19BeforeActivationSetup : public TestChainSetup {
    TestChainV19BeforeActivationSetup();
};

struct TestChainV19Setup : public TestChainV19BeforeActivationSetup {
    TestChainV19Setup()
    {
        const CScript coinbase_pk = GetScriptForRawPubKey(coinbaseKey.GetPubKey());
        // Activate V19
        for (int i = 0; i < 5; ++i) {
            CreateAndProcessBlock({}, coinbase_pk);
        }
        bool v19_just_activated{WITH_LOCK(::cs_main, return DeploymentActiveAfter(m_node.chainman->ActiveChain().Tip(), m_node.chainman->GetConsensus(), Consensus::DEPLOYMENT_V19) &&
            !DeploymentActiveAt(*m_node.chainman->ActiveChain().Tip(), m_node.chainman->GetConsensus(), Consensus::DEPLOYMENT_V19))};
        assert(v19_just_activated);
    }
};

// 5 blocks earlier
TestChainV19BeforeActivationSetup::TestChainV19BeforeActivationSetup() :
    TestChainSetup(494, CBaseChainParams::REGTEST, {"-testactivationheight=v19@500", "-testactivationheight=v20@500", "-testactivationheight=mn_rr@500"})
{
    bool v19_active{WITH_LOCK(::cs_main, return DeploymentActiveAfter(m_node.chainman->ActiveChain().Tip(), m_node.chainman->GetConsensus(),
                                          Consensus::DEPLOYMENT_V19))};
    assert(!v19_active);
}

struct TestChainV24SignalBeforeV19Setup : public TestChainSetup {
    TestChainV24SignalBeforeV19Setup() :
        TestChainSetup(494, CBaseChainParams::REGTEST,
                       {"-testactivationheight=v19@500", "-testactivationheight=v20@500",
                        "-testactivationheight=mn_rr@500",
                        "-vbparams=v24:0:9999999999:0:500:400:300:5:0"})
    {
        assert(WITH_LOCK(::cs_main, return !DeploymentActiveAfter(m_node.chainman->ActiveChain().Tip(), m_node.chainman->GetConsensus(),
                                      Consensus::DEPLOYMENT_V19)));
        assert(WITH_LOCK(::cs_main, return !DeploymentActiveAfter(m_node.chainman->ActiveChain().Tip(), *m_node.chainman,
                                      Consensus::DEPLOYMENT_V24)));
    }
};

// v19/v20/mn_rr active at height 500, with v24 signalled but still PENDING (not yet locked in).
// The 500-block window means v24 locks in well after v19, so this fixture stops in the window
// where basic-BLS (v2) registrations are valid but strict multiplicity matching is off, letting
// a single test cross the v24 activation boundary.
struct TestChainV24PendingSetup : public TestChainSetup {
    TestChainV24PendingSetup() :
        TestChainSetup(494, CBaseChainParams::REGTEST,
                       {"-testactivationheight=v19@500", "-testactivationheight=v20@500",
                        "-testactivationheight=mn_rr@500",
                        "-vbparams=v24:0:9999999999:0:500:400:300:5:0"})
    {
        const CScript coinbase_pk = GetScriptForRawPubKey(coinbaseKey.GetPubKey());
        auto& chainman = *Assert(m_node.chainman);
        auto& dmnman = *Assert(m_node.dmnman);
        // Mine just enough to activate v19/v20/mn_rr (height 500) while keeping v24 pending.
        for (int i = 0; i < 20 && WITH_LOCK(::cs_main, return !DeploymentActiveAfter(chainman.ActiveChain().Tip(), chainman.GetConsensus(), Consensus::DEPLOYMENT_V19)); ++i) {
            CreateAndProcessBlock({}, coinbase_pk);
            dmnman.UpdatedBlockTip(WITH_LOCK(::cs_main, return chainman.ActiveChain().Tip()));
        }
        assert(WITH_LOCK(::cs_main, return DeploymentActiveAfter(chainman.ActiveChain().Tip(), chainman.GetConsensus(), Consensus::DEPLOYMENT_V19)));
        assert(WITH_LOCK(::cs_main, return !DeploymentActiveAfter(chainman.ActiveChain().Tip(), chainman, Consensus::DEPLOYMENT_V24)));
        assert(!bls::bls_legacy_scheme.load());
    }
};

// DIP3 can only be activated with legacy scheme (v19 is activated later)
BOOST_AUTO_TEST_CASE(dip3_activation_legacy)
{
    TestChainDIP3BeforeActivationSetup setup;
    FuncDIP3Activation(setup);
}

// V19 can only be activated with legacy scheme
BOOST_AUTO_TEST_CASE(v19_activation_legacy)
{
    TestChainV19BeforeActivationSetup setup;
    FuncV19Activation(setup);
}

BOOST_AUTO_TEST_CASE(proupreg_version_handling_before_v24)
{
    TestChainV19BeforeActivationSetup setup;
    FuncProUpRegTxVersionHandlingBeforeV24(setup);
}

BOOST_AUTO_TEST_CASE(proupreg_v4_on_legacy_rejected)
{
    TestChainV24SignalBeforeV19Setup setup;
    FuncProUpRegTxV4OnLegacyRejected(setup);
}

BOOST_AUTO_TEST_CASE(proupreg_v2_cannot_bypass_v4_payout_collateral_reuse)
{
    TestChainV24SignalBeforeV19Setup setup;
    FuncProUpRegTxV2CannotBypassV4PayoutCollateralReuse(setup);
}

BOOST_AUTO_TEST_CASE(proregtx_rejects_invalid_deserialized_extnetinfo)
{
    TestChainV24SignalBeforeV19Setup setup;
    FuncProRegTxRejectsInvalidDeserializedExtNetInfo(setup);
}

BOOST_AUTO_TEST_CASE(mn_payment_multiplicity_v24_boundary)
{
    TestChainV24PendingSetup setup;
    FuncMNPaymentMultiplicityV24Boundary(setup);
}

BOOST_AUTO_TEST_CASE(dip3_protx_legacy)
{
    TestChainDIP3Setup setup;
    FuncDIP3Protx(setup);
}

BOOST_AUTO_TEST_CASE(dip3_protx_basic)
{
    TestChainV19Setup setup;
    FuncDIP3Protx(setup);
}

BOOST_AUTO_TEST_CASE(test_mempool_reorg_legacy)
{
    TestChainDIP3Setup setup;
    FuncTestMempoolReorg(setup);
}

BOOST_AUTO_TEST_CASE(test_mempool_reorg_basic)
{
    TestChainV19Setup setup;
    FuncTestMempoolReorg(setup);
}

BOOST_AUTO_TEST_CASE(test_mempool_dual_proregtx_legacy)
{
    TestChainDIP3Setup setup;
    FuncTestMempoolDualProregtx(setup);
}

BOOST_AUTO_TEST_CASE(test_mempool_dual_proregtx_basic)
{
    TestChainV19Setup setup;
    FuncTestMempoolDualProregtx(setup);
}

//This one can be started only with legacy scheme, since inside undo block will switch it back to legacy resulting into an inconsistency
BOOST_AUTO_TEST_CASE(verify_db_legacy)
{
    TestChainDIP3Setup setup;
    FuncVerifyDB(setup);
}

BOOST_AUTO_TEST_CASE(test_sml_cache_legacy)
{
    TestChainDIP3Setup setup;
    SmlCache(setup);
}

BOOST_AUTO_TEST_CASE(test_sml_cache_basic)
{
    TestChainV19Setup setup;
    SmlCache(setup);
}

BOOST_AUTO_TEST_CASE(field_bit_migration_validation)
{
    // Test individual field mappings for ALL 19 fields
    struct FieldMapping {
        uint32_t legacyBit;
        uint32_t newBit;
        std::string name;
    };

    std::vector<FieldMapping> mappings = {
        {0x0001, CDeterministicMNStateDiff::Field_nRegisteredHeight, "nRegisteredHeight"},
        {0x0002, CDeterministicMNStateDiff::Field_nLastPaidHeight, "nLastPaidHeight"},
        {0x0004, CDeterministicMNStateDiff::Field_nPoSePenalty, "nPoSePenalty"},
        {0x0008, CDeterministicMNStateDiff::Field_nPoSeRevivedHeight, "nPoSeRevivedHeight"},
        {0x0010, CDeterministicMNStateDiff::Field_nPoSeBanHeight, "nPoSeBanHeight"},
        {0x0020, CDeterministicMNStateDiff::Field_nRevocationReason, "nRevocationReason"},
        {0x0040, CDeterministicMNStateDiff::Field_confirmedHash, "confirmedHash"},
        {0x0080, CDeterministicMNStateDiff::Field_confirmedHashWithProRegTxHash, "confirmedHashWithProRegTxHash"},
        {0x0100, CDeterministicMNStateDiff::Field_keyIDOwner, "keyIDOwner"},
        {0x0200, CDeterministicMNStateDiff::Field_pubKeyOperator, "pubKeyOperator"},
        {0x0400, CDeterministicMNStateDiff::Field_keyIDVoting, "keyIDVoting"},
        {0x0800, CDeterministicMNStateDiff::Field_netInfo, "netInfo"},
        {0x1000, CDeterministicMNStateDiff::Field_scriptPayout, "scriptPayout"},
        {0x2000, CDeterministicMNStateDiff::Field_scriptOperatorPayout, "scriptOperatorPayout"},
        {0x4000, CDeterministicMNStateDiff::Field_nConsecutivePayments, "nConsecutivePayments"},
        {0x8000, CDeterministicMNStateDiff::Field_platformNodeID, "platformNodeID"},
        {0x10000, CDeterministicMNStateDiff::Field_platformP2PPort, "platformP2PPort"},
        {0x20000, CDeterministicMNStateDiff::Field_platformHTTPPort, "platformHTTPPort"},
        {0x40000, CDeterministicMNStateDiff::Field_nVersion, "nVersion"},
    };

    // Verify each field mapping is correct
    for (const auto& mapping : mappings) {
        // Test individual field conversion
        CDeterministicMNStateDiffLegacy legacyDiff;
        legacyDiff.fields |= mapping.legacyBit;
        // Convert to new format
        auto newDiff = legacyDiff.ToNewFormat();
        BOOST_CHECK_MESSAGE(newDiff.fields == mapping.newBit, strprintf("Field %s: legacy 0x%x should convert to 0x%x",
                                                                        mapping.name, mapping.legacyBit, mapping.newBit));
    }

    // Test complex multi-field scenarios
    uint32_t complexLegacyFields = 0x0200 | // Legacy Field_pubKeyOperator
                                   0x0800 | // Legacy Field_netInfo
                                   0x1000 | // Legacy Field_scriptPayout
                                   0x40000; // Legacy Field_nVersion

    uint32_t expectedNewFields = CDeterministicMNStateDiff::Field_nVersion |       // 0x0001
                                 CDeterministicMNStateDiff::Field_pubKeyOperator | // 0x0400 (was 0x0200)
                                 CDeterministicMNStateDiff::Field_netInfo |        // 0x1000 (was 0x0800)
                                 CDeterministicMNStateDiff::Field_scriptPayout;    // 0x2000 (was 0x1000)

    CDeterministicMNStateDiffLegacy legacyDiff;
    legacyDiff.fields |= complexLegacyFields;
    // Convert to new format
    auto newDiff = legacyDiff.ToNewFormat();
    BOOST_CHECK_EQUAL(newDiff.fields, expectedNewFields);

    // Verify no bit conflicts exist in new field layout
    std::set<uint32_t> usedBits;
    for (const auto& mapping : mappings) {
        BOOST_CHECK_MESSAGE(usedBits.find(mapping.newBit) == usedBits.end(),
                            strprintf("Duplicate bit 0x%x found for field %s", mapping.newBit, mapping.name));
        usedBits.insert(mapping.newBit);
    }

    // Verify all 19 fields have unique bit assignments
    BOOST_CHECK_EQUAL(usedBits.size(), 19);
}

BOOST_AUTO_TEST_CASE(migration_logic_validation)
{
    // Test the database migration logic for nVersion-first format conversion.
    // Migration logic is handled at CDeterministicMNListDiff level
    // using CDeterministicMNStateDiffLegacy for legacy format deserialization.

    // Create sample legacy format state diff
    CDeterministicMNStateDiffLegacy legacyDiff;
    legacyDiff.fields = 0x40000 | 0x0200 | 0x0800 | 0x0010; // Legacy: nVersion, pubKeyOperator, netInfo, nPoSeBanHeight
    legacyDiff.state.nVersion = ProTxVersion::BasicBLS;
    CBLSSecretKey sk;
    sk.MakeNewKey();
    legacyDiff.state.pubKeyOperator.Set(sk.GetPublicKey(), false);
    BOOST_CHECK(!legacyDiff.state.pubKeyOperator.IsLegacy());
    legacyDiff.state.netInfo = NetInfoInterface::MakeNetInfo(ProTxVersion::BasicBLS);
    BOOST_CHECK(!legacyDiff.state.IsBanned());
    legacyDiff.state.BanIfNotBanned(2367316);
    BOOST_CHECK(legacyDiff.state.IsBanned());
    BOOST_CHECK_EQUAL(legacyDiff.state.GetBannedHeight(), 2367316);


    // Test legacy class conversion (this would normally be done by CDeterministicMNListDiff)
    CDataStream ss(SER_DISK, CLIENT_VERSION);
    ss << legacyDiff;

    CDeterministicMNStateDiffLegacy legacyDeserializer(deserialize, ss);
    CDeterministicMNStateDiff convertedDiff = legacyDeserializer.ToNewFormat();
    BOOST_CHECK(!legacyDiff.state.pubKeyOperator.IsLegacy());
    BOOST_CHECK(convertedDiff.state.pubKeyOperator.IsLegacy());
    convertedDiff.state.pubKeyOperator.SetLegacy(false);
    BOOST_CHECK(!convertedDiff.state.pubKeyOperator.IsLegacy());

    // Verify conversion worked correctly
    uint32_t expectedNewFields = CDeterministicMNStateDiff::Field_nVersion |       // 0x0001
                                 CDeterministicMNStateDiff::Field_nPoSeBanHeight | // 0x0020
                                 CDeterministicMNStateDiff::Field_pubKeyOperator | // 0x0400
                                 CDeterministicMNStateDiff::Field_netInfo;         // 0x1000

    BOOST_CHECK_EQUAL(convertedDiff.fields, expectedNewFields);
    BOOST_CHECK_EQUAL(convertedDiff.state.nVersion, legacyDiff.state.nVersion);
    BOOST_CHECK_EQUAL(convertedDiff.state.GetBannedHeight(), legacyDiff.state.GetBannedHeight());
    BOOST_CHECK(convertedDiff.state.pubKeyOperator.Get() == legacyDiff.state.pubKeyOperator.Get());
    BOOST_CHECK_EQUAL(convertedDiff.state.pubKeyOperator.ToString(), legacyDiff.state.pubKeyOperator.ToString());
}

BOOST_AUTO_TEST_SUITE_END()
