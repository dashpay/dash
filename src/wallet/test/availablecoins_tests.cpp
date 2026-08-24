// Copyright (c) 2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or https://www.opensource.org/licenses/mit-license.php.

#include <coinjoin/coinjoin.h>
#include <txmempool.h>
#include <bls/bls.h>
#include <evo/deterministicmns.h>
#include <evo/dmn_types.h>
#include <evo/providertx.h>
#include <evo/specialtx.h>
#include <interfaces/chain.h>
#include <test/util/masternode.h>
#include <validation.h>
#include <wallet/coincontrol.h>
#include <wallet/coinjoin.h>
#include <wallet/spend.h>
#include <wallet/test/util.h>
#include <wallet/test/wallet_test_fixture.h>

#include <boost/test/unit_test.hpp>

namespace wallet {
BOOST_FIXTURE_TEST_SUITE(availablecoins_tests, WalletTestingSetup)
class AvailableCoinsTestingSetup : public TestChain100Setup
{
public:
    explicit AvailableCoinsTestingSetup(const std::vector<const char*>& extra_args = {}) :
        TestChain100Setup{CBaseChainParams::REGTEST, extra_args}
    {
        CreateAndProcessBlock({}, {});
        wallet = CreateSyncedWallet(*m_node.chain, *m_node.coinjoin_loader, *m_node.chainman, m_args, coinbaseKey);
    }

    ~AvailableCoinsTestingSetup()
    {
        wallet.reset();
    }
    //! Feed a mined block to the wallet the way the node would.
    void ConnectToWallet(const CBlock& block)
    {
        const CBlockIndex* tip{WITH_LOCK(m_node.chainman->GetMutex(), return m_node.chainman->ActiveChain().Tip())};
        const uint256 block_hash{block.GetHash()};
        interfaces::BlockInfo block_info{block_hash};
        block_info.prev_hash = &block.hashPrevBlock;
        block_info.height = tip->nHeight;
        block_info.data = &block;
        wallet->blockConnected(block_info);
    }

    CWalletTx& AddTx(CRecipient recipient)
    {
        CTransactionRef tx;
        CCoinControl dummy;
        {
            auto res = CreateTransaction(*wallet, {recipient}, RANDOM_CHANGE_POSITION, dummy);
            BOOST_CHECK(res);
            tx = res->tx;
        }
        wallet->CommitTransaction(tx, {}, {});
        CMutableTransaction blocktx;
        {
            LOCK(wallet->cs_wallet);
            blocktx = CMutableTransaction(*wallet->mapWallet.at(tx->GetHash()).tx);
        }
        CreateAndProcessBlock({CMutableTransaction(blocktx)}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));

        const CBlockIndex* tip{WITH_LOCK(m_node.chainman->GetMutex(), return m_node.chainman->ActiveChain().Tip())};
        LOCK(wallet->cs_wallet);
        wallet->SetLastBlockProcessed(tip->nHeight, tip->GetBlockHash());
        auto it = wallet->mapWallet.find(tx->GetHash());
        BOOST_CHECK(it != wallet->mapWallet.end());
        it->second.m_state = TxStateConfirmed{tip->GetBlockHash(), tip->nHeight, /*index=*/1};
        return it->second;
    }

    std::unique_ptr<CWallet> wallet;
};

BOOST_FIXTURE_TEST_CASE(BasicOutputTypesTest, AvailableCoinsTestingSetup)
{
    CoinsResult available_coins;
    util::Result<CTxDestination> dest{util::Error{}};

    // Verify our wallet has one usable coinbase UTXO before starting
    // This UTXO is a P2PK, so it should show up in the Other bucket
    {
        LOCK(wallet->cs_wallet);
        available_coins = AvailableCoins(*wallet);
    }
    BOOST_CHECK_EQUAL(available_coins.size(), 1U);
    BOOST_CHECK_EQUAL(available_coins.other.size(), 1U);

    // We will create a self transfer for each of the OutputTypes and
    // verify it is put in the correct bucket after running GetAvailablecoins
    //
    // For each OutputType, We expect 2 UTXOs in our wallet following the self transfer:
    //   1. One UTXO as the recipient
    //   2. One UTXO from the change, due to payment address matching logic

    // Legacy (P2PKH)
    {
        LOCK(wallet->cs_wallet);
        dest = wallet->GetNewDestination("");
    }
    BOOST_ASSERT(dest);
    AddTx(CRecipient{{GetScriptForDestination(*dest)}, 4 * COIN, /*fSubtractFeeFromAmount=*/true});
    {
        LOCK(wallet->cs_wallet);
        available_coins = AvailableCoins(*wallet);
    }
    BOOST_CHECK_EQUAL(available_coins.legacy.size(), 2U);
}

BOOST_FIXTURE_TEST_CASE(UnconfirmableOutputsAreNotWalletFunds, AvailableCoinsTestingSetup)
{
    LOCK(wallet->cs_wallet);

    const auto dest{wallet->GetNewDestination("")};
    BOOST_ASSERT(dest);

    // Use a real CoinJoin denomination so the denominated-credit paths apply.
    const CAmount denom{CoinJoin::GetSmallestDenomination()};
    CMutableTransaction mtx;
    mtx.vin.emplace_back(COutPoint{uint256::ONE, 0});
    mtx.vout.emplace_back(denom, GetScriptForDestination(*dest));
    const CTransactionRef tx{MakeTransactionRef(mtx)};

    // A transaction the wallet knows about but that never reached the mempool cannot
    // confirm as it stands, so its outputs are not funds the wallet can spend or mix.
    BOOST_CHECK(wallet->AddToWallet(tx, TxStateInactive{}));
    BOOST_CHECK_EQUAL(wallet->CountInputsWithAmount(denom), 0);

    // The aggregate CoinJoin balances have to agree: an output that is not wallet funds
    // is not denominated or anonymized funds either.
    const CWalletTx& wtx{wallet->mapWallet.at(tx->GetHash())};
    BOOST_CHECK_EQUAL(CachedTxGetAvailableCoinJoinCredits(*wallet, wtx).m_denominated, 0);

    // Once it is in the mempool they count.
    BOOST_CHECK(wallet->AddToWallet(tx, TxStateInMempool{}));
    BOOST_CHECK_EQUAL(wallet->CountInputsWithAmount(denom), 1);
    BOOST_CHECK_EQUAL(CachedTxGetAvailableCoinJoinCredits(*wallet, wtx).m_denominated, denom);
}

BOOST_FIXTURE_TEST_CASE(MempoolRemovalInvalidatesAnonymizableTally, AvailableCoinsTestingSetup)
{
    LOCK(wallet->cs_wallet);

    const auto dest{wallet->GetNewDestination("")};
    BOOST_ASSERT(dest);

    // A wallet transaction in the mempool is trusted at depth zero, so its outputs count
    // towards the anonymizable tally and that tally is cacheable.
    auto created{CreateTransaction(*wallet, {CRecipient{GetScriptForDestination(*dest), 1 * COIN,
                                                        /*fSubtractFeeFromAmount=*/false}},
                                   RANDOM_CHANGE_POSITION, CCoinControl{})};
    BOOST_REQUIRE(created);
    const CTransactionRef tx{created->tx};
    BOOST_CHECK(wallet->AddToWallet(tx, TxStateInMempool{}));

    const auto tallied = [&](const CTxDestination& target) {
        for (const auto& item : wallet->SelectCoinsGroupedByAddresses()) {
            if (item.txdest == target) return true;
        }
        return false;
    };

    // Prime the cache, so that the check below cannot pass by recomputing the tally.
    BOOST_REQUIRE(tallied(*dest));

    // Leaving the mempool makes the transaction unconfirmable as it stands; the cached
    // tally must not keep handing out its outputs.
    wallet->transactionRemovedFromMempool(tx, MemPoolRemovalReason::EXPIRY);
    BOOST_CHECK(!tallied(*dest));
}

BOOST_FIXTURE_TEST_CASE(DeliberateUnlockSurvivesAutomaticLocking, AvailableCoinsTestingSetup)
{
    LOCK(wallet->cs_wallet);
    wallet->m_dust_protection_threshold = 1 * COIN;

    const auto dest{wallet->GetNewDestination("")};
    BOOST_ASSERT(dest);

    // An external transaction pays us a dust-protection target; AddToWallet() locks it.
    CMutableTransaction dust_mtx;
    dust_mtx.vin.emplace_back(COutPoint{uint256::ONE, 0});
    dust_mtx.vout.emplace_back(COIN / 100, GetScriptForDestination(*dest));
    const CTransactionRef dust_tx{MakeTransactionRef(dust_mtx)};
    const COutPoint dust_outpoint{dust_tx->GetHash(), 0};
    BOOST_CHECK(wallet->AddToWallet(dust_tx, TxStateInMempool{}));
    BOOST_CHECK(wallet->IsLockedCoin(dust_outpoint));

    // The user releases the automatic lock because the spend is really intended.
    WalletBatch batch{wallet->GetDatabase()};
    BOOST_CHECK(wallet->UnlockCoinByUser(dust_outpoint, &batch));
    BOOST_CHECK(wallet->IsAutoLockOptOut(dust_outpoint));

    // A wallet load reapplies the automatic locks, so the decision has to outlive it.
    wallet->LockExistingDustOutputs();
    BOOST_CHECK(!wallet->IsLockedCoin(dust_outpoint));

    // A lock the caller keeps in memory only leaves the decision standing.
    BOOST_CHECK(wallet->LockCoinByUser(dust_outpoint, /*batch=*/nullptr));
    BOOST_CHECK(wallet->IsAutoLockOptOut(dust_outpoint));
    BOOST_CHECK(wallet->UnlockCoin(dust_outpoint, &batch));

    // Locking again persistently hands the outpoint back to the automatic protection.
    BOOST_CHECK(wallet->LockCoinByUser(dust_outpoint, &batch));
    BOOST_CHECK(!wallet->IsAutoLockOptOut(dust_outpoint));
    BOOST_CHECK(wallet->UnlockCoin(dust_outpoint, &batch));
    wallet->LockExistingDustOutputs();
    BOOST_CHECK(wallet->IsLockedCoin(dust_outpoint));
}

// The shared fixture leaves the chain at height 101, so activate DIP3 just above it
// instead of mining to the regtest default of 432.
struct MasternodeCollateralTestingSetup : public AvailableCoinsTestingSetup {
    MasternodeCollateralTestingSetup() :
        AvailableCoinsTestingSetup{{"-dip3params=105:500", "-testactivationheight=v20@105",
                                    "-testactivationheight=mn_rr@105"}}
    {
    }
};

BOOST_FIXTURE_TEST_CASE(DeliberateUnlockSurvivesCollateralAutoLocking, MasternodeCollateralTestingSetup)
{
    const CScript wallet_script{GetScriptForDestination(PKHash(coinbaseKey.GetPubKey()))};
    while (WITH_LOCK(m_node.chainman->GetMutex(), return m_node.chainman->ActiveChain().Height()) <
           Params().GetConsensus().DIP0003Height) {
        CreateAndProcessBlock({}, wallet_script);
    }

    CKey owner_key;
    CBLSSecretKey operator_key;
    auto utxos{BuildSimpleUtxoMap(m_coinbase_txns)};
    CMutableTransaction pro_reg_mtx{CreateProRegTx(*m_node.chainman, utxos, /*port=*/1, wallet_script,
                                                   coinbaseKey, owner_key, operator_key)};
    const CTransactionRef pro_reg_tx{MakeTransactionRef(pro_reg_mtx)};
    const CBlock block{CreateAndProcessBlock({pro_reg_mtx}, wallet_script)};
    const CBlockIndex* tip{WITH_LOCK(m_node.chainman->GetMutex(), return m_node.chainman->ActiveChain().Tip())};
    {
        LOCK(::cs_main);
        m_node.dmnman->UpdatedBlockTip(tip);
        BOOST_REQUIRE(m_node.dmnman->GetListAtChainTip().HasMN(pro_reg_tx->GetHash()));
    }

    const uint256 block_hash{block.GetHash()};
    interfaces::BlockInfo block_info{block_hash};
    block_info.prev_hash = &block.hashPrevBlock;
    block_info.height = tip->nHeight;
    block_info.data = &block;
    wallet->blockConnected(block_info);

    const COutPoint collateral{pro_reg_tx->GetHash(), 0};
    BOOST_CHECK(WITH_LOCK(wallet->cs_wallet, return wallet->IsLockedCoin(collateral)));

    // Unlocking the collateral deliberately opts it out of the automatic protection, so
    // that a reload cannot silently take back the decision to spend it.
    {
        LOCK(wallet->cs_wallet);
        WalletBatch batch{wallet->GetDatabase()};
        BOOST_CHECK(wallet->UnlockCoinByUser(collateral, &batch));
        BOOST_CHECK(wallet->IsAutoLockOptOut(collateral));
    }
    wallet->AutoLockMasternodeCollaterals();
    BOOST_CHECK(WITH_LOCK(wallet->cs_wallet, return !wallet->IsLockedCoin(collateral)));

    // Locking it again hands it back to the automatic protection.
    {
        LOCK(wallet->cs_wallet);
        WalletBatch batch{wallet->GetDatabase()};
        BOOST_CHECK(wallet->LockCoinByUser(collateral, &batch));
        BOOST_CHECK(!wallet->IsAutoLockOptOut(collateral));
        BOOST_CHECK(wallet->UnlockCoin(collateral, &batch));
    }
    wallet->AutoLockMasternodeCollaterals();
    BOOST_CHECK(WITH_LOCK(wallet->cs_wallet, return wallet->IsLockedCoin(collateral)));
}

BOOST_FIXTURE_TEST_CASE(CollateralRegistrationSupersedesDeliberateUnlock, MasternodeCollateralTestingSetup)
{
    const CScript wallet_script{GetScriptForDestination(PKHash(coinbaseKey.GetPubKey()))};
    while (WITH_LOCK(m_node.chainman->GetMutex(), return m_node.chainman->ActiveChain().Height()) <
           Params().GetConsensus().DIP0003Height) {
        CreateAndProcessBlock({}, wallet_script);
    }

    // An ordinary output of collateral size, mined and known to the wallet.
    auto utxos{BuildSimpleUtxoMap(m_coinbase_txns)};
    CMutableTransaction collateral_mtx{CreateSpendTx(*m_node.chainman, utxos, wallet_script,
                                                     dmn_types::Regular.collat_amount, coinbaseKey)};
    const CBlock collateral_block{CreateAndProcessBlock({collateral_mtx}, wallet_script)};
    ConnectToWallet(collateral_block);
    const COutPoint collateral{GetCollateralOutpoint(collateral_mtx)};
    BOOST_REQUIRE(!collateral.hash.IsNull());

    // The user unlocks it while it is still an ordinary coin.
    {
        LOCK(wallet->cs_wallet);
        WalletBatch batch{wallet->GetDatabase()};
        BOOST_REQUIRE(wallet->LockCoinByUser(collateral, &batch));
        BOOST_REQUIRE(wallet->UnlockCoinByUser(collateral, &batch));
        BOOST_REQUIRE(wallet->IsAutoLockOptOut(collateral));
        BOOST_REQUIRE(!wallet->IsLockedCoin(collateral));
    }

    // Registering it as an external collateral makes that decision obsolete: it was made
    // about a coin that was not protected, and the collateral is live now.
    CKey owner_key;
    owner_key.MakeNewKey(true);
    CBLSSecretKey operator_key;
    operator_key.MakeNewKey();
    CMutableTransaction pro_reg_mtx{CreateProRegTxExternalCollateral(*m_node.chainman, utxos, /*port=*/1, collateral,
                                                                     wallet_script, owner_key, operator_key,
                                                                     coinbaseKey, coinbaseKey)};
    const CBlock block{CreateAndProcessBlock({pro_reg_mtx}, wallet_script)};
    const CBlockIndex* tip{WITH_LOCK(m_node.chainman->GetMutex(), return m_node.chainman->ActiveChain().Tip())};
    {
        LOCK(::cs_main);
        m_node.dmnman->UpdatedBlockTip(tip);
        BOOST_REQUIRE(m_node.dmnman->GetListAtChainTip().HasMNByCollateral(collateral));
    }
    ConnectToWallet(block);

    LOCK(wallet->cs_wallet);
    BOOST_CHECK(!wallet->IsAutoLockOptOut(collateral));
    BOOST_CHECK(wallet->IsLockedCoin(collateral));
}

BOOST_FIXTURE_TEST_CASE(SpentOptOutsAreNotRecheckedEachBlock, AvailableCoinsTestingSetup)
{
    LOCK(wallet->cs_wallet);

    const auto dest{wallet->GetNewDestination("")};
    BOOST_ASSERT(dest);

    // Shaped so that the chain would report it as a collateral, to show the spend is what
    // keeps it out of the per-block recheck rather than its shape.
    CProRegTx payload;
    payload.nVersion = ProTxVersion::GetMax(!bls::bls_legacy_scheme, /*is_extended_addr=*/false);
    payload.netInfo = NetInfoInterface::MakeNetInfo(payload.nVersion);
    payload.collateralOutpoint.n = 0;

    CMutableTransaction mtx;
    mtx.nVersion = 3;
    mtx.nType = TRANSACTION_PROVIDER_REGISTER;
    mtx.vin.emplace_back(COutPoint{uint256::ONE, 0});
    mtx.vout.emplace_back(dmn_types::Regular.collat_amount, GetScriptForDestination(*dest));
    SetTxPayload(mtx, payload);
    const CTransactionRef pro_reg_tx{MakeTransactionRef(mtx)};
    const COutPoint collateral{pro_reg_tx->GetHash(), 0};
    BOOST_REQUIRE(wallet->AddToWallet(pro_reg_tx, TxStateInMempool{}));
    wallet->LoadAutoLockOptOut(collateral, /*was_collateral=*/false);

    // Spending it puts the outpoint beyond registration for good.
    CMutableTransaction spend_mtx;
    spend_mtx.vin.emplace_back(collateral);
    spend_mtx.vout.emplace_back(dmn_types::Regular.collat_amount / 2, GetScriptForDestination(*dest));
    BOOST_REQUIRE(wallet->AddToWallet(MakeTransactionRef(spend_mtx), TxStateInMempool{}));
    BOOST_REQUIRE(wallet->IsSpent(collateral));

    const CBlock block{CreateAndProcessBlock({}, GetScriptForDestination(*dest))};
    ConnectToWallet(block);

    // The spent outpoint is never handed to the collateral lookup, and the record stays,
    // so abandoning the spend would bring the decision back into play.
    BOOST_CHECK(wallet->IsAutoLockOptOut(collateral));
    BOOST_CHECK(!wallet->IsLockedCoin(collateral));
}

BOOST_FIXTURE_TEST_CASE(DeliberateUnlockPrecedesDustProtection, AvailableCoinsTestingSetup)
{
    LOCK(wallet->cs_wallet);

    const auto dest{wallet->GetNewDestination("")};
    BOOST_ASSERT(dest);

    CMutableTransaction mtx;
    mtx.vin.emplace_back(COutPoint{uint256::ONE, 0});
    mtx.vout.emplace_back(COIN / 100, GetScriptForDestination(*dest));
    const CTransactionRef tx{MakeTransactionRef(mtx)};
    const COutPoint outpoint{tx->GetHash(), 0};

    // Dust protection is off, so nothing locks the output automatically.
    BOOST_CHECK(wallet->AddToWallet(tx, TxStateInMempool{}));
    BOOST_CHECK(!wallet->IsLockedCoin(outpoint));

    WalletBatch batch{wallet->GetDatabase()};
    BOOST_CHECK(wallet->LockCoinByUser(outpoint, &batch));
    BOOST_CHECK(wallet->UnlockCoinByUser(outpoint, &batch));

    // Turning dust protection on afterwards must not take the unlock back: an output can
    // become a target long after the user decided to spend it.
    wallet->m_dust_protection_threshold = 1 * COIN;
    wallet->LockExistingDustOutputs();
    BOOST_CHECK(!wallet->IsLockedCoin(outpoint));
}

BOOST_AUTO_TEST_SUITE_END()
} // namespace wallet
