// Copyright (c) 2019-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
#include <arith_uint256.h>
#include <banman.h>
#include <bls/bls.h>
#include <chainlock/chainlock.h>
#include <chainlock/clsig.h>
#include <chainparams.h>
#include <compat/endian.h>
#include <consensus/merkle.h>
#include <consensus/validation.h>
#include <crypto/sha256.h>
#include <evo/assetlocktx.h>
#include <evo/cbtx.h>
#include <evo/creditpool.h>
#include <evo/deterministicmns.h>
#include <evo/mnhftx.h>
#include <evo/snapshot.h>
#include <evo/snapshot_config.h>
#include <evo/specialtx.h>
#include <evo/specialtxman.h>
#include <governance/superblock.h>
#include <init.h>
#include <instantsend/net_instantsend.h>
#include <kernel/coinstats.h>
#include <kernel/disconnected_transactions.h>
#include <key.h>
#include <logging.h>
#include <masternode/meta.h>
#include <net.h>
#include <net_processing.h>
#include <netmessagemaker.h>
#include <node/chainstate.h>
#include <node/interface_ui.h>
#include <node/miner.h>
#include <node/utxo_snapshot.h>
#include <pow.h>
#include <protocol.h>
#include <random.h>
#include <rpc/blockchain.h>
#include <rpc/protocol.h>
#include <rpc/server.h>
#include <rpc/server_util.h>
#include <shutdown.h>
#include <streams.h>
#include <sync.h>
#include <test/util/chainstate.h>
#include <test/util/evo_snapshot.h>
#include <test/util/logging.h>
#include <test/util/masternode.h>
#include <test/util/net.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>
#include <timedata.h>
#include <uint256.h>
#include <util/strencodings.h>
#include <util/string.h>
#include <util/translation.h>
#include <validation.h>
#include <validationinterface.h>
#include <versionbits.h>
#include <warnings.h>

#include <evo/evodb.h>
#include <llmq/blockprocessor.h>
#include <llmq/commitment.h>
#include <llmq/context.h>
#include <llmq/net_signing.h>
#include <llmq/options.h>
#include <llmq/quorumsman.h>
#include <llmq/signhash.h>
#include <llmq/signing.h>
#include <llmq/signing_shares.h>
#include <llmq/snapshot.h>
#include <llmq/utils.h>
#include <node/blockstorage.h>

#include <tinyformat.h>

#include <optional>
#include <type_traits>
#include <vector>

#include <boost/signals2/connection.hpp>
#include <boost/test/unit_test.hpp>

using node::SnapshotMetadata;

namespace {

//! Point the snapshot generation's best-block marker at `hash`, first
//! publishing a generation based at `hash` if none is live.
void SeedSnapshotMarker(CEvoDB& evodb, const uint256& hash, int base_height = -1)
{
    LOCK(::cs_main);
    if (!evodb.GetSnapshotGeneration()) {
        BOOST_REQUIRE(evodb.CreateSnapshotGeneration(hash, base_height));
        BOOST_REQUIRE(evodb.PublishSnapshotGeneration(EvoSnapshotConsensusConfig::FromParams(Params()).Fingerprint(),
                                                      std::nullopt));
    }
    auto tx = evodb.BeginTransaction(EvoDbIdentity::SNAPSHOT);
    evodb.WriteBestBlock(EvoDbIdentity::SNAPSHOT, hash);
    tx->Commit();
    BOOST_REQUIRE(evodb.CommitRootTransaction(EvoDbIdentity::SNAPSHOT));
}

//! The regtest assumeutxo fixture predates DIP3. Activate DIP3 from height 1
//! for the lifetime of this guard, so the cold EvoDB holds no MN-list history
//! for blocks that now count as DIP3-active.
class ScopedDIP3FromGenesis
{
    Consensus::Params& m_params{const_cast<Consensus::Params&>(Params().GetConsensus())};
    const int m_old_height{m_params.DIP0003Height};

public:
    ScopedDIP3FromGenesis() { m_params.DIP0003Height = 1; }
    ~ScopedDIP3FromGenesis() { m_params.DIP0003Height = m_old_height; }
};

//! The section seeded at a pre-DIP3 base proves that every scan sequence ends
//! there. With DIP3 moved below the base (ScopedDIP3FromGenesis) that proof no
//! longer holds, so drop it. The key mirrors DB_SCAN_COVERAGE in
//! llmq/blockprocessor.cpp (file-local there).
void ForgetSeededScanCoverage(CEvoDB& evodb)
{
    LOCK(::cs_main);
    {
        auto tx = evodb.BeginTransaction(EvoDbIdentity::SNAPSHOT);
        for (const auto& params : Params().GetConsensus().llmqs)
            evodb.Erase(std::make_pair(std::string{"q_scov"}, params.type));
        tx->Commit();
    }
    BOOST_REQUIRE(evodb.CommitRootTransaction(EvoDbIdentity::SNAPSHOT));
}

//! Turn a pre-DIP3 template into one whose coinbase is a CbTx, as required
//! once DIP3 is active, and solve it again.
CBlock MakeCbTxBlock(CBlock block, int height, CCbTx::Version version = CCbTx::Version::MERKLE_ROOT_QUORUMS)
{
    CCbTx cbtx;
    cbtx.nVersion = version;
    cbtx.nHeight = height;
    CMutableTransaction coinbase{*block.vtx[0]};
    coinbase.nVersion = CTransaction::SPECIAL_VERSION;
    coinbase.nType = TRANSACTION_COINBASE;
    SetTxPayload(coinbase, cbtx);
    block.vtx[0] = MakeTransactionRef(std::move(coinbase));
    block.hashMerkleRoot = BlockMerkleRoot(block);
    while (!CheckProofOfWork(block.GetHash(), block.nBits, Params().GetConsensus()))
        ++block.nNonce;
    return block;
}

// Simulate a restart of the node by optionally flushing all state to disk,
// clearing the existing ChainstateManager, and unloading the block index.
//
// @returns a reference to the "restarted" ChainstateManager
ChainstateManager& RestartChainstateManager(node::NodeContext& node, bool flush_chainstates)
{
    ChainstateManager& chainman = *Assert(node.chainman);

    BOOST_TEST_MESSAGE("Simulating node restart");
    {
        if (flush_chainstates) {
            for (Chainstate* cs : chainman.GetAll()) {
                LOCK(::cs_main);
                cs->ForceFlushStateToDisk();
            }
        }
        // Process all callbacks referring to the old manager before wiping it.
        SyncWithValidationInterfaceQueue();
        LOCK(::cs_main);
        node.special_tx.reset();
        node.llmq_ctx.reset();
        chainman.ResetChainstates();
        BOOST_CHECK_EQUAL(chainman.GetAll().size(), 0);
        const ChainstateManager::Options chainman_opts{
            .chainparams = ::Params(),
            .datadir = Assert(node.args)->GetDataDirNet(),
            .superblock_status = [&sbman = *node.sbman](const CDeterministicMNList& mn_list, int height) { return sbman.GetStatus(mn_list, height); },
            .mn_sync = node.mn_sync.get(),
        };
        // For robustness, ensure the old manager is destroyed before creating a
        // new one.
        node.chainman.reset();
        node.chainman.reset(new ChainstateManager(chainman_opts));
    }
    return *Assert(node.chainman);
}

//! Counts the masternode-list changes validation announces.
class MnListChangeCounter final : public CValidationInterface
{
public:
    int changes{0};

protected:
    void NotifyMasternodeListChanged(bool, const CDeterministicMNList&, const CDeterministicMNListDiff&) override
    {
        ++changes;
    }
};

//! The last tip validation announced, synchronously and through the queue.
class TipAnnouncements final : public CValidationInterface
{
public:
    const CBlockIndex* synchronous{nullptr};
    const CBlockIndex* queued{nullptr};

protected:
    void SynchronousUpdatedBlockTip(const CBlockIndex* new_tip, const CBlockIndex*, bool) override
    {
        synchronous = new_tip;
    }
    void UpdatedBlockTip(const CBlockIndex* new_tip, const CBlockIndex*, bool) override { queued = new_tip; }
};

//! The validation result announced for one block.
class BlockCheckedCatcher final : public CValidationInterface
{
public:
    explicit BlockCheckedCatcher(const uint256& hash) :
        m_hash{hash}
    {
    }
    std::optional<BlockValidationState> state{};

protected:
    void BlockChecked(const CBlock& block, const BlockValidationState& checked) override
    {
        if (block.GetHash() == m_hash) state = checked;
    }

private:
    const uint256 m_hash{};
};

} // namespace

//! Reaches InstantSend's conflicting-block invalidation.
struct NetInstantSendConflictsTest {
    //! Invalidate `block_hash` for a lock whose transaction is unknown.
    static void InvalidateTemporarily(NetInstantSend& net, const uint256& block_hash)
    {
        net.InvalidateConflictingBlocks(GetRandHash(), {.blocks = {block_hash}, .has_tx_for_lock = false});
    }
};

BOOST_FIXTURE_TEST_SUITE(validation_chainstatemanager_tests, ChainTestingSetup)

//! Basic tests for ChainstateManager.
//!
//! First create a legacy (IBD) chainstate, then create a snapshot chainstate.
BOOST_AUTO_TEST_CASE(chainstatemanager)
{
    ChainstateManager& manager = *m_node.chainman;
    CTxMemPool& mempool = *m_node.mempool;
    CEvoDB& evodb = *m_node.evodb;
    std::vector<Chainstate*> chainstates;

    BOOST_CHECK(!manager.SnapshotBlockhash().has_value());

    // Create a legacy (IBD) chainstate.
    //
    MakeDashChainContexts(/*llmq_dbs_wipe=*/false);
    Chainstate& c1 = WITH_LOCK(::cs_main, return manager.InitializeChainstate(&mempool, evodb, *Assert(m_node.special_tx),
                                                                              *Assert(m_node.chainlocks), m_node.isman.get()));
    chainstates.push_back(&c1);
    c1.InitCoinsDB(
        /*cache_size_bytes=*/1 << 23, /*in_memory=*/true, /*should_wipe=*/false);
    WITH_LOCK(::cs_main, c1.InitCoinsCache(1 << 23));

    BOOST_REQUIRE(c1.LoadGenesisBlock());
    BlockValidationState val_state;
    BOOST_CHECK(c1.ActivateBestChain(val_state, nullptr));

    BOOST_CHECK(!manager.IsSnapshotActive());
    BOOST_CHECK(!manager.IsSnapshotActiveAndUnvalidated());
    BOOST_CHECK(llmq::CSigSharesManager::IsQuorumSigningAllowed(manager));
    BOOST_CHECK(WITH_LOCK(::cs_main, return !manager.IsSnapshotValidated()));
    auto all = manager.GetAll();
    BOOST_CHECK_EQUAL_COLLECTIONS(all.begin(), all.end(), chainstates.begin(), chainstates.end());

    auto& active_chain = WITH_LOCK(manager.GetMutex(), return manager.ActiveChain());
    BOOST_CHECK_EQUAL(&active_chain, &c1.m_chain);

    BOOST_CHECK_EQUAL(WITH_LOCK(manager.GetMutex(), return manager.ActiveHeight()), 0);

    auto active_tip = WITH_LOCK(manager.GetMutex(), return manager.ActiveTip());
    auto exp_tip = c1.m_chain.Tip();
    BOOST_CHECK_EQUAL(active_tip, exp_tip);

    BOOST_CHECK(!manager.SnapshotBlockhash().has_value());

    // Create a snapshot-based chainstate.
    //
    const uint256 snapshot_blockhash = active_tip->GetBlockHash();
    SeedSnapshotMarker(evodb, snapshot_blockhash);
    Chainstate* c2_ptr = WITH_LOCK(::cs_main, return manager.ActivateExistingSnapshot(
        &mempool, snapshot_blockhash));
    BOOST_REQUIRE(c2_ptr);
    Chainstate& c2 = *c2_ptr;
    chainstates.push_back(&c2);

    // Only the active chainstate keeps the mempool.
    BOOST_CHECK_EQUAL(c2.GetMempool(), &mempool);
    BOOST_CHECK(!c1.GetMempool());

    BOOST_CHECK_EQUAL(manager.SnapshotBlockhash().value(), snapshot_blockhash);

    c2.InitCoinsDB(
        /*cache_size_bytes=*/1 << 23, /*in_memory=*/true, /*should_wipe=*/false);
    WITH_LOCK(::cs_main, c2.InitCoinsCache(1 << 23));
    c2.m_chain.SetTip(*active_tip);
    BlockValidationState _;
    BOOST_CHECK(c2.ActivateBestChain(_, nullptr));

    BOOST_CHECK(manager.IsSnapshotActive());
    BOOST_CHECK(WITH_LOCK(::cs_main, return !manager.IsSnapshotValidated()));
    BOOST_CHECK(manager.IsSnapshotActiveAndUnvalidated());
    BOOST_CHECK(!llmq::CSigSharesManager::IsQuorumSigningAllowed(manager));
    BOOST_CHECK_EQUAL(&c2, &manager.ActiveChainstate());
    BOOST_CHECK(&c1 != &manager.ActiveChainstate());
    auto all2 = manager.GetAll();
    BOOST_CHECK_EQUAL_COLLECTIONS(all2.begin(), all2.end(), chainstates.begin(), chainstates.end());

    auto& active_chain2 = WITH_LOCK(manager.GetMutex(), return manager.ActiveChain());
    BOOST_CHECK_EQUAL(&active_chain2, &c2.m_chain);

    BOOST_CHECK_EQUAL(WITH_LOCK(manager.GetMutex(), return manager.ActiveHeight()), 0);

    auto active_tip2 = WITH_LOCK(manager.GetMutex(), return manager.ActiveTip());
    auto exp_tip2 = c2.m_chain.Tip();
    BOOST_CHECK_EQUAL(active_tip2, exp_tip2);

    BOOST_CHECK_EQUAL(exp_tip, exp_tip2);

    // Let scheduler events finish running to avoid accessing memory that is going to be unloaded
    SyncWithValidationInterfaceQueue();

    m_node.special_tx.reset();
    m_node.llmq_ctx.reset();
    // dmnman holds a reference to m_node.evodb, it mustn't outlive it
    m_node.dmnman.reset();
}

//! Test rebalancing the caches associated with each chainstate.
BOOST_FIXTURE_TEST_CASE(chainstatemanager_rebalance_caches, TestChain100Setup)
{
    ChainstateManager& manager = *m_node.chainman;
    CTxMemPool& mempool = *m_node.mempool;
    CEvoDB& evodb = *m_node.evodb;
    size_t max_cache = 10000;
    manager.m_total_coinsdb_cache = max_cache;
    manager.m_total_coinstip_cache = max_cache;

    std::vector<Chainstate*> chainstates;

    // Create a legacy (IBD) chainstate.
    //
    Chainstate& c1 = manager.ActiveChainstate();
    chainstates.push_back(&c1);
    c1.InitCoinsDB(
        /*cache_size_bytes=*/1 << 23, /*in_memory=*/true, /*should_wipe=*/false);

    {
        LOCK(::cs_main);
        c1.InitCoinsCache(1 << 23);
        manager.MaybeRebalanceCaches();
    }

    BOOST_CHECK_EQUAL(c1.m_coinstip_cache_size_bytes, max_cache);
    BOOST_CHECK_EQUAL(c1.m_coinsdb_cache_size_bytes, max_cache);

    // Create a snapshot-based chainstate.
    //
    CBlockIndex* snapshot_base{WITH_LOCK(manager.GetMutex(), return manager.ActiveChain()[manager.ActiveChain().Height() / 2])};
    SeedSnapshotMarker(evodb, snapshot_base->GetBlockHash());
    Chainstate* c2_ptr = WITH_LOCK(cs_main, return manager.ActivateExistingSnapshot(&mempool, snapshot_base->GetBlockHash()));
    BOOST_REQUIRE(c2_ptr);
    Chainstate& c2 = *c2_ptr;
    chainstates.push_back(&c2);
    c2.InitCoinsDB(
        /*cache_size_bytes=*/1 << 23, /*in_memory=*/true, /*should_wipe=*/false);

    {
        LOCK(::cs_main);
        c2.InitCoinsCache(1 << 23);
        manager.MaybeRebalanceCaches();
    }

    // Since both chainstates are considered to be in initial block download,
    // the snapshot chainstate should take priority.
    BOOST_CHECK_CLOSE(c1.m_coinstip_cache_size_bytes, max_cache * 0.05, 1);
    BOOST_CHECK_CLOSE(c1.m_coinsdb_cache_size_bytes, max_cache * 0.05, 1);
    BOOST_CHECK_CLOSE(c2.m_coinstip_cache_size_bytes, max_cache * 0.95, 1);
    BOOST_CHECK_CLOSE(c2.m_coinsdb_cache_size_bytes, max_cache * 0.95, 1);
}

struct SnapshotTestSetup : TestChain100Setup {
    // Run with coinsdb on the filesystem to support, e.g., moving invalidated
    // chainstate dirs to "*_invalid".
    //
    // Note that this means the tests run considerably slower than in-memory DB
    // tests, but we can't otherwise test this functionality since it relies on
    // destructive filesystem operations.
    SnapshotTestSetup() : TestChain100Setup{
                              CBaseChainParams::REGTEST,
                              {},
                              /*coins_db_in_memory=*/false,
                              /*block_tree_db_in_memory=*/false,
                              /*dash_dbs_in_memory=*/false,
                          }
    {
    }

    std::tuple<Chainstate*, Chainstate*> SetupSnapshot()
    {
        ChainstateManager& chainman = *Assert(m_node.chainman);

        BOOST_CHECK(!chainman.IsSnapshotActive());

        {
            LOCK(::cs_main);
            BOOST_CHECK(!chainman.IsSnapshotValidated());
            BOOST_CHECK(!node::FindSnapshotChainstateDir());
        }

        size_t initial_size;
        size_t initial_total_coins{100};

        // Make some initial assertions about the contents of the chainstate.
        {
            LOCK(::cs_main);
            CCoinsViewCache& ibd_coinscache = chainman.ActiveChainstate().CoinsTip();
            initial_size = ibd_coinscache.GetCacheSize();
            size_t total_coins{0};

            for (CTransactionRef& txn : m_coinbase_txns) {
                COutPoint op{txn->GetHash(), 0};
                BOOST_CHECK(ibd_coinscache.HaveCoin(op));
                total_coins++;
            }

            BOOST_CHECK_EQUAL(total_coins, initial_total_coins);
            BOOST_CHECK_EQUAL(initial_size, initial_total_coins);
        }

        Chainstate& validation_chainstate = chainman.ActiveChainstate();

        // Snapshot should refuse to load at this height.
        BOOST_REQUIRE(!CreateAndActivateUTXOSnapshot(this));
        BOOST_CHECK(!chainman.ActiveChainstate().m_from_snapshot_blockhash);
        BOOST_CHECK(!chainman.SnapshotBlockhash());

        // Mine 10 more blocks, putting at us height 110 where a valid assumeutxo value can
        // be found.
        constexpr int snapshot_height = 110;
        mineBlocks(10);
        initial_size += 10;
        initial_total_coins += 10;

        // Should not load malleated snapshots
        BOOST_REQUIRE(!CreateAndActivateUTXOSnapshot(
            this, [](AutoFile& auto_infile, SnapshotMetadata& metadata) {
                // A UTXO is missing but count is correct
                metadata.m_coins_count -= 1;

                COutPoint outpoint;
                Coin coin;

                auto_infile >> outpoint;
                auto_infile >> coin;
        }));

        BOOST_CHECK(!node::FindSnapshotChainstateDir());

        BOOST_REQUIRE(!CreateAndActivateUTXOSnapshot(
            this, [](AutoFile& auto_infile, SnapshotMetadata& metadata) {
                // Coins count is larger than coins in file
                metadata.m_coins_count += 1;
        }));
        BOOST_REQUIRE(!CreateAndActivateUTXOSnapshot(
            this, [](AutoFile& auto_infile, SnapshotMetadata& metadata) {
                // Coins count is smaller than coins in file
                metadata.m_coins_count -= 1;
        }));
        BOOST_REQUIRE(!CreateAndActivateUTXOSnapshot(
            this, [](AutoFile& auto_infile, SnapshotMetadata& metadata) {
                // Wrong hash
                metadata.m_base_blockhash = uint256::ZERO;
        }));
        BOOST_REQUIRE(!CreateAndActivateUTXOSnapshot(
            this, [](AutoFile& auto_infile, SnapshotMetadata& metadata) {
                // Wrong hash
                metadata.m_base_blockhash = uint256::ONE;
        }));

        BOOST_REQUIRE(CreateAndActivateUTXOSnapshot(this));
        BOOST_CHECK(fs::exists(*node::FindSnapshotChainstateDir()));

        // Ensure our active chain is the snapshot chainstate.
        BOOST_CHECK(!chainman.ActiveChainstate().m_from_snapshot_blockhash->IsNull());
        BOOST_CHECK_EQUAL(
            *chainman.ActiveChainstate().m_from_snapshot_blockhash,
            *chainman.SnapshotBlockhash());

        Chainstate& snapshot_chainstate = chainman.ActiveChainstate();

        // To be checked against later when we try loading a subsequent snapshot.
        uint256 loaded_snapshot_blockhash{*chainman.SnapshotBlockhash()};

        BOOST_CHECK(m_node.evodb->VerifyBestBlock(
            EvoDbIdentity::SNAPSHOT, loaded_snapshot_blockhash));
        BOOST_CHECK(m_node.evodb->VerifyBestBlock(
            EvoDbIdentity::NORMAL, loaded_snapshot_blockhash));
        BOOST_CHECK(m_node.evodb->HasDualChainstateMarker());

        {
            LOCK(::cs_main);

            fs::path found = *node::FindSnapshotChainstateDir();

            // Note: WriteSnapshotBaseBlockhash() is implicitly tested above.
            BOOST_CHECK_EQUAL(
                *node::ReadSnapshotBaseBlockhash(found),
                *chainman.SnapshotBlockhash());

            // Ensure that the genesis block was not marked assumed-valid.
            BOOST_CHECK(!chainman.ActiveChain().Genesis()->IsAssumedValid());
        }

        const AssumeutxoData& au_data = *ExpectedAssumeutxo(snapshot_height, ::Params());
        const CBlockIndex* tip = WITH_LOCK(chainman.GetMutex(), return chainman.ActiveTip());

        BOOST_CHECK_EQUAL(tip->nChainTx, au_data.nChainTx);

        // Make some assertions about the both chainstates. These checks ensure the
        // legacy chainstate hasn't changed and that the newly created chainstate
        // reflects the expected content.
        {
            LOCK(::cs_main);
            int chains_tested{0};

            for (Chainstate* chainstate : chainman.GetAll()) {
                BOOST_TEST_MESSAGE("Checking coins in " << chainstate->ToString());
                CCoinsViewCache& coinscache = chainstate->CoinsTip();

                // Both caches will be empty initially.
                BOOST_CHECK_EQUAL((unsigned int)0, coinscache.GetCacheSize());

                size_t total_coins{0};

                for (CTransactionRef& txn : m_coinbase_txns) {
                    COutPoint op{txn->GetHash(), 0};
                    BOOST_CHECK(coinscache.HaveCoin(op));
                    total_coins++;
                }

                BOOST_CHECK_EQUAL(initial_size , coinscache.GetCacheSize());
                BOOST_CHECK_EQUAL(total_coins, initial_total_coins);
                chains_tested++;
            }

            BOOST_CHECK_EQUAL(chains_tested, 2);
        }

        // Mine some new blocks on top of the activated snapshot chainstate.
        constexpr size_t new_coins{100};
        mineBlocks(new_coins);  // Defined in TestChain100Setup.

        const uint256 snapshot_tip = WITH_LOCK(chainman.GetMutex(), return chainman.ActiveTip()->GetBlockHash());
        BOOST_CHECK(m_node.evodb->VerifyBestBlock(EvoDbIdentity::SNAPSHOT, snapshot_tip));
        BOOST_CHECK(m_node.evodb->VerifyBestBlock(EvoDbIdentity::NORMAL, loaded_snapshot_blockhash));

        {
            LOCK(::cs_main);
            size_t coins_in_active{0};
            size_t coins_in_background{0};
            size_t coins_missing_from_background{0};

            for (Chainstate* chainstate : chainman.GetAll()) {
                BOOST_TEST_MESSAGE("Checking coins in " << chainstate->ToString());
                CCoinsViewCache& coinscache = chainstate->CoinsTip();
                bool is_background = chainstate != &chainman.ActiveChainstate();

                for (CTransactionRef& txn : m_coinbase_txns) {
                    COutPoint op{txn->GetHash(), 0};
                    if (coinscache.HaveCoin(op)) {
                        (is_background ? coins_in_background : coins_in_active)++;
                    } else if (is_background) {
                        coins_missing_from_background++;
                    }
                }
            }

            BOOST_CHECK_EQUAL(coins_in_active, initial_total_coins + new_coins);
            BOOST_CHECK_EQUAL(coins_in_background, initial_total_coins);
            BOOST_CHECK_EQUAL(coins_missing_from_background, new_coins);
        }

        // Snapshot should refuse to load after one has already loaded.
        BOOST_REQUIRE(!CreateAndActivateUTXOSnapshot(this));

        // Snapshot blockhash should be unchanged.
        BOOST_CHECK_EQUAL(
            *chainman.ActiveChainstate().m_from_snapshot_blockhash,
            loaded_snapshot_blockhash);
        return std::make_tuple(&validation_chainstate, &snapshot_chainstate);
    }

    // Simulate a restart of the node by optionally flushing all state to disk,
    // clearing the existing ChainstateManager, and unloading the block index.
    //
    // @returns a reference to the "restarted" ChainstateManager
    ChainstateManager& SimulateNodeRestart(bool flush_chainstates = true)
    {
        return RestartChainstateManager(m_node, flush_chainstates);
    }

    //! Execute an RPC command the way the RPC server dispatches it.
    UniValue CallRPC(const std::string& method, const UniValue& params = UniValue{UniValue::VARR})
    {
        JSONRPCRequest request;
        request.context = CoreContext{m_node};
        request.strMethod = method;
        request.params = params;
        if (RPCIsInWarmup(nullptr)) SetRPCWarmupFinished();
        return tableRPC.execute(request);
    }

    //! Load the chainstates like startup does, reporting failure instead of
    //! asserting success.
    node::ChainstateLoadResult TryLoadChainstate()
    {
        MakeDashChainContexts(/*llmq_dbs_wipe=*/false);
        return node::LoadChainstate(*Assert(m_node.chainman), m_cache_sizes, ChainstateLoadOptionsForTest(),
                                    *m_node.evodb, *m_node.dmnman, *m_node.special_tx);
    }

    //! What the snapshot chainstate built past DIP3 activation, by hash: the
    //! block index is reloaded across restarts.
    struct SnapshotChainPastDip3 {
        uint256 protx_hash;
        uint256 protx_block;
        uint256 period_block; //!< at a multiple of DISK_SNAPSHOT_PERIOD
        uint256 tip;
    };

    //! Extend the active snapshot chain past DIP3 and V20 activation (432) and
    //! the first disk-snapshot period (576), registering a masternode on the
    //! way. Block connection then writes real MN-list diffs (dmn_D4), a full
    //! list and a credit pool (dmn_S3, cpm_S) under the snapshot generation.
    SnapshotChainPastDip3 MineSnapshotChainPastDip3()
    {
        ChainstateManager& chainman{*Assert(m_node.chainman)};
        const auto mine_to{[&](int height) {
            while (WITH_LOCK(::cs_main, return chainman.ActiveHeight()) < height)
                mineBlocks(1);
        }};
        const int dip3_height{Params().GetConsensus().DIP0003Height};
        mine_to(dip3_height);

        auto utxos{BuildSimpleUtxoMap(m_coinbase_txns)};
        CKey owner_key;
        CBLSSecretKey operator_key;
        const auto protx{
            CreateProRegTx(chainman, utxos, /*port=*/1, GenerateRandomAddress(), coinbaseKey, owner_key, operator_key)};
        const CBlock protx_block{CreateAndProcessBlock({protx}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()))};
        BOOST_REQUIRE_EQUAL(WITH_LOCK(::cs_main, return chainman.ActiveTip()->GetBlockHash()), protx_block.GetHash());

        // DISK_SNAPSHOT_PERIOD of both the MN-list and credit-pool managers.
        constexpr int period{576};
        mine_to(period + 1);
        LOCK(::cs_main);
        return {protx.GetHash(), protx_block.GetHash(), chainman.ActiveChain()[period]->GetBlockHash(),
                chainman.ActiveTip()->GetBlockHash()};
    }
};

using MinedQuorumCommitmentRef = evo::MinedQuorumCommitment&;

evo::QuorumSnapshotData& EvoSnapshotChainSetupType(evo::EvoSnapshot& snapshot, Consensus::LLMQType type)
{
    const auto it{std::ranges::find_if(snapshot.quorums, [type](const auto& data) { return data.llmq_type == type; })};
    BOOST_REQUIRE(it != snapshot.quorums.end());
    return *it;
}

//! Replaces the assumeutxo entry at a height for the lifetime of the guard.
class ScopedAssumeutxoEntry
{
    MapAssumeutxo& m_map{const_cast<MapAssumeutxo&>(Params().Assumeutxo())};
    const int m_height;
    std::optional<AssumeutxoData> m_saved;

public:
    ScopedAssumeutxoEntry(int height, const AssumeutxoData& data) :
        m_height{height}
    {
        if (const auto it{m_map.find(height)}; it != m_map.end()) {
            m_saved.emplace(it->second);
            m_map.erase(it);
        }
        m_map.emplace(height, data);
    }
    ~ScopedAssumeutxoEntry()
    {
        m_map.erase(m_height);
        if (m_saved) m_map.emplace(m_height, *m_saved);
    }
};

/**
 * A chain with DIP3, v20 and MN_RR active from height 130 and four masternodes
 * registered with fixed keys at height 131, so its blocks, and with them the
 * UTXO and evo pins of the snapshot base, are the same in every run. The
 * blocks before DIP3 fund the collaterals. No DKG runs, so every quorum type
 * has an empty history; the rotating type stores a quarter for each cycle from
 * 144 on, and the base at 219 needs those of 144 to 216.
 */
struct EvoSnapshotTestSetup : public TestChainSetup {
    static constexpr int DIP3_HEIGHT{130};
    static constexpr int BASE_HEIGHT{219};
    static constexpr int MN_COUNT{4};
    //! The pins of the base. The evo hash was derived from the NORMAL state of
    //! a node that never loaded a snapshot, and checked against the SHA256 of
    //! the dumped section bytes.
    static constexpr const char* PINS{"-assumeutxodata=219:"
                                      "0b8154835ac26c0a9b495bac7655dcad1cff6c6ca66adadb2168e842d3ee14e9:"
                                      "d5900cd54a2d1321b2565aeb3313628c8dbbfd622615325b6fdbea8ac53b35a9:"
                                      "368:"
                                      "72b8059db75889556ab069c31b3c10e3d951771422176b4d02a05c8348a354c4"};

    //! The operator key of every registered masternode, by ProRegTx hash.
    std::map<uint256, CBLSSecretKey> m_operator_keys;
    int m_dumps{0};

    EvoSnapshotTestSetup() :
        EvoSnapshotTestSetup{/*evo_count=*/0}
    {
        MineTo(BASE_HEIGHT);
    }
    virtual ~EvoSnapshotTestSetup() = default;

protected:
    //! Register MN_COUNT masternodes, the last `evo_count` of them evonodes.
    explicit EvoSnapshotTestSetup(int evo_count) :
        TestChainSetup{100,
                       CBaseChainParams::REGTEST,
                       {"-dip3params=130:130", "-testactivationheight=v20@130", "-testactivationheight=mn_rr@130", PINS},
                       /*coins_db_in_memory=*/false,
                       /*block_tree_db_in_memory=*/false,
                       /*dash_dbs_in_memory=*/false}
    {
        MineTo(DIP3_HEIGHT);
        auto utxos{BuildSimpleUtxoMap(m_coinbase_txns)};
        std::vector<CMutableTransaction> registrations;
        for (int i{0}; i < MN_COUNT; ++i) {
            const auto key = [i](unsigned char role) {
                std::array<unsigned char, 32> secret{};
                secret[30] = role;
                secret[31] = static_cast<unsigned char>(i + 1);
                return secret;
            };
            CKey owner_key;
            const auto owner_secret{key(1)};
            owner_key.Set(owner_secret.begin(), owner_secret.end(), /*fCompressedIn=*/true);
            CKey payout_key;
            const auto payout_secret{key(2)};
            payout_key.Set(payout_secret.begin(), payout_secret.end(), /*fCompressedIn=*/true);
            const CBLSSecretKey operator_key{key(3)};
            const MnType type{i < MN_COUNT - evo_count ? MnType::Regular : MnType::Evo};
            registrations.push_back(CreateProRegTx(*m_node.chainman, utxos, /*port=*/1 + i,
                                                   GetScriptForDestination(PKHash(payout_key.GetPubKey())), coinbaseKey,
                                                   owner_key, operator_key, type));
            m_operator_keys.emplace(registrations.back().GetHash(), operator_key);
        }
        CreateAndProcessBlock(registrations, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
    }

public:
    virtual void MineOneBlock() { mineBlocks(1); }

    void MineTo(int height)
    {
        while (WITH_LOCK(::cs_main, return m_node.chainman->ActiveHeight()) < height)
            MineOneBlock();
    }

    //! The pin of the base the next dump is for, derived from NORMAL state.
    std::optional<ScopedAssumeutxoEntry> m_pin;

    std::optional<evo::EvoSnapshot> BuildEvoSnapshotAtTip() EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
    {
        evo::EvoSnapshot section;
        std::string error;
        const EvoViewScope normal{m_node.evodb->ViewFor(EvoDbIdentity::NORMAL)};
        if (!evo::BuildEvoSnapshot(Params(), *m_node.chainman, *m_node.dmnman, *m_node.llmq_ctx->quorum_block_processor,
                                   *m_node.llmq_ctx->qsnapman, *m_node.special_tx->m_cpoolman,
                                   *m_node.special_tx->m_mnhfman, m_node.chainman->ActiveTip(), section, error)) {
            BOOST_ERROR(error);
            return std::nullopt;
        }
        return section;
    }

    //! Pin the base at the tip, for this deployment configuration, from the
    //! NORMAL state of this node, which has not loaded a snapshot.
    void PinTip()
    {
        m_pin.reset();
        LOCK(::cs_main);
        Chainstate& chainstate{m_node.chainman->ActiveChainstate()};
        const CBlockIndex* base{chainstate.m_chain.Tip()};
        evo::EvoSnapshot section;
        std::string error;
        {
            const EvoViewScope normal{m_node.evodb->ViewFor(EvoDbIdentity::NORMAL)};
            BOOST_REQUIRE_MESSAGE(evo::BuildEvoSnapshot(Params(), *m_node.chainman, *m_node.dmnman,
                                                        *m_node.llmq_ctx->quorum_block_processor,
                                                        *m_node.llmq_ctx->qsnapman, *m_node.special_tx->m_cpoolman,
                                                        *m_node.special_tx->m_mnhfman, base, section, error),
                                  error);
        }
        chainstate.ForceFlushStateToDisk();
        const auto stats{*kernel::ComputeUTXOStats(kernel::CoinStatsHashType::HASH_SERIALIZED, &chainstate.CoinsDB(),
                                                   m_node.chainman->m_blockman)};
        m_pin.emplace(base->nHeight, AssumeutxoData{AssumeutxoHash{stats.hashSerialized},
                                                    EvoSnapshotHash{evo::GetEvoSnapshotHash(section)}, base->nChainTx,
                                                    base->GetBlockHash()});
    }

    std::optional<uint32_t> NextGeneration()
    {
        uint32_t next;
        if (!m_node.evodb->GetRawDB().Read(EVODB_GENERATION_NEXT, next)) return std::nullopt;
        return next;
    }

    //! Dump the active chainstate, apply `mutate` to its evo section (absent
    //! for a UTXO-only file) and activate the result.
    bool ActivateEvoSnapshot(const std::function<void(std::optional<evo::EvoSnapshot>&)>& mutate = nullptr)
    {
        return LoadEvoSnapshot(DumpEvoSnapshot(mutate));
    }

    bool LoadEvoSnapshot(const fs::path& path)
    {
        AutoFile in{fsbridge::fopen(path, "rb")};
        node::SnapshotMetadata metadata;
        in >> metadata;
        return m_node.chainman->ActivateSnapshot(in, metadata, /*in_memory=*/false);
    }

    ChainstateManager& SimulateNodeRestart() { return RestartChainstateManager(m_node, /*flush_chainstates=*/true); }

    node::ChainstateLoadResult TryLoadChainstate()
    {
        MakeDashChainContexts(/*llmq_dbs_wipe=*/false);
        return node::LoadChainstate(*Assert(m_node.chainman), m_cache_sizes, ChainstateLoadOptionsForTest(),
                                    *m_node.evodb, *m_node.dmnman, *m_node.special_tx);
    }

    void Restart()
    {
        SimulateNodeRestart();
        BOOST_REQUIRE(std::get<0>(TryLoadChainstate()) == node::ChainstateLoadStatus::SUCCESS);
    }

    //! The blocks this node mined, by height, for a node that has to download
    //! them again.
    std::vector<std::shared_ptr<const CBlock>> m_blocks;

    //! Forget every block body and roll the chainstate back to genesis, as on
    //! a node that only has the headers, keeping the bodies in m_blocks.
    //! `keep_caches` keeps what NORMAL cached in memory, as on a node whose
    //! own sync got there before it loaded the snapshot.
    void ForgetBlocks(bool keep_caches = false)
    {
        LOCK(::cs_main);
        ChainstateManager& chainman{*m_node.chainman};
        const CBlockIndex* tip{chainman.ActiveTip()};
        m_blocks.assign(tip->nHeight + 1, nullptr);
        for (const CBlockIndex* index{tip}; index; index = index->pprev) {
            auto block{std::make_shared<CBlock>()};
            BOOST_REQUIRE(node::ReadBlockFromDisk(*block, index, chainman.GetConsensus()));
            m_blocks[index->nHeight] = std::move(block);
        }
        // Nothing NORMAL derived may stay behind in memory either.
        chainman.ActiveChainstate().ForceFlushStateToDisk();
        BOOST_REQUIRE(m_node.evodb->CommitRootTransaction(EvoDbIdentity::NORMAL, /*sync=*/true));
        // As in CreateAndActivateUTXOSnapshot(reset_chainstate=true).
        CBlockIndex* orig_tip{chainman.ActiveTip()};
        const uint256 genesis{chainman.ActiveChain()[0]->GetBlockHash()};
        chainman.ResetChainstates();
        chainman.InitializeChainstate(m_node.mempool.get(), *m_node.evodb, *m_node.special_tx, *m_node.chainlocks,
                                      m_node.isman.get());
        Chainstate& chainstate{chainman.ActiveChainstate()};
        BOOST_REQUIRE(chainstate.LoadGenesisBlock());
        chainstate.InitCoinsDB(1 << 20, /*in_memory=*/false, /*should_wipe=*/true);
        chainstate.InitCoinsCache(1 << 20);
        chainstate.CoinsTip().SetBestBlock(genesis);
        chainstate.setBlockIndexCandidates.insert(chainman.m_blockman.LookupBlockIndex(genesis));
        chainstate.LoadChainTip();
        chainman.MaybeRebalanceCaches();
        for (CBlockIndex* index{orig_tip}; index && index != chainstate.m_chain.Tip(); index = index->pprev) {
            index->nStatus &= ~(BLOCK_HAVE_DATA | BLOCK_HAVE_UNDO);
            index->nStatus |= BLOCK_ASSUMED_VALID;
        }
        // NORMAL forgets the Evo state of those blocks too, and rebuilds it
        // as they connect again.
        {
            CDBWrapper& db{m_node.evodb->GetRawDB()};
            CDBBatch batch{db};
            std::unique_ptr<CDBIterator> it{db.NewIterator()};
            for (it->SeekToFirst(); it->Valid(); it->Next())
                batch.Erase(it->GetKey());
            BOOST_REQUIRE(db.WriteBatch(batch, /*fSync=*/true));
        }
        if (!keep_caches) DropEvoCaches();
        {
            auto tx{m_node.evodb->BeginTransaction(EvoDbIdentity::NORMAL)};
            m_node.evodb->WriteBestBlock(EvoDbIdentity::NORMAL, genesis);
            tx->Commit();
        }
        BOOST_REQUIRE(m_node.evodb->CommitRootTransaction(EvoDbIdentity::NORMAL, /*sync=*/true));
    }

    //! Drop every partition of every view cache, as a restart would.
    static void DropEvoCaches()
    {
        const CEvoDB unrelated{util::DbWrapperParams{.path = {}, .memory = true, .wipe = true}};
        CCreditPoolManager::ClearBlockDataCacheForTesting();
    }

    //! Download the forgotten blocks in [from, to] again and connect them on
    //! `chainstate`. Returns the state of the last connection attempt.
    BlockValidationState Feed(Chainstate& chainstate, int from, int to)
    {
        BlockValidationState state;
        for (int height{from}; height <= to; ++height) {
            const auto& block{m_blocks.at(height)};
            {
                LOCK(::cs_main);
                CBlockIndex* index{nullptr};
                bool new_block{false};
                BOOST_REQUIRE(CheckBlock(*block, state, m_node.chainman->GetConsensus()));
                BOOST_REQUIRE(m_node.chainman->AcceptBlock(block, state, &index, /*fRequested=*/true, nullptr, &new_block));
            }
            if (!chainstate.ActivateBestChain(state, block) || !state.IsValid()) return state;
        }
        return state;
    }

    Chainstate& Background()
    {
        for (Chainstate* chainstate : m_node.chainman->GetAll()) {
            if (chainstate != &m_node.chainman->ActiveChainstate()) return *chainstate;
        }
        BOOST_FAIL("no background chainstate");
        return m_node.chainman->ActiveChainstate();
    }

    int Height(const Chainstate& chainstate) { return WITH_LOCK(::cs_main, return chainstate.m_chain.Height()); }

    //! The members of the quorum of `type` at `height` under `chainstate`'s
    //! view, computed afresh.
    std::vector<uint256> Members(const Chainstate& chainstate, Consensus::LLMQType type, int height)
    {
        LOCK(::cs_main);
        const EvoViewScope view{chainstate.EvoView()};
        const CBlockIndex* index{Assert(m_node.chainman->m_best_header)->GetAncestor(height)};
        std::vector<uint256> hashes;
        for (const auto& member : llmq::utils::GetAllQuorumMembers(type,
                                                                   {*m_node.dmnman, *m_node.llmq_ctx->qsnapman,
                                                                    *m_node.chainman, Assert(index)},
                                                                   /*reset_cache=*/true)) {
            hashes.push_back(member->proTxHash);
        }
        return hashes;
    }

    //! Dump at the base a section that `mutate` turns into another one, and
    //! pin that one for the lifetime of the returned guard, as a bad review
    //! would.
    std::pair<fs::path, std::unique_ptr<ScopedAssumeutxoEntry>> DumpMutatedAndPinned(
        const std::function<void(evo::EvoSnapshot&)>& mutate)
    {
        std::optional<EvoSnapshotHash> pin;
        const fs::path path{DumpEvoSnapshot([&](auto& evo_section) {
            mutate(*evo_section);
            pin.emplace(evo::GetEvoSnapshotHash(*evo_section));
        })};
        const int base_height{WITH_LOCK(::cs_main, return m_node.chainman->ActiveHeight())};
        const AssumeutxoData pinned{*ExpectedAssumeutxo(base_height, Params())};
        return {path,
                std::make_unique<ScopedAssumeutxoEntry>(base_height, AssumeutxoData{pinned.hash_serialized, *pin,
                                                                                    pinned.nChainTx, pinned.blockhash})};
    }

    SnapshotCompletionResult Complete()
    {
        return WITH_LOCK(::cs_main, return m_node.chainman->MaybeCompleteSnapshotValidation([](bilingual_str) {}));
    }

    bool AnyBlockFailed()
    {
        LOCK(::cs_main);
        for (const auto& [_, index] : m_node.chainman->BlockIndex()) {
            if (index.nStatus & BLOCK_FAILED_MASK) return true;
        }
        return false;
    }

    fs::path DumpEvoSnapshot(const std::function<void(std::optional<evo::EvoSnapshot>&)>& mutate = nullptr)
    {
        const fs::path dump_path{m_path_root / fs::u8path(strprintf("evo_snapshot.%d.dat", ++m_dumps))};
        {
            AutoFile out{fsbridge::fopen(dump_path, "wb")};
            (void)CreateUTXOSnapshot(m_node, m_node.chainman->ActiveChainstate(), out, dump_path, dump_path);
        }
        node::SnapshotMetadata metadata;
        std::vector<std::pair<COutPoint, Coin>> coins;
        std::optional<evo::EvoSnapshot> evo_section;
        {
            AutoFile in{fsbridge::fopen(dump_path, "rb")};
            in >> metadata;
            for (uint64_t i{0}; i < metadata.m_coins_count; ++i) {
                auto& [outpoint, coin]{coins.emplace_back()};
                in >> outpoint >> coin;
            }
            uint64_t marker;
            in >> marker;
            BOOST_REQUIRE_EQUAL(marker, evo::EVO_SNAPSHOT_MARKER);
            OverrideStream<AutoFile> evo_in{&in, SER_DISK, CLIENT_VERSION};
            evo_in >> evo_section.emplace();
        }
        if (mutate) mutate(evo_section);
        const fs::path load_path{fs::PathFromString(fs::PathToString(dump_path) + ".load")};
        AutoFile out{fsbridge::fopen(load_path, "wb")};
        out << metadata;
        for (const auto& [outpoint, coin] : coins)
            out << outpoint << coin;
        if (evo_section) {
            out << evo::EVO_SNAPSHOT_MARKER;
            OverrideStream<AutoFile> evo_out{&out, SER_DISK, CLIENT_VERSION};
            evo_out << *evo_section;
        }
        return load_path;
    }
};

/**
 * EvoSnapshotTestSetup with one masternode and three evonodes, whose quorums
 * really form: from cycle 144 on, every enabled type mines a final commitment
 * signed by its members' operator keys and by a quorum key the fixture keeps,
 * and every block is ChainLocked with those keys once a ChainLock quorum is
 * old enough to sign. The base at 263 is the block before cycle 264, after
 * cycle 240's commitments are mined.
 */
struct EvoQuorumSnapshotTestSetup : public EvoSnapshotTestSetup {
    static constexpr int FIRST_CYCLE{144};
    static constexpr int QUORUM_BASE_HEIGHT{263};
    //! The secret key of each mined quorum, by type and quorum hash.
    std::map<std::pair<Consensus::LLMQType, uint256>, CBLSSecretKey> m_quorum_keys;
    //! Quorums, by type and height, whose DKG fails, so no commitment is mined.
    std::set<std::pair<Consensus::LLMQType, int>> m_failed_dkgs;
    std::optional<CMutableTransaction> m_mnhf_signal;

    EvoQuorumSnapshotTestSetup() :
        EvoSnapshotTestSetup{/*evo_count=*/3}
    {
        MineTo(QUORUM_BASE_HEIGHT);
    }

    llmq::UtilParameters Util(const CBlockIndex* index)
    {
        return {*m_node.dmnman, *m_node.llmq_ctx->qsnapman, *m_node.chainman, index};
    }

    //! A final commitment for the quorum at `quorum` as its members' DKG
    //! would produce it, with every member valid and signing.
    llmq::CFinalCommitment MakeCommitment(const Consensus::LLMQParams& params, const CBlockIndex* quorum)
        EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
    {
        const bool rotation{llmq::IsQuorumRotationEnabled(params, quorum)};
        const auto members{llmq::utils::GetAllQuorumMembers(params.type, Util(quorum))};
        llmq::CFinalCommitment qc{params, quorum->GetBlockHash()};
        qc.nVersion = llmq::CFinalCommitment::GetVersion(rotation, /*is_basic_scheme_active=*/true);
        qc.quorumIndex = rotation ? quorum->nHeight % params.dkgInterval : 0;
        for (size_t i{0}; i < members.size(); ++i) {
            qc.signers[i] = qc.validMembers[i] = true;
        }
        // Below the curve order, so a valid secret key.
        uint256 secret{SerializeHash(std::make_pair(params.type, quorum->GetBlockHash()))};
        *secret.begin() = 0;
        const CBLSSecretKey quorum_key{Span{secret}};
        qc.quorumPublicKey = quorum_key.GetPublicKey();
        qc.quorumVvecHash = SerializeHash(std::make_pair(std::string{"vvec"}, quorum->GetBlockHash()));
        const uint256 commitment_hash{llmq::BuildCommitmentHash(params.type, qc.quorumHash, qc.validMembers,
                                                                qc.quorumPublicKey, qc.quorumVvecHash)};
        std::vector<CBLSSignature> signatures;
        std::vector<CBLSPublicKey> public_keys;
        for (const auto& member : members) {
            const CBLSSecretKey& operator_key{m_operator_keys.at(member->proTxHash)};
            signatures.push_back(operator_key.Sign(commitment_hash, /*specificLegacyScheme=*/false));
            public_keys.push_back(operator_key.GetPublicKey());
        }
        qc.membersSig = CBLSSignature::AggregateSecure(signatures, public_keys, commitment_hash);
        qc.quorumSig = quorum_key.Sign(commitment_hash, /*specificLegacyScheme=*/false);
        m_quorum_keys.insert_or_assign({params.type, qc.quorumHash}, quorum_key);
        return qc;
    }

    //! Sign the recovered signature of request `id` over `msg_hash` as the
    //! quorum of `type` at `quorum_hash`.
    CBLSSignature SignAs(Consensus::LLMQType type, const uint256& quorum_hash, const uint256& id, const uint256& msg_hash)
    {
        return m_quorum_keys.at({type, quorum_hash})
            .Sign(llmq::SignHash{type, quorum_hash, id, msg_hash}.Get(),
                  /*specificLegacyScheme=*/false);
    }

    //! A ChainLock on `block` by the quorum the signing rules select for it,
    //! once a quorum is old enough to sign.
    std::optional<chainlock::ChainLockSig> MakeChainLock(const CBlockIndex* block)
    {
        LOCK(::cs_main);
        const Consensus::LLMQParams params{*Params().GetLLMQ(Params().GetConsensus().llmqTypeChainLocks)};
        const uint256 id{chainlock::GenSigRequestId(block->nHeight)};
        const auto quorum{llmq::SelectQuorumForSigning(params, m_node.chainman->ActiveChain(), *m_node.llmq_ctx->qman,
                                                       id, block->nHeight)};
        if (!quorum || !m_quorum_keys.contains({params.type, quorum->qc->quorumHash})) return std::nullopt;
        return chainlock::ChainLockSig{block->nHeight, block->GetBlockHash(),
                                       SignAs(params.type, quorum->qc->quorumHash, id, block->GetBlockHash())};
    }

    void MineOneBlock() override
    {
        {
            LOCK(::cs_main);
            const CBlockIndex* tip{m_node.chainman->ActiveTip()};
            const int next{tip->nHeight + 1};
            // Every quorum whose mining window opens with the next block has
            // its commitment ready to mine, unless its DKG failed.
            for (const Consensus::LLMQParams& params : llmq::GetEnabledQuorumParams(*m_node.chainman, tip)) {
                const int cycle{next - next % params.dkgInterval};
                if (cycle < FIRST_CYCLE || next != cycle + params.dkgMiningWindowStart) continue;
                const bool rotation{llmq::IsQuorumRotationEnabled(params, tip->GetAncestor(cycle))};
                for (int index{0}; index < (rotation ? params.signingActiveQuorumCount : 1); ++index) {
                    if (m_failed_dkgs.contains({params.type, cycle + index})) continue;
                    const auto qc{MakeCommitment(params, tip->GetAncestor(cycle + index))};
                    if (qc.CountSigners() < params.minSize) continue;
                    m_node.llmq_ctx->quorum_block_processor->AddMineableCommitment(qc);
                }
            }
        }
        // The block carries the ChainLock of its parent.
        const CBlockIndex* tip{WITH_LOCK(::cs_main, return m_node.chainman->ActiveTip())};
        if (const auto clsig{MakeChainLock(tip)}) {
            m_node.chainlocks->UpdateBestChainlock(tip->GetBlockHash(), *clsig, tip);
        }
        if (WITH_LOCK(m_node.mempool->cs, return m_node.mempool->size()) == 0) {
            mineBlocks(1);
            return;
        }
        // Mine the mempool, e.g. an asset unlock, as a miner would.
        auto block{std::make_shared<CBlock>(
            node::BlockAssembler{m_node.chainman->ActiveChainstate(), m_node, m_node.mempool.get()}
                .CreateNewBlock(GetScriptForRawPubKey(coinbaseKey.GetPubKey()))
                ->block)};
        block->hashMerkleRoot = BlockMerkleRoot(*block);
        while (!CheckProofOfWork(block->GetHash(), block->nBits, m_node.chainman->GetConsensus()))
            ++block->nNonce;
        BOOST_REQUIRE(m_node.chainman->ProcessNewBlock(block, /*force_processing=*/true, /*new_block=*/nullptr));
        BOOST_REQUIRE_EQUAL(WITH_LOCK(::cs_main, return m_node.chainman->ActiveTip()->GetBlockHash()), block->GetHash());
        SetMockTime(GetTime() + 1);
    }

    //! A withdrawal of `amount` plus `fee` from the credit pool, signed by the
    //! newest Platform quorum.
    CTransactionRef MakeAssetUnlock(uint64_t index, CAmount amount, uint32_t fee)
    {
        LOCK(::cs_main);
        const EvoViewScope view{m_node.chainman->ActiveChainstate().EvoView()};
        const CBlockIndex* tip{m_node.chainman->ActiveTip()};
        const auto type{Params().GetConsensus().llmqTypePlatform};
        const auto quorums{m_node.llmq_ctx->qman->ScanQuorums(type, tip, 1)};
        BOOST_REQUIRE(!quorums.empty());
        const uint256 quorum_hash{quorums.front()->qc->quorumHash};
        CMutableTransaction tx;
        tx.nVersion = 3;
        tx.nType = TRANSACTION_ASSET_UNLOCK;
        tx.vout.emplace_back(amount, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
        const auto with_signature = [&](const CBLSSignature& sig) {
            SetTxPayload(tx, CAssetUnlockPayload{CAssetUnlockPayload::INITIAL_VERSION, index, fee,
                                                 static_cast<uint32_t>(tip->nHeight), quorum_hash, sig});
        };
        with_signature({});
        const uint256 id{::SerializeHash(std::make_pair(ASSET_UNLOCK_REQUESTID_PREFIX, index))};
        with_signature(SignAs(type, quorum_hash, id, ::SerializeHash(tx)));
        return MakeTransactionRef(tx);
    }

    //! Withdraw `amount`, or exactly what the credit pool allows, in the next
    //! block.
    void Withdraw(uint64_t index, std::optional<CAmount> amount = std::nullopt)
    {
        constexpr uint32_t FEE{1000};
        const CAmount limit{
            WITH_LOCK(::cs_main,
                      return m_node.special_tx->m_cpoolman->GetCreditPool(m_node.chainman->ActiveTip()).currentLimit)};
        BOOST_REQUIRE_GT(limit, amount.value_or(0) + FEE);
        const auto result{WITH_LOCK(::cs_main, return m_node.chainman->ProcessTransaction(
                                                   MakeAssetUnlock(index, amount.value_or(limit - FEE), FEE)))};
        BOOST_REQUIRE_MESSAGE(result.m_result_type == MempoolAcceptResult::ResultType::VALID, result.m_state.ToString());
        MineOneBlock();
        BOOST_REQUIRE_EQUAL(WITH_LOCK(m_node.mempool->cs, return m_node.mempool->size()), 0U);
    }
};

//! Evo records the snapshot chainstate writes for one of its blocks above the
//! base: a full MN list and a mined quorum commitment.
struct SnapshotViewRecords {
    static constexpr auto LLMQ_TYPE{Consensus::LLMQType::LLMQ_TEST};

    //! Block hash, not index: the block index is reloaded across restarts.
    uint256 block_hash;
    uint256 quorum_hash;
    CDeterministicMNList list;

    // These keys mirror DB_LIST_SNAPSHOT in evo/deterministicmns.cpp and
    // DB_MINED_COMMITMENT in llmq/blockprocessor.cpp (file-local there).
    auto ListKey() const { return std::make_pair(std::string{"dmn_S3"}, block_hash); }
    auto CommitmentKey() const { return std::make_pair(std::string{"q_mc"}, std::make_pair(LLMQ_TYPE, quorum_hash)); }

    static SnapshotViewRecords Write(CEvoDB& evodb, const CBlockIndex* block) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
    {
        SnapshotViewRecords records{block->GetBlockHash(), GetRandHash(),
                                    CDeterministicMNList{block->GetBlockHash(), block->nHeight, 1}};
        {
            auto tx = evodb.BeginTransaction(EvoDbIdentity::SNAPSHOT);
            evodb.Write(records.ListKey(), records.list);
            const llmq::CFinalCommitment qc{Params().GetLLMQ(LLMQ_TYPE).value(), records.quorum_hash};
            evodb.Write(records.CommitmentKey(), std::make_pair(qc, block->GetBlockHash()));
            tx->Commit();
        }
        BOOST_REQUIRE(evodb.CommitRootTransaction(EvoDbIdentity::SNAPSHOT, /*sync=*/true));
        return records;
    }

    //! Which of these records the NORMAL view reads, directly and through
    //! the commitment consumer.
    std::pair<bool, bool> ReadByNormal(node::NodeContext& node) const
    {
        LOCK(::cs_main);
        CEvoDB& evodb{*node.evodb};
        EvoViewScope normal{evodb.ViewFor(EvoDbIdentity::NORMAL)};
        CDeterministicMNList stored_list;
        const bool list_read{evodb.Read(ListKey(), stored_list)};
        return {list_read, node.llmq_ctx->quorum_block_processor->HasMinedCommitment(LLMQ_TYPE, quorum_hash,
                                                                                     node.chainman->ActiveChain())};
    }

    //! Whether the MN-list manager, under NORMAL with DIP3 active, serves the
    //! stored list for the block rather than building its own. Needs a
    //! single chainstate: dual operation refuses to bootstrap a missing list.
    bool ListServedByNormal(node::NodeContext& node) const
    {
        LOCK(::cs_main);
        EvoViewScope normal{node.evodb->ViewFor(EvoDbIdentity::NORMAL)};
        ScopedDIP3FromGenesis dip3_active;
        const CBlockIndex* block{Assert(node.chainman->m_blockman.LookupBlockIndex(block_hash))};
        return SerializeHash(node.dmnman->GetListForBlock(block)) == SerializeHash(list);
    }
};

//! The snapshot chainstate's own Evo state, built by real block connection
//! past DIP3, stays in its generation: NORMAL never reads it, through EvoDB or
//! any partitioned cache, and rebuilds everything itself once the snapshot is
//! rejected.
BOOST_FIXTURE_TEST_CASE(chainstatemanager_snapshot_evo_state_past_dip3, SnapshotTestSetup)
{
    this->SetupSnapshot();
    ChainstateManager* chainman{Assert(m_node.chainman.get())};

    // Positive control for the background-suppression checks elsewhere: the
    // active chainstate announces the list change its ProRegTx block makes.
    MnListChangeCounter events;
    RegisterValidationInterface(&events);
    int ui_mn_list_changed{0};
    auto ui_connection{uiInterface.NotifyMasternodeListChanged_connect(
        [&](const CDeterministicMNList&, const CBlockIndex*) { ++ui_mn_list_changed; })};
    const auto chain{MineSnapshotChainPastDip3()};
    SyncWithValidationInterfaceQueue();
    ui_connection.disconnect();
    UnregisterValidationInterface(&events);
    BOOST_CHECK_GE(events.changes, 1);
    BOOST_CHECK_GE(ui_mn_list_changed, 1);

    // These keys mirror DB_LIST_DIFF and DB_LIST_SNAPSHOT in
    // evo/deterministicmns.cpp, DB_CREDITPOOL_SNAPSHOT in evo/creditpool.cpp
    // and DB_SIGNALS_v2 in evo/mnhftx.cpp (file-local there).
    const auto diff_key{std::make_pair(std::string{"dmn_D4"}, chain.protx_block)};
    const auto list_key{std::make_pair(std::string{"dmn_S3"}, chain.period_block)};
    const auto pool_key{std::make_pair(std::string{"cpm_S"}, chain.period_block)};
    const auto signals_key{std::make_pair(std::string{"mnhf_s2"}, chain.tip)};

    CEvoDB& evodb{*m_node.evodb};
    CCreditPool snapshot_pool;
    {
        LOCK(::cs_main);
        EvoViewScope snapshot_view{evodb.ViewFor(EvoDbIdentity::SNAPSHOT)};
        CDeterministicMNListDiff diff;
        CDeterministicMNList list;
        BOOST_CHECK(evodb.Read(diff_key, diff));
        BOOST_REQUIRE(evodb.Read(list_key, list));
        BOOST_CHECK(list.HasMN(chain.protx_hash));
        BOOST_CHECK(evodb.Read(pool_key, snapshot_pool));
    }
    {
        LOCK(::cs_main);
        EvoViewScope normal_view{evodb.ViewFor(EvoDbIdentity::NORMAL)};
        CDeterministicMNListDiff diff;
        CDeterministicMNList list;
        CCreditPool pool;
        BOOST_CHECK(!evodb.Read(diff_key, diff));
        BOOST_CHECK(!evodb.Read(list_key, list));
        BOOST_CHECK(!evodb.Read(pool_key, pool));
    }

    // Snapshot-only state that NORMAL must not see even once the snapshot
    // view has warmed its caches with it: a credit pool and a V24 EHF signal
    // the blocks never carried. A deployment window short enough for the
    // signal to start V24 by the tip makes the signal observable through
    // versionbits.
    CCreditPool tampered_pool{snapshot_pool};
    tampered_pool.locked += COIN;
    {
        LOCK(::cs_main);
        auto tx{evodb.BeginTransaction(EvoDbIdentity::SNAPSHOT)};
        evodb.Write(pool_key, tampered_pool);
        evodb.Write(signals_key,
                    CMNHFManager::Signals{{Params().GetConsensus().vDeployments[Consensus::DEPLOYMENT_V24].bit, 500}});
        tx->Commit();
    }
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return evodb.CommitRootTransaction(EvoDbIdentity::SNAPSHOT, /*sync=*/true)));
    auto& v24{const_cast<Consensus::Params&>(Params().GetConsensus()).vDeployments[Consensus::DEPLOYMENT_V24]};
    struct V24Restore {
        Consensus::BIP9Deployment& deployment;
        const Consensus::BIP9Deployment saved{deployment};
        ~V24Restore() { deployment = saved; }
    } v24_restore{v24};
    v24.nWindowSize = 10;
    v24.nThresholdStart = v24.nThresholdMin = 1;
    // Drop every partition of every view cache, as a restart would, so the
    // snapshot view reads the records back.
    {
        const CEvoDB unrelated{util::DbWrapperParams{.path = {}, .memory = true, .wipe = true}};
    }

    CCreditPoolManager& cpoolman{*m_node.special_tx->m_cpoolman};
    {
        LOCK(::cs_main);
        const CBlockIndex* period_block{chainman->m_blockman.LookupBlockIndex(chain.period_block)};
        const CBlockIndex* protx_block{chainman->m_blockman.LookupBlockIndex(chain.protx_block)};
        const CBlockIndex* tip{chainman->m_blockman.LookupBlockIndex(chain.tip)};
        const auto v24_state{[&] {
            return chainman->m_versionbitscache.State(tip, chainman->GetConsensus(), Consensus::DEPLOYMENT_V24);
        }};
        {
            EvoViewScope snapshot_view{evodb.ViewFor(EvoDbIdentity::SNAPSHOT)};
            BOOST_CHECK(m_node.dmnman->GetListForBlock(protx_block).HasMN(chain.protx_hash));
            BOOST_CHECK_EQUAL(cpoolman.GetCreditPool(period_block).locked, tampered_pool.locked);
            BOOST_CHECK(v24_state() == ThresholdState::STARTED);
        }
        EvoViewScope normal_view{evodb.ViewFor(EvoDbIdentity::NORMAL)};
        // NORMAL has no MN-list history above the base yet: it cannot use the
        // snapshot's and fails as unavailable.
        BOOST_CHECK_THROW(m_node.dmnman->GetListForBlock(protx_block), BlockDataUnavailableError);
        // It derives the credit pool and EHF signals from the blocks itself.
        // What it derives for blocks it has not connected is discarded.
        const auto discard{evodb.BeginTransaction(EvoDbIdentity::NORMAL)};
        BOOST_CHECK_EQUAL(cpoolman.GetCreditPool(period_block).locked, snapshot_pool.locked);
        BOOST_CHECK(v24_state() == ThresholdState::DEFINED);
    }

    // Reject the snapshot and restart on the background chainstate alone. It
    // connects the blocks the snapshot chainstate connected, building its own
    // MN list, diffs and credit pool, without reading or tripping over any of
    // the snapshot generation's records.
    BOOST_REQUIRE(chainman->HandleSnapshotStateMismatch("test rejection", [](bilingual_str) {}));
    BOOST_CHECK(!evodb.GetSnapshotGeneration());
    chainman = &this->SimulateNodeRestart();
    BOOST_REQUIRE(std::get<0>(this->TryLoadChainstate()) == node::ChainstateLoadStatus::SUCCESS);
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return !chainman->IsSnapshotActive()));
    BlockValidationState state;
    BOOST_REQUIRE(chainman->ActiveChainstate().ActivateBestChain(state));
    BOOST_CHECK(!ShutdownRequested());
    {
        LOCK(::cs_main);
        BOOST_REQUIRE_EQUAL(chainman->ActiveTip()->GetBlockHash(), chain.tip);
        EvoViewScope normal_view{evodb.ViewFor(EvoDbIdentity::NORMAL)};
        CDeterministicMNListDiff diff;
        CDeterministicMNList list;
        CCreditPool pool;
        BOOST_CHECK(evodb.Read(diff_key, diff));
        BOOST_REQUIRE(evodb.Read(list_key, list));
        BOOST_CHECK(list.HasMN(chain.protx_hash));
        BOOST_REQUIRE(evodb.Read(pool_key, pool));
        BOOST_CHECK_EQUAL(pool.locked, snapshot_pool.locked);
        BOOST_CHECK(chainman->m_versionbitscache.State(chainman->ActiveTip(), chainman->GetConsensus(),
                                                       Consensus::DEPLOYMENT_V24) == ThresholdState::DEFINED);
    }
}

//! Test basic snapshot activation.
BOOST_FIXTURE_TEST_CASE(chainstatemanager_activate_snapshot, SnapshotTestSetup)
{
    auto [background_chainstate, snapshot_chainstate] = this->SetupSnapshot();
    ChainstateManager& chainman = *Assert(m_node.chainman);

    // Evo-derived caches are partitioned by view: state warmed under the
    // snapshot view at blocks the background chainstate also reaches is never
    // served to NORMAL lookups, which must derive their own.
    {
        LOCK(::cs_main);
        ScopedDIP3FromGenesis dip3_active;
        const auto llmq_type{Consensus::LLMQType::LLMQ_TEST};
        const CBlockIndex* list_block{background_chainstate->m_chain[100]};
        const CBlockIndex* quorum_base{background_chainstate->m_chain[96]};
        const llmq::UtilParameters util_params{*m_node.dmnman, *m_node.llmq_ctx->qsnapman, chainman, quorum_base};
        auto& qblockman{*m_node.llmq_ctx->quorum_block_processor};
        const uint256 quorum_hash{GetRandHash()};
        {
            EvoViewScope snapshot_view{m_node.evodb->ViewFor(EvoDbIdentity::SNAPSHOT)};
            m_node.dmnman->SetListForBlockForTesting(
                CDeterministicMNList{list_block->GetBlockHash(), list_block->nHeight, 1});
            m_node.dmnman->SetListForBlockForTesting(
                CDeterministicMNList{quorum_base->GetBlockHash(), quorum_base->nHeight, 0});
            llmq::utils::GetAllQuorumMembers(llmq_type, util_params);
            {
                auto tx = m_node.evodb->BeginTransaction(EvoDbIdentity::SNAPSHOT);
                const llmq::CFinalCommitment qc{Params().GetLLMQ(llmq_type).value(), quorum_hash};
                m_node.evodb->Write(std::make_pair(std::string{"q_mc"}, std::make_pair(llmq_type, quorum_hash)),
                                    std::make_pair(qc, list_block->GetBlockHash()));
                tx->Commit();
            }
            BOOST_REQUIRE(qblockman.HasMinedCommitment(llmq_type, quorum_hash, snapshot_chainstate->m_chain));
        }

        EvoViewScope normal_view{m_node.evodb->ViewFor(EvoDbIdentity::NORMAL)};
        std::vector<const CBlockIndex*> list_misses;
        m_node.dmnman->SetListSnapshotMissHookForTesting(
            [&](const CBlockIndex* pindex) { list_misses.push_back(pindex); });
        // NORMAL has no MN-list history for these blocks, so deriving its own
        // lists fails as unavailable rather than reusing the snapshot's.
        BOOST_CHECK_THROW(m_node.dmnman->GetListForBlock(list_block), BlockDataUnavailableError);
        BOOST_CHECK_THROW(llmq::utils::GetAllQuorumMembers(llmq_type, util_params), BlockDataUnavailableError);
        BOOST_CHECK(list_misses == (std::vector<const CBlockIndex*>{list_block, quorum_base}));
        BOOST_CHECK(!qblockman.HasMinedCommitment(llmq_type, quorum_hash, snapshot_chainstate->m_chain));
        m_node.dmnman->SetListSnapshotMissHookForTesting(nullptr);
    }

    // Stay outside every regtest LLMQ mining window (dkgInterval 24, windows
    // end at offset 20), so the block needs no quorum commitment.
    while (WITH_LOCK(::cs_main, return (snapshot_chainstate->m_chain.Height() + 1) % 24) < 21) {
        mineBlocks(1);
    }
    const CBlockIndex* snapshot_tip{WITH_LOCK(::cs_main, return snapshot_chainstate->m_chain.Tip())};
    const CBlock template_block{CreateBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()), *snapshot_chainstate)};

    // With DIP3 active at the base, the snapshot chainstate's next block needs
    // the pre-base MN list, whose diffs this cold EvoDB never derived. That is
    // missing local data: the chainstate must stall with a local error, and
    // the block must not be marked invalid.
    std::optional<ScopedDIP3FromGenesis> dip3_active{std::in_place};
    int evo_state_errors{0};
    const auto log_callback{LogInstance().PushBackCallback([&](const std::string& line) {
        if (line.find("local Evo state error") != std::string::npos) ++evo_state_errors;
    })};
    const auto block{std::make_shared<const CBlock>(MakeCbTxBlock(template_block, snapshot_tip->nHeight + 1))};
    BOOST_CHECK(!chainman.ProcessNewBlock(block, /*force_processing=*/true, /*new_block=*/nullptr));
    BlockValidationState state;
    BOOST_CHECK(!snapshot_chainstate->ActivateBestChain(state, block));
    BOOST_CHECK(IsLocalEvoStateError(state));
    BOOST_CHECK(state.GetLocalEvoStateResult() == LocalEvoStateResult::UNAVAILABLE);
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "evo-local-unavailable");
    BOOST_CHECK(GetWarnings(/*verbose=*/true).original.find("Snapshot state unavailable") != std::string::npos);

    // Longer branches, each stalling at its own first block, are out of reach
    // together: no stall makes the chainstate forget another.
    const auto child_of{[&](const CBlock& parent, int height = 0) {
        if (!height) height = snapshot_tip->nHeight + 2;
        CBlock child_template{template_block};
        child_template.hashPrevBlock = parent.GetHash();
        CMutableTransaction child_coinbase{*child_template.vtx[0]};
        child_coinbase.vin[0].scriptSig = CScript{} << height << CScriptNum{1};
        child_template.vtx[0] = MakeTransactionRef(std::move(child_coinbase));
        return std::make_shared<const CBlock>(MakeCbTxBlock(child_template, height));
    }};
    BOOST_CHECK(!chainman.ProcessNewBlock(child_of(*block), /*force_processing=*/true, /*new_block=*/nullptr));
    // However many there are: every stalled block is remembered.
    constexpr int stalled_branches{40};
    for (int i{1}; i < stalled_branches; ++i) {
        CBlock other_template{template_block};
        other_template.nTime += i;
        const auto other{std::make_shared<const CBlock>(MakeCbTxBlock(other_template, snapshot_tip->nHeight + 1))};
        BOOST_CHECK(!chainman.ProcessNewBlock(other, /*force_processing=*/true, /*new_block=*/nullptr));
        BOOST_CHECK(!chainman.ProcessNewBlock(child_of(*other), /*force_processing=*/true, /*new_block=*/nullptr));
    }

    // Startup's block import goes on past the stalled chainstate instead of
    // aborting the node.
    static std::atomic<int> exit_status{EXIT_SUCCESS};
    BOOST_REQUIRE(InitShutdownState(exit_status));
    node::ThreadImport(chainman, /*vImportFiles=*/{}, *m_node.args, /*mempool_path=*/{});
    BOOST_CHECK(!ShutdownRequested());
    AbortShutdown();

    // Retrying cannot succeed before the snapshot's Evo state changes, so each
    // stalled block was validated once and every later attempt refused it
    // outright.
    LogInstance().DeleteCallback(log_callback);
    BOOST_CHECK_EQUAL(evo_state_errors, stalled_branches);
    {
        LOCK(::cs_main);
        const CBlockIndex* index{chainman.m_blockman.LookupBlockIndex(block->GetHash())};
        BOOST_REQUIRE(index);
        BOOST_CHECK_EQUAL(index->nStatus & BLOCK_FAILED_MASK, 0U);
        BOOST_CHECK_EQUAL(snapshot_chainstate->m_chain.Tip(), snapshot_tip);
    }

    // The stalls keep out only the branches through the stalled blocks: a
    // block that needs no unavailable data still advances the chain, although
    // every stalled branch has more work.
    dip3_active.reset();
    const auto usable{std::make_shared<const CBlock>(template_block)};
    BOOST_CHECK(chainman.ProcessNewBlock(usable, /*force_processing=*/true, /*new_block=*/nullptr));
    BOOST_CHECK_EQUAL(WITH_LOCK(::cs_main, return snapshot_chainstate->m_chain.Tip()->GetBlockHash()), usable->GetHash());

    // A chainstate that stalls partway back still announces the tip it got
    // to, whether it was invalidating a block, moving away from a block that
    // conflicts with a ChainLock, or reorganizing. Undoing `usable` stalls once
    // it changes the MN list: that needs the pre-base list.
    CBlockIndex* usable_index{WITH_LOCK(::cs_main, return chainman.m_blockman.LookupBlockIndex(usable->GetHash()))};
    {
        LOCK(::cs_main);
        CDeterministicMNListDiff diff;
        diff.removedMns.emplace(0);
        // The key mirrors DB_LIST_DIFF in evo/deterministicmns.cpp.
        auto tx{m_node.evodb->BeginTransaction(EvoDbIdentity::SNAPSHOT)};
        m_node.evodb->Write(std::make_pair(std::string{"dmn_D4"}, usable->GetHash()), diff);
        tx->Commit();
    }
    TipAnnouncements tips;
    RegisterValidationInterface(&tips);
    const auto check_announced{[&](const CBlockIndex* expected_tip) {
        SyncWithValidationInterfaceQueue();
        BOOST_CHECK_EQUAL(WITH_LOCK(::cs_main, return snapshot_chainstate->m_chain.Tip()), expected_tip);
        BOOST_CHECK_EQUAL(tips.synchronous, expected_tip);
        BOOST_CHECK_EQUAL(tips.queued, expected_tip);
    }};

    mineBlocks(1);
    CBlockIndex* above_usable{WITH_LOCK(::cs_main, return snapshot_chainstate->m_chain.Tip())};
    dip3_active.emplace();
    BOOST_CHECK(!snapshot_chainstate->InvalidateBlock(state, usable_index));
    BOOST_CHECK(IsLocalEvoStateError(state));
    check_announced(usable_index);

    dip3_active.reset();
    WITH_LOCK(::cs_main, snapshot_chainstate->ResetBlockFailureFlags(above_usable));
    BOOST_REQUIRE(snapshot_chainstate->ActivateBestChain(state));
    BOOST_REQUIRE_EQUAL(WITH_LOCK(::cs_main, return snapshot_chainstate->m_chain.Tip()), above_usable);

    // An InstantSend lock without its transaction invalidates its conflicting
    // blocks only for a moment: stalling partway leaves none of the blocks it
    // disconnected marked invalid.
    {
        NetInstantSend net{m_node.peerman.get(),   *m_node.isman,      nullptr,  *m_node.llmq_ctx->sigman,
                           *m_node.llmq_ctx->qman, *m_node.chainlocks, chainman, *m_node.mempool,
                           *m_node.mn_sync};
        dip3_active.emplace();
        NetInstantSendConflictsTest::InvalidateTemporarily(net, usable->GetHash());
        dip3_active.reset();
        LOCK(::cs_main);
        BOOST_CHECK_EQUAL(snapshot_chainstate->m_chain.Tip(), usable_index);
        BOOST_CHECK_EQUAL(above_usable->nStatus & BLOCK_FAILED_MASK, 0U);
        BOOST_CHECK_EQUAL(usable_index->nStatus & BLOCK_FAILED_MASK, 0U);
    }
    BOOST_REQUIRE(snapshot_chainstate->ActivateBestChain(state));
    BOOST_REQUIRE_EQUAL(WITH_LOCK(::cs_main, return snapshot_chainstate->m_chain.Tip()), above_usable);

    // Enforcing a ChainLock on a stalled sibling moves the chain away from
    // `usable`.
    const CBlockIndex* stalled_sibling{WITH_LOCK(::cs_main, return chainman.m_blockman.LookupBlockIndex(block->GetHash()))};
    snapshot_chainstate->EnforceBlock(state, stalled_sibling);
    check_announced(usable_index);
    BOOST_CHECK(WITH_LOCK(::cs_main, return above_usable->nStatus & BLOCK_CONFLICT_CHAINLOCK));

    // A reorganization from a new tip above `usable` to a longer branch that
    // forks at `usable` disconnects that tip, then stalls connecting the
    // branch.
    mineBlocks(1);
    dip3_active.emplace();
    const auto reorg_first{child_of(*usable)};
    const auto reorg_second{child_of(*reorg_first, snapshot_tip->nHeight + 3)};
    chainman.ProcessNewBlock(reorg_first, /*force_processing=*/true, /*new_block=*/nullptr);
    BOOST_CHECK(!chainman.ProcessNewBlock(reorg_second, /*force_processing=*/true, /*new_block=*/nullptr));
    check_announced(usable_index);

    UnregisterValidationInterface(&tips);
    dip3_active.reset();
    SetMiscWarning(Untranslated(""));
}

//! A chainstate whose first disconnect stalls has not moved, so its best
//! header stays where it was, whether it was invalidating that block or moving
//! away from it for a ChainLock.
BOOST_FIXTURE_TEST_CASE(chainstatemanager_stall_keeps_best_header, SnapshotTestSetup)
{
    auto [background_chainstate, snapshot_chainstate] = this->SetupSnapshot();
    ChainstateManager& chainman = *Assert(m_node.chainman);
    // Stay outside every regtest LLMQ mining window, so the sibling below
    // needs no quorum commitment.
    while (WITH_LOCK(::cs_main, return (snapshot_chainstate->m_chain.Height() + 1) % 24) < 21) {
        mineBlocks(1);
    }
    const CBlock sibling_template{CreateBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()), *snapshot_chainstate)};
    mineBlocks(1);
    CBlockIndex* tip{WITH_LOCK(::cs_main, return snapshot_chainstate->m_chain.Tip())};
    BOOST_REQUIRE_EQUAL(WITH_LOCK(::cs_main, return chainman.m_best_header), tip);
    // Undoing the tip once it changes the MN list needs the pre-base list.
    {
        LOCK(::cs_main);
        CDeterministicMNListDiff diff;
        diff.removedMns.emplace(0);
        // The key mirrors DB_LIST_DIFF in evo/deterministicmns.cpp.
        auto tx{m_node.evodb->BeginTransaction(EvoDbIdentity::SNAPSHOT)};
        m_node.evodb->Write(std::make_pair(std::string{"dmn_D4"}, tip->GetBlockHash()), diff);
        tx->Commit();
    }
    ScopedDIP3FromGenesis dip3_active;
    const auto check_unmoved{[&] {
        LOCK(::cs_main);
        BOOST_CHECK_EQUAL(snapshot_chainstate->m_chain.Tip(), tip);
        BOOST_CHECK_EQUAL(chainman.m_best_header, tip);
    }};

    BlockValidationState state;
    BOOST_CHECK(!snapshot_chainstate->InvalidateBlock(state, tip));
    BOOST_CHECK(IsLocalEvoStateError(state));
    check_unmoved();

    const auto sibling{std::make_shared<const CBlock>(MakeCbTxBlock(sibling_template, tip->nHeight))};
    chainman.ProcessNewBlock(sibling, /*force_processing=*/true, /*new_block=*/nullptr);
    const CBlockIndex* sibling_index{WITH_LOCK(::cs_main, return chainman.m_blockman.LookupBlockIndex(sibling->GetHash()))};
    BOOST_REQUIRE(sibling_index);
    snapshot_chainstate->EnforceBlock(state, sibling_index);
    check_unmoved();
    SetMiscWarning(Untranslated(""));
}

//! A historical block body this node does not have is local state, not
//! invalidity: a snapshot chainstate that has to rebuild the credit pool or the
//! EHF signals from it stalls, and the block it was connecting stays valid.
BOOST_FIXTURE_TEST_CASE(chainstatemanager_missing_body_stalls, SnapshotTestSetup)
{
    auto [background_chainstate, snapshot_chainstate] = this->SetupSnapshot();
    ChainstateManager& chainman = *Assert(m_node.chainman);
    CBlockIndex* tip{WITH_LOCK(::cs_main, return snapshot_chainstate->m_chain.Tip())};
    const CBlock template_block{CreateBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()), *snapshot_chainstate)};
    // Prune the tip's body.
    const uint32_t tip_status{WITH_LOCK(::cs_main, return tip->nStatus)};
    {
        LOCK(::cs_main);
        chainman.m_blockman.m_have_pruned = true;
        tip->nStatus &= ~(BLOCK_HAVE_DATA | BLOCK_HAVE_UNDO);
    }

    // With V20 active, the next block needs the credit pool at the tip, which
    // the snapshot view has to rebuild from the bodies.
    ScopedDIP3FromGenesis dip3_active;
    Consensus::Params& consensus{const_cast<Consensus::Params&>(Params().GetConsensus())};
    struct V20HeightRestore {
        Consensus::Params& params;
        const int height;
        ~V20HeightRestore() { params.V20Height = height; }
    } v20_restore{consensus, std::exchange(consensus.V20Height, 1)};
    const auto block{std::make_shared<const CBlock>(
        MakeCbTxBlock(template_block, tip->nHeight + 1, CCbTx::Version::CLSIG_AND_BALANCE))};
    // Submitting it announces the stall as a local error, which submitblock
    // reports instead of an inconclusive result, and blames no peer.
    const auto checked{std::make_shared<BlockCheckedCatcher>(block->GetHash())};
    RegisterSharedValidationInterface(checked);
    CDataStream block_stream{SER_NETWORK, PROTOCOL_VERSION};
    block_stream << *block;
    UniValue submit_params{UniValue::VARR};
    submit_params.push_back(HexStr(block_stream));
    BOOST_CHECK_EXCEPTION(CallRPC("submitblock", submit_params), UniValue, [](const UniValue& error) {
        return error["code"].getValStr() == ToString(RPC_VERIFY_ERROR) &&
               error["message"].get_str() == "evo-local-unavailable";
    });
    UnregisterSharedValidationInterface(checked);
    BOOST_REQUIRE(checked->state);
    BOOST_CHECK(IsLocalEvoStateError(*checked->state));
    BlockValidationState state;
    BOOST_CHECK(!snapshot_chainstate->ActivateBestChain(state, block));
    BOOST_CHECK(IsLocalEvoStateError(state));
    {
        LOCK(::cs_main);
        const CBlockIndex* index{chainman.m_blockman.LookupBlockIndex(block->GetHash())};
        BOOST_REQUIRE(index);
        BOOST_CHECK_EQUAL(index->nStatus & BLOCK_FAILED_MASK, 0U);
        BOOST_CHECK_EQUAL(snapshot_chainstate->m_chain.Tip(), tip);
    }

    // An asset unlock checked against the credit pool at the tip, rebuilt from
    // its pruned body, is not rejected either: its acceptance ends in an error.
    {
        CMutableTransaction unlock_tx;
        unlock_tx.nVersion = CTransaction::SPECIAL_VERSION;
        unlock_tx.nType = TRANSACTION_ASSET_UNLOCK;
        unlock_tx.vout.emplace_back(COIN, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
        SetTxPayload(unlock_tx, CAssetUnlockPayload{CAssetUnlockPayload::INITIAL_VERSION, /*index=*/1, /*fee=*/1000,
                                                    static_cast<uint32_t>(tip->nHeight), uint256::ONE, CBLSSignature{}});
        const MempoolAcceptResult result{
            WITH_LOCK(::cs_main, return chainman.ProcessTransaction(MakeTransactionRef(std::move(unlock_tx))))};
        BOOST_CHECK(result.m_result_type == MempoolAcceptResult::ResultType::INVALID);
        BOOST_CHECK(result.m_state.IsError());
    }

    // The block above the snapshot base checks its coinbase ChainLock against
    // the base's, whose body a snapshot node does not have: it cannot tell a
    // missing ChainLock from an unreadable one.
    {
        LOCK(::cs_main);
        const int snapshot_base_height{m_node.evodb->ViewFor(EvoDbIdentity::SNAPSHOT).base_height};
        CBlockIndex* snapshot_base{snapshot_chainstate->m_chain[snapshot_base_height]};
        const uint32_t snapshot_base_status{snapshot_base->nStatus};
        // The background chainstate accepting the base's coinbase ChainLock
        // remembers it for its own view only.
        {
            EvoViewScope normal_view{m_node.evodb->ViewFor(EvoDbIdentity::NORMAL)};
            CBLSSecretKey key;
            key.MakeNewKey();
            const CBLSSignature sig{key.Sign(uint256::ONE, bls::bls_legacy_scheme.load())};
            BOOST_REQUIRE(m_node.chainlocks->UpdateBestChainlock(
                GetRandHash(), chainlock::ChainLockSig{snapshot_base_height - 1, uint256::ONE, sig}, nullptr));
            CCbTx base_cbtx;
            base_cbtx.nVersion = CCbTx::Version::CLSIG_AND_BALANCE;
            base_cbtx.nHeight = snapshot_base_height;
            base_cbtx.bestCLSignature.Set(sig, bls::bls_legacy_scheme.load());
            BlockValidationState base_state;
            BOOST_CHECK(CheckCbTxBestChainlock(base_cbtx, snapshot_base, consensus, background_chainstate->m_chain,
                                               *m_node.llmq_ctx->qman, *m_node.chainlocks, *m_node.special_tx, base_state));
            m_node.chainlocks->ResetChainlock();
        }
        snapshot_base->nStatus &= ~(BLOCK_HAVE_DATA | BLOCK_HAVE_UNDO);
        EvoViewScope snapshot_view{m_node.evodb->ViewFor(EvoDbIdentity::SNAPSHOT)};
        CCbTx cbtx;
        cbtx.nVersion = CCbTx::Version::CLSIG_AND_BALANCE;
        cbtx.nHeight = snapshot_base_height + 1;
        BlockValidationState cl_state;
        BOOST_CHECK_THROW(CheckCbTxBestChainlock(cbtx, snapshot_chainstate->m_chain[snapshot_base_height + 1],
                                                 consensus, snapshot_chainstate->m_chain, *m_node.llmq_ctx->qman,
                                                 *m_node.chainlocks, *m_node.special_tx, cl_state),
                          BlockDataUnavailableError);
        // The quorum modifier of a work block at or below the base reads the
        // same coinbase through the same lookup.
        BOOST_CHECK_THROW(GetNonNullCoinbaseChainlock(snapshot_base), BlockDataUnavailableError);
        snapshot_base->nStatus = snapshot_base_status;
        // With the body there, the coinbase is read.
        BOOST_CHECK_NO_THROW(GetNonNullCoinbaseChainlock(snapshot_base));
    }

    // Mempool acceptance that needs the EHF signals at the tip, rebuilt from its
    // pruned body, ends in an error instead of throwing, and drops the coins it
    // loaded like any failure.
    {
        const auto& funding{m_coinbase_txns[50]};
        const CMutableTransaction spend{CreateValidMempoolTransaction(funding, 0, 51, coinbaseKey,
                                                                      GetScriptForRawPubKey(coinbaseKey.GetPubKey()),
                                                                      CAmount(1 * COIN), /*submit=*/false)};
        const COutPoint input{funding->GetHash(), 0};
        // Drop every cached deployment state and EHF signal, as a restart would.
        {
            const CEvoDB unrelated{util::DbWrapperParams{.path = {}, .memory = true, .wipe = true}};
        }
        LOCK(::cs_main);
        chainman.m_versionbitscache.Clear();
        snapshot_chainstate->CoinsTip().Uncache(input);
        BOOST_REQUIRE(!snapshot_chainstate->CoinsTip().HaveCoinInCache(input));
        std::optional<MempoolAcceptResult> result;
        BOOST_CHECK_NO_THROW(result.emplace(AcceptToMemoryPool(*snapshot_chainstate, MakeTransactionRef(spend), GetTime(),
                                                               /*bypass_limits=*/false, /*test_accept=*/false)));
        BOOST_REQUIRE(result);
        BOOST_CHECK(result->m_result_type == MempoolAcceptResult::ResultType::INVALID);
        BOOST_CHECK(result->m_state.IsError());
        BOOST_CHECK(!snapshot_chainstate->CoinsTip().HaveCoinInCache(input));
    }

    // Startup's EHF signal update at the active snapshot tip cannot rebuild them
    // from its pruned body either: it leaves them to be rebuilt on demand, so
    // loading can go on, and leaves the snapshot view clean for the next flush.
    {
        LOCK(::cs_main);
        BOOST_CHECK(m_node.special_tx->m_mnhfman->ForceSignalDBUpdate(snapshot_chainstate->EvoView(), tip));
        BOOST_CHECK(m_node.evodb->CommitRootTransaction(EvoDbIdentity::SNAPSHOT));
        EvoViewScope snapshot_view{m_node.evodb->ViewFor(EvoDbIdentity::SNAPSHOT)};
        BOOST_CHECK(!m_node.evodb->Exists(std::make_pair(std::string{"mnhf_s2"}, tip->GetBlockHash())));
    }

    // Checking a child's EHF signals rebuilds those of the block it extends
    // from the bodies, and stops on a pruned body the same way.
    CBlockIndex* stalled{WITH_LOCK(::cs_main, return chainman.m_blockman.LookupBlockIndex(block->GetHash()))};
    CBlock child_template{template_block};
    child_template.hashPrevBlock = block->GetHash();
    CMutableTransaction child_coinbase{*child_template.vtx[0]};
    child_coinbase.vin[0].scriptSig = CScript{} << (tip->nHeight + 2) << CScriptNum{1};
    child_template.vtx[0] = MakeTransactionRef(std::move(child_coinbase));
    const auto child{std::make_shared<const CBlock>(
        MakeCbTxBlock(child_template, tip->nHeight + 2, CCbTx::Version::CLSIG_AND_BALANCE))};
    chainman.ProcessNewBlock(child, /*force_processing=*/true, /*new_block=*/nullptr);
    const uint32_t stalled_status{WITH_LOCK(::cs_main, return stalled->nStatus)};
    {
        LOCK(::cs_main);
        stalled->nStatus &= ~BLOCK_HAVE_DATA;
        const CBlockIndex* child_index{chainman.m_blockman.LookupBlockIndex(child->GetHash())};
        BOOST_REQUIRE(child_index);
        const auto discard{m_node.evodb->BeginTransaction(EvoDbIdentity::NORMAL)};
        BlockValidationState mnhf_state;
        BOOST_CHECK_THROW(m_node.special_tx->m_mnhfman->ProcessBlock(*child, child_index, /*fJustCheck=*/true, mnhf_state),
                          BlockDataUnavailableError);
    }
    {
        LOCK(::cs_main);
        stalled->nStatus = stalled_status;
        tip->nStatus = tip_status;
        chainman.m_blockman.m_have_pruned = false;
    }

    // With the bodies back, a lookup outside any transaction (an RPC asking for
    // deployment info) rebuilds the snapshot view's EHF signals from them. It
    // persists nothing, so the next flush finds the view clean; a lookup in a
    // block-scoped transaction persists the cached signals.
    const auto signals_key{std::make_pair(std::string{"mnhf_s2"}, tip->GetBlockHash())};
    {
        LOCK(::cs_main);
        EvoViewScope snapshot_view{m_node.evodb->ViewFor(EvoDbIdentity::SNAPSHOT)};
        BOOST_CHECK_NO_THROW(m_node.special_tx->m_mnhfman->GetSignalsStage(tip));
        BOOST_CHECK(m_node.evodb->CommitRootTransaction(EvoDbIdentity::SNAPSHOT));
        BOOST_CHECK(!m_node.evodb->Exists(signals_key));
        {
            auto tx{m_node.evodb->BeginTransaction(EvoDbIdentity::SNAPSHOT)};
            m_node.special_tx->m_mnhfman->GetSignalsStage(tip);
            tx->Commit();
        }
        BOOST_CHECK(m_node.evodb->CommitRootTransaction(EvoDbIdentity::SNAPSHOT));
        BOOST_CHECK(m_node.evodb->Exists(signals_key));
    }
    SetMiscWarning(Untranslated(""));
}

//! A peer's turn in the message handler is bound to the active Evo view and
//! contains this node's local Evo errors, starting with the GETDATA requests
//! that backpressure left queued, which resume before any message is read.
BOOST_FIXTURE_TEST_CASE(chainstatemanager_peer_turn_is_bound, SnapshotTestSetup)
{
    this->SetupSnapshot();
    // Dash message handlers run on every message the peer manager does not
    // handle itself; recovered signatures are handled as the node does.
    BOOST_REQUIRE(m_node.mn_metaman->LoadCache(/*load_cache=*/false));
    m_node.peerman->AddExtraHandler(std::make_unique<llmq::NetSigning>(m_node.peerman.get(), *m_node.llmq_ctx->sigman,
                                                                       /*shares_manager=*/nullptr, *m_node.sporkman));
    auto& connman{static_cast<ConnmanTestMsg&>(*m_node.connman)};
    PeerManager& peerman{*Assert(m_node.peerman)};
    const CEvoDB& evodb{*m_node.evodb};
    LOCK(NetEventsInterface::g_msgproc_mutex);
    in_addr peer_in_addr{};
    peer_in_addr.s_addr = htonl(0x01020304);
    CNode peer{/*id=*/0,
               /*sock=*/nullptr,
               CAddress{CService{peer_in_addr, Params().GetDefaultPort()}, NODE_NETWORK},
               /*nKeyedNetGroupIn=*/0,
               /*nLocalHostNonceIn=*/0,
               CAddress{},
               /*addrNameIn=*/"",
               ConnectionType::OUTBOUND_FULL_RELAY,
               /*inbound_onion=*/false};
    {
        // The helper reads messages outside the message handler.
        const ActiveEvoViewScope evo_view{&evodb};
        connman.Handshake(peer, /*successfully_connected=*/true, ServiceFlags(NODE_NETWORK), ServiceFlags(NODE_NETWORK),
                          PROTOCOL_VERSION, /*relay_txs=*/true);
    }
    TestOnlyResetTimeData();
    const auto sent{[&](const std::string& msg_type) {
        LOCK(peer.cs_vSend);
        return std::ranges::count_if(peer.vSendMsg, [&](const auto& msg) { return msg.m_type == msg_type; });
    }};
    const auto not_found{[&] {
        LOCK(peer.cs_vSend);
        std::vector<CInv> invs;
        for (const auto& msg : peer.vSendMsg) {
            if (msg.m_type != NetMsgType::NOTFOUND) continue;
            CDataStream stream{msg.data, SER_NETWORK, PROTOCOL_VERSION};
            std::vector<CInv> msg_invs;
            stream >> msg_invs;
            invs.insert(invs.end(), msg_invs.begin(), msg_invs.end());
        }
        return invs;
    }};

    // A recovered signature this node holds, requested while the send buffer
    // is full.
    const auto llmq_type{Consensus::LLMQType::LLMQ_TEST};
    const uint256 quorum_hash{GetRandHash()};
    const uint256 id{GetRandHash()};
    const uint256 msg_hash{GetRandHash()};
    CBLSSecretKey key;
    key.MakeNewKey();
    const auto rec_sig{
        std::make_shared<const llmq::CRecoveredSig>(llmq_type, quorum_hash, id, msg_hash,
                                                    key.Sign(llmq::SignHash{llmq_type, quorum_hash, id, msg_hash}.Get(),
                                                             bls::bls_legacy_scheme.load()))};
    BOOST_REQUIRE(m_node.llmq_ctx->sigman->ProcessRecoveredSig(rec_sig));
    const CInv rec_sig_inv{MSG_QUORUM_RECOVERED_SIG, rec_sig->GetHash()};
    const auto request_while_paused{
        [&](const std::vector<CInv>& invs) EXCLUSIVE_LOCKS_REQUIRED(NetEventsInterface::g_msgproc_mutex) {
            CDataStream getdata{SER_NETWORK, PROTOCOL_VERSION};
            getdata << invs;
            std::atomic<bool> interrupt{false};
            peer.fPauseSend = true;
            peerman.ProcessMessage(peer, NetMsgType::GETDATA, getdata, GetTime<std::chrono::microseconds>(), interrupt);
            peer.fPauseSend = false;
        }};

    // Resumed, the request is answered from the active view: its quorum is
    // not active there.
    request_while_paused({rec_sig_inv});
    BOOST_CHECK_NO_THROW(connman.ServicePeer(evodb, peer, /*send=*/true));
    BOOST_CHECK(not_found() == std::vector<CInv>{rec_sig_inv});

    // When the active view cannot answer, the turn ends without the request:
    // it is neither served again nor reported missing, a missing transaction
    // requested ahead of it still is, and the peer's next message is processed.
    {
        ScopedDIP3FromGenesis dip3_active;
        ForgetSeededScanCoverage(*m_node.evodb);
        const CInv missing_tx{MSG_TX, GetRandHash()};
        request_while_paused({missing_tx, rec_sig_inv});
        BOOST_CHECK(connman.ReceiveMsgFrom(peer, CNetMsgMaker{PROTOCOL_VERSION}.Make(NetMsgType::PING, uint64_t{1})));
        BOOST_CHECK_NO_THROW(connman.ServicePeer(evodb, peer, /*send=*/true));
        BOOST_CHECK(not_found() == (std::vector<CInv>{rec_sig_inv, missing_tx}));
        BOOST_CHECK_EQUAL(sent(NetMsgType::PONG), 0);
        // The replies leave the send buffer.
        connman.FlushSendBuffer(peer);
        peer.fPauseSend = false;
        BOOST_CHECK_NO_THROW(connman.ServicePeer(evodb, peer, /*send=*/true));
        BOOST_CHECK_EQUAL(sent(NetMsgType::PONG), 1);

        // A message whose handler cannot read this node's Evo state is dropped
        // like any other message that fails: the peer is not punished, and its
        // next message is processed on its next turn.
        const auto next_turn{[&]() EXCLUSIVE_LOCKS_REQUIRED(NetEventsInterface::g_msgproc_mutex) {
            connman.FlushSendBuffer(peer);
            peer.fPauseSend = false;
            bool more_work{false};
            BOOST_CHECK_NO_THROW(more_work = connman.ServicePeer(evodb, peer, /*send=*/true));
            return more_work;
        }};
        const CNetMsgMaker msg_maker{PROTOCOL_VERSION};
        BOOST_CHECK(connman.ReceiveMsgFrom(peer, msg_maker.Make(NetMsgType::QSIGREC, *rec_sig)));
        BOOST_CHECK(connman.ReceiveMsgFrom(peer, msg_maker.Make(NetMsgType::PING, uint64_t{2})));
        {
            ASSERT_DEBUG_LOG("ProcessMessages(qsigrec, 193 bytes): Exception 'GetMinedCommitmentsUntilBlock");
            // The queued PING is still reported as work for this peer.
            BOOST_CHECK(next_turn());
        }
        BOOST_CHECK_EQUAL(sent(NetMsgType::PONG), 0);
        next_turn();
        BOOST_CHECK_EQUAL(sent(NetMsgType::PONG), 1);
        BOOST_CHECK(!m_node.banman->IsDiscouraged(peer.addr));

        // A turn whose queued requests end early still sends: a peer due to be
        // discouraged cannot keep its connection by queueing such requests.
        peerman.UnitTestMisbehaving(peer.GetId(), DISCOURAGEMENT_THRESHOLD);
        request_while_paused({rec_sig_inv});
        next_turn();
        BOOST_CHECK(m_node.banman->IsDiscouraged(peer.addr));
    }
    peerman.FinalizeNode(peer);
}

BOOST_FIXTURE_TEST_CASE(chainstatemanager_reconsider_block_candidates, SnapshotTestSetup)
{
    auto [background_chainstate, snapshot_chainstate] = this->SetupSnapshot();
    ChainstateManager& chainman = *Assert(m_node.chainman);

    // Build a valid fork block directly on the background tip. Since the
    // background tip is the snapshot base, this block is not on the path the
    // background chainstate is allowed to validate.
    CScript script_pub_key = CScript() << ToByteVector(coinbaseKey.GetPubKey()) << OP_CHECKSIG;
    CBlock fork_block = CreateBlock(/*txns=*/{}, script_pub_key, *background_chainstate);
    BOOST_REQUIRE_EQUAL(fork_block.hashPrevBlock, WITH_LOCK(::cs_main, return background_chainstate->m_chain.Tip()->GetBlockHash()));
    BOOST_REQUIRE(chainman.ProcessNewBlock(std::make_shared<const CBlock>(fork_block), /*force_processing=*/true, /*new_block=*/nullptr));

    CBlockIndex* fork_index{WITH_LOCK(::cs_main, return chainman.m_blockman.LookupBlockIndex(fork_block.GetHash()))};
    BOOST_REQUIRE(fork_index);

    {
        LOCK(::cs_main);
        const arith_uint256 original_work{fork_index->nChainWork};
        const CBlockIndex* snapshot_base{snapshot_chainstate->SnapshotBase()};
        BOOST_REQUIRE(snapshot_base);
        BOOST_REQUIRE_EQUAL(background_chainstate->m_chain.Tip(), snapshot_base);
        BOOST_REQUIRE(!snapshot_base->GetAncestor(fork_index->nHeight));

        // Make the fork the highest-work block and simulate validation having
        // marked it failed. ResetBlockFailureFlags used to add it only to the
        // invoking background chainstate's candidate set.
        fork_index->nChainWork = snapshot_chainstate->m_chain.Tip()->nChainWork + 1;
        fork_index->nStatus |= BLOCK_FAILED_VALID;
        chainman.m_failed_blocks.insert(fork_index);
        background_chainstate->setBlockIndexCandidates.erase(fork_index);
        snapshot_chainstate->setBlockIndexCandidates.erase(fork_index);

        background_chainstate->ResetBlockFailureFlags(fork_index);

        BOOST_CHECK(fork_index->IsValid());
        BOOST_CHECK_EQUAL(background_chainstate->setBlockIndexCandidates.count(fork_index), 0);
        BOOST_CHECK_EQUAL(snapshot_chainstate->setBlockIndexCandidates.count(fork_index), 1);
        for (const CBlockIndex* candidate : background_chainstate->setBlockIndexCandidates) {
            BOOST_CHECK_EQUAL(snapshot_base->GetAncestor(candidate->nHeight), candidate);
        }

        // Restore the synthetic work value so the shared block index remains
        // internally consistent for fixture teardown.
        snapshot_chainstate->setBlockIndexCandidates.erase(fork_index);
        fork_index->nChainWork = original_work;
    }
}

//! Test LoadBlockIndex behavior when multiple chainstates are in use.
//!
//! - First, verify that setBlockIndexCandidates is as expected when using a single,
//!   fully-validating chainstate.
//!
//! - Then mark a region of the chain BLOCK_ASSUMED_VALID and introduce a second chainstate
//!   that will tolerate assumed-valid blocks. Run LoadBlockIndex() and ensure that the first
//!   chainstate only contains fully validated blocks and the other chainstate contains all blocks,
//!   except those marked assume-valid, because those entries don't HAVE_DATA.
//!
BOOST_FIXTURE_TEST_CASE(chainstatemanager_loadblockindex, TestChain100Setup)
{
    ChainstateManager& chainman = *Assert(m_node.chainman);
    CTxMemPool& mempool = *m_node.mempool;
    Chainstate& cs1 = chainman.ActiveChainstate();

    int num_indexes{0};
    int num_assumed_valid{0};
    const int expected_assumed_valid{20};
    const int last_assumed_valid_idx{40};
    const int assumed_valid_start_idx = last_assumed_valid_idx - expected_assumed_valid;

    CBlockIndex* validated_tip{nullptr};
    CBlockIndex* assumed_base{nullptr};
    CBlockIndex* assumed_tip{WITH_LOCK(chainman.GetMutex(), return chainman.ActiveChain().Tip())};

    auto reload_all_block_indexes = [&]() {
        // For completeness, we also reset the block sequence counters to
        // ensure that no state which affects the ranking of tip-candidates is
        // retained (even though this isn't strictly necessary).
        WITH_LOCK(::cs_main, return chainman.ResetBlockSequenceCounters());
        for (Chainstate* cs : chainman.GetAll()) {
            LOCK(::cs_main);
            cs->ClearBlockIndexCandidates();
            BOOST_CHECK(cs->setBlockIndexCandidates.empty());
        }

        WITH_LOCK(::cs_main, chainman.LoadBlockIndex());
    };

    // Ensure that without any assumed-valid BlockIndex entries, only the current tip is
    // considered as a candidate.
    reload_all_block_indexes();
    BOOST_CHECK_EQUAL(cs1.setBlockIndexCandidates.size(), 1);

    // Mark some region of the chain assumed-valid, and remove the HAVE_DATA flag.
    for (int i = 0; i <= cs1.m_chain.Height(); ++i) {
        LOCK(::cs_main);
        auto index = cs1.m_chain[i];

        // Blocks with heights in range [20, 40) are marked ASSUMED_VALID
        if (i < last_assumed_valid_idx && i >= assumed_valid_start_idx) {
            index->nStatus = BlockStatus::BLOCK_VALID_TREE | BlockStatus::BLOCK_ASSUMED_VALID;
        }

        ++num_indexes;
        if (index->IsAssumedValid()) ++num_assumed_valid;

        // Note the last fully-validated block as the expected validated tip.
        if (i == (assumed_valid_start_idx - 1)) {
            validated_tip = index;
            BOOST_CHECK(!index->IsAssumedValid());
        }
        // Note the last assumed valid block as the snapshot base
        if (i == last_assumed_valid_idx - 1) {
            assumed_base = index;
            BOOST_CHECK(index->IsAssumedValid());
        } else if (i == last_assumed_valid_idx) {
            BOOST_CHECK(!index->IsAssumedValid());
        }
    }

    BOOST_CHECK_EQUAL(expected_assumed_valid, num_assumed_valid);

    const uint256 snapshot_blockhash = assumed_base->GetBlockHash();
    SeedSnapshotMarker(*m_node.evodb, snapshot_blockhash);
    Chainstate* cs2_ptr = WITH_LOCK(::cs_main,
        return chainman.ActivateExistingSnapshot(&mempool, snapshot_blockhash));
    BOOST_REQUIRE(cs2_ptr);
    Chainstate& cs2 = *cs2_ptr;

    // Note: cs2's tip is not set when ActivateExistingSnapshot is called.
    // Set tip of the fully validated chain to be the validated tip
    cs1.m_chain.SetTip(*validated_tip);

    // Set tip of the assume-valid-based chain to the assume-valid block
    cs2.m_chain.SetTip(*assumed_base);

    reload_all_block_indexes();

    // The fully validated chain should have the current validated tip
    // and the assumed valid base as candidates.
    BOOST_CHECK_EQUAL(cs1.setBlockIndexCandidates.size(), 2);
    BOOST_CHECK_EQUAL(cs1.setBlockIndexCandidates.count(validated_tip), 1);
    BOOST_CHECK_EQUAL(cs1.setBlockIndexCandidates.count(assumed_base), 1);

    // The assumed-valid tolerant chain has the assumed valid base as a
    // candidate, but otherwise has none of the assumed-valid (which do not
    // HAVE_DATA) blocks as candidates.
    BOOST_CHECK_EQUAL(cs2.setBlockIndexCandidates.count(validated_tip), 0);
    BOOST_CHECK_EQUAL(cs2.setBlockIndexCandidates.count(assumed_tip), 1);
    BOOST_CHECK_EQUAL(cs2.setBlockIndexCandidates.size(), num_indexes - last_assumed_valid_idx + 1);
}

//! Ensure that snapshot chainstates initialize properly when found on disk.
BOOST_FIXTURE_TEST_CASE(chainstatemanager_snapshot_init, SnapshotTestSetup)
{
    ChainstateManager& chainman = *Assert(m_node.chainman);
    Chainstate& bg_chainstate = chainman.ActiveChainstate();

    this->SetupSnapshot();

    fs::path snapshot_chainstate_dir = *node::FindSnapshotChainstateDir();
    BOOST_CHECK(fs::exists(snapshot_chainstate_dir));
    BOOST_CHECK_EQUAL(snapshot_chainstate_dir, gArgs.GetDataDirNet() / "chainstate_snapshot");

    BOOST_CHECK(chainman.IsSnapshotActive());
    const uint256 snapshot_tip_hash = WITH_LOCK(chainman.GetMutex(),
        return chainman.ActiveTip()->GetBlockHash());

    auto all_chainstates = chainman.GetAll();
    BOOST_CHECK_EQUAL(all_chainstates.size(), 2);

    // Runtime activation moved the mempool to the (active) snapshot chainstate.
    BOOST_CHECK_EQUAL(chainman.ActiveChainstate().GetMempool(), m_node.mempool.get());
    BOOST_CHECK(!bg_chainstate.GetMempool());

    // "Rewind" the background chainstate so that its tip is not at the
    // base block of the snapshot - this is so after simulating a node restart,
    // it will initialize instead of attempting to complete validation.
    //
    // Note that this is not a realistic use of DisconnectTip().
    DisconnectedBlockTransactions unused_pool{MAX_DISCONNECTED_TX_POOL_BYTES};
    BlockValidationState unused_state;
    {
        LOCK2(::cs_main, bg_chainstate.MempoolMutex());
        BOOST_CHECK(bg_chainstate.DisconnectTip(unused_state, &unused_pool));
        unused_pool.clear();  // to avoid queuedTx assertion errors on teardown
    }
    BOOST_CHECK_EQUAL(bg_chainstate.m_chain.Height(), 109);

    // Test that simulating a shutdown (resetting ChainstateManager) and then performing
    // chainstate reinitializing successfully cleans up the background-validation
    // chainstate data, and we end up with a single chainstate that is at tip.
    ChainstateManager& chainman_restarted = this->SimulateNodeRestart();

    BOOST_TEST_MESSAGE("Performing Load/Verify/Activate of chainstate");

    // This call reinitializes the chainstates.
    this->LoadVerifyActivateChainstate();

    {
        LOCK(chainman_restarted.GetMutex());
        BOOST_CHECK_EQUAL(chainman_restarted.GetAll().size(), 2);
        BOOST_CHECK(chainman_restarted.IsSnapshotActive());
        BOOST_CHECK(!chainman_restarted.IsSnapshotValidated());

        BOOST_CHECK_EQUAL(chainman_restarted.ActiveTip()->GetBlockHash(), snapshot_tip_hash);
        BOOST_CHECK_EQUAL(chainman_restarted.ActiveHeight(), 210);
    }

    // The startup tail that follows the block import runs on its own thread:
    // it must announce the restarted tip, warm caches and check EvoDB repair
    // bound to the active view, not abort on unbound Evo reads.
    {
        class StartupTipReader final : public CValidationInterface
        {
        public:
            explicit StartupTipReader(CDeterministicMNManager& dmnman) :
                m_dmnman{dmnman}
            {
            }
            void InitializeCurrentBlockTip(const CBlockIndex* tip, bool) override
            {
                m_dmnman.UpdatedBlockTip(tip);
                ++announced;
            }
            int announced{0};

        private:
            CDeterministicMNManager& m_dmnman;
        } tip_reader{*m_node.dmnman};
        RegisterValidationInterface(&tip_reader);
        BOOST_CHECK(!m_node.evodb->IsBound());
        BOOST_CHECK_NO_THROW(FinishImportedStartup(m_node, m_args, /*skip_evodb_repair_on_reindex=*/false));
        UnregisterValidationInterface(&tip_reader);
        BOOST_CHECK_EQUAL(tip_reader.announced, 1);
    }

    // Active-chain reads through the RPC dispatcher bind the active view.
    BOOST_CHECK_NO_THROW(this->CallRPC("getblockchaininfo"));
    UniValue count_params{UniValue::VARR};
    count_params.push_back("count");
    BOOST_CHECK_NO_THROW(this->CallRPC("masternode", count_params));

    BOOST_TEST_MESSAGE(
        "Ensure we can mine blocks on top of the initialized snapshot chainstate");
    mineBlocks(10);
    {
        LOCK(chainman_restarted.GetMutex());
        BOOST_CHECK_EQUAL(chainman_restarted.ActiveHeight(), 220);

        // Background chainstate should be unaware of new blocks on the snapshot
        // chainstate.
        for (Chainstate* cs : chainman_restarted.GetAll()) {
            if (cs != &chainman_restarted.ActiveChainstate()) {
                BOOST_CHECK_EQUAL(cs->m_chain.Height(), 109);
            }
        }
    }
}

BOOST_FIXTURE_TEST_CASE(chainstatemanager_evodb_reorg_erase_guard, SnapshotTestSetup)
{
    auto [background_chainstate, snapshot_chainstate] = this->SetupSnapshot();
    const auto llmq_type = Consensus::LLMQType::LLMQ_TEST;
    const uint256 shared_quorum_hash = GetRandHash();
    const uint256 snapshot_only_quorum_hash = GetRandHash();
    const auto shared_key = std::make_pair(std::string{"q_mc"}, std::make_pair(llmq_type, shared_quorum_hash));
    const auto snapshot_only_key = std::make_pair(std::string{"q_mc"}, std::make_pair(llmq_type, snapshot_only_quorum_hash));
    const CBlockIndex* shared_block;
    const CBlockIndex* snapshot_only_block;
    {
        LOCK(::cs_main);
        shared_block = snapshot_chainstate->m_chain[background_chainstate->m_chain.Height()];
        snapshot_only_block = snapshot_chainstate->m_chain[background_chainstate->m_chain.Height() + 1];
        BOOST_REQUIRE(background_chainstate->m_chain.Contains(shared_block));
        BOOST_REQUIRE(!background_chainstate->m_chain.Contains(snapshot_only_block));
    }

    // Constructing a mined quorum commitment through snapshot activation is
    // impractical here, so exercise the production erase guard with its real
    // second Chainstate and synthetic commitment keys written through EvoDB.
    {
        LOCK(::cs_main);
        auto tx = m_node.evodb->BeginTransaction(EvoDbIdentity::SNAPSHOT);
        m_node.evodb->Write(shared_key, shared_block->GetBlockHash());
        m_node.evodb->Write(snapshot_only_key, snapshot_only_block->GetBlockHash());
        tx->Commit();
    }
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return m_node.evodb->CommitRootTransaction(EvoDbIdentity::SNAPSHOT)));

    {
        LOCK(::cs_main);
        auto tx = m_node.evodb->BeginTransaction(EvoDbIdentity::SNAPSHOT);
        BOOST_CHECK(!llmq::EraseMinedCommitmentIfUnreferenced(*m_node.evodb, *snapshot_chainstate, shared_block,
                                                              llmq_type, shared_quorum_hash));
        BOOST_CHECK(llmq::EraseMinedCommitmentIfUnreferenced(*m_node.evodb, *snapshot_chainstate, snapshot_only_block,
                                                             llmq_type, snapshot_only_quorum_hash));
        tx->Commit();
    }

    {
        LOCK(::cs_main);
        auto tx = m_node.evodb->BeginTransaction(EvoDbIdentity::SNAPSHOT);
        uint256 value;
        BOOST_CHECK(m_node.evodb->Read(shared_key, value));
        BOOST_CHECK(!m_node.evodb->Read(snapshot_only_key, value));
    }
}

BOOST_FIXTURE_TEST_CASE(chainstatemanager_mined_commitment_is_chain_aware, SnapshotTestSetup)
{
    auto [background_chainstate, snapshot_chainstate] = this->SetupSnapshot();
    const auto llmq_type = Consensus::LLMQType::LLMQ_TEST;
    const auto llmq_params = Params().GetLLMQ(llmq_type).value();
    const int target_height = background_chainstate->m_chain.Height() + 1;
    BOOST_REQUIRE_GE(target_height % llmq_params.dkgInterval, llmq_params.dkgMiningWindowStart);
    BOOST_REQUIRE_LE(target_height % llmq_params.dkgInterval, llmq_params.dkgMiningWindowEnd);

    const CBlockIndex* quorum_base;
    const CBlockIndex* snapshot_mined_block;
    {
        LOCK(::cs_main);
        quorum_base = background_chainstate->m_chain[target_height - (target_height % llmq_params.dkgInterval)];
        snapshot_mined_block = snapshot_chainstate->m_chain[target_height];
        BOOST_REQUIRE(quorum_base);
        BOOST_REQUIRE(snapshot_mined_block);
        BOOST_REQUIRE(background_chainstate->m_chain.Contains(quorum_base));
        BOOST_REQUIRE(!background_chainstate->m_chain.Contains(snapshot_mined_block));
        BOOST_REQUIRE(snapshot_chainstate->m_chain.Contains(snapshot_mined_block));
    }

    const uint256 quorum_hash = quorum_base->GetBlockHash();
    const auto key = std::make_pair(std::string{"q_mc"}, std::make_pair(llmq_type, quorum_hash));
    const llmq::CFinalCommitment seeded_commitment{llmq_params, quorum_hash};
    {
        LOCK(::cs_main);
        auto tx = m_node.evodb->BeginTransaction(EvoDbIdentity::SNAPSHOT);
        m_node.evodb->Write(key, std::make_pair(seeded_commitment, snapshot_mined_block->GetBlockHash()));
        tx->Commit();
    }
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return m_node.evodb->CommitRootTransaction(EvoDbIdentity::SNAPSHOT)));

    auto& qblockman = *Assert(m_node.llmq_ctx)->quorum_block_processor;
    auto& qman = *Assert(m_node.llmq_ctx)->qman;
    {
        LOCK(::cs_main);
        // The seeded record lives in the snapshot view.
        EvoViewScope snapshot_view{snapshot_chainstate->EvoView()};
        BOOST_CHECK(qblockman.HasMinedCommitment(llmq_type, quorum_hash));
        BOOST_CHECK(qblockman.HasMinedCommitment(llmq_type, quorum_hash, snapshot_chainstate->m_chain));
        BOOST_CHECK(!qblockman.HasMinedCommitment(llmq_type, quorum_hash, background_chainstate->m_chain));

        BOOST_CHECK(llmq::CQuorumManager::HasQuorum(llmq_type, qblockman, quorum_hash));
        BOOST_CHECK(llmq::CQuorumManager::HasQuorum(llmq_type, qblockman, quorum_hash,
                                                    snapshot_chainstate->m_chain));
        BOOST_CHECK(!llmq::CQuorumManager::HasQuorum(llmq_type, qblockman, quorum_hash,
                                                     background_chainstate->m_chain));

        // Exercise quorum resolution itself, including the cache-order hazard:
        // a snapshot-only commitment must not resolve for the background
        // chain, even after the snapshot lookup has populated the quorum cache.
        BOOST_CHECK(qman.GetQuorum(llmq_type, quorum_hash, background_chainstate->m_chain) == nullptr);
        BOOST_CHECK(qman.GetQuorum(llmq_type, quorum_hash, snapshot_chainstate->m_chain) != nullptr);
        BOOST_CHECK(qman.GetQuorum(llmq_type, quorum_hash, background_chainstate->m_chain) == nullptr);

        // ConnectBlock accepts exactly this required count. The record from
        // snapshot chain A therefore cannot suppress the commitment expected
        // in chain B's block at the same height.
        BOOST_CHECK_EQUAL(qblockman.GetNumCommitmentsRequired(llmq_params, snapshot_chainstate->m_chain, target_height), 0);
        BOOST_CHECK_EQUAL(qblockman.GetNumCommitmentsRequired(llmq_params, background_chainstate->m_chain, target_height), 1);

        CBlock block;
        BOOST_REQUIRE(node::ReadBlockFromDisk(block, snapshot_mined_block, Params().GetConsensus()));
        BlockValidationState state;
        EvoViewScope background_view{background_chainstate->EvoView()};
        BOOST_CHECK(qblockman.ProcessBlock(*background_chainstate, block, snapshot_mined_block, state,
                                           /*fJustCheck=*/true, /*fBLSChecks=*/false));
    }

    // Block templates, built before DIP3 is moved to genesis below.
    const CBlock signal_template{CreateBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()), *snapshot_chainstate)};
    const CBlockIndex* snapshot_tip{WITH_LOCK(::cs_main, return snapshot_chainstate->m_chain.Tip())};

    // Before DIP3 no quorum is mined, so while DIP3 activates above the base,
    // the snapshot view's empty history at or below it is authoritative.
    const auto platform_type{Params().GetConsensus().llmqTypePlatform};
    {
        LOCK(::cs_main);
        EvoViewScope snapshot_view{m_node.evodb->ViewFor(EvoDbIdentity::SNAPSHOT)};
        BOOST_CHECK(qblockman.GetMinedCommitmentsUntilBlock(platform_type, snapshot_tip, 1).empty());
    }

    // With DIP3 active at the base, a commitment only NORMAL has, for a quorum
    // at or below the base, is history the snapshot view lacks: a miss there is
    // unavailable data, not a quorum that was never mined. Above the base, a
    // miss is authoritative.
    ScopedDIP3FromGenesis dip3_active;
    ForgetSeededScanCoverage(*m_node.evodb);
    const CBlockIndex* earlier_base{
        WITH_LOCK(::cs_main, return background_chainstate->m_chain[quorum_base->nHeight - llmq_params.dkgInterval])};
    const uint256 normal_only_hash{earlier_base->GetBlockHash()};
    {
        LOCK(::cs_main);
        const CBlockIndex* normal_mined_block{
            background_chainstate->m_chain[earlier_base->nHeight + llmq_params.dkgMiningWindowStart]};
        auto tx = m_node.evodb->BeginTransaction(EvoDbIdentity::NORMAL);
        m_node.evodb->Write(std::make_pair(std::string{"q_mc"}, std::make_pair(llmq_type, normal_only_hash)),
                            std::make_pair(llmq::CFinalCommitment{llmq_params, normal_only_hash},
                                           normal_mined_block->GetBlockHash()));
        tx->Commit();
    }
    {
        LOCK(::cs_main);
        EvoViewScope background_view{m_node.evodb->ViewFor(EvoDbIdentity::NORMAL)};
        BOOST_CHECK(qblockman.HasMinedCommitment(llmq_type, normal_only_hash, background_chainstate->m_chain));
    }
    {
        LOCK(::cs_main);
        EvoViewScope snapshot_view{m_node.evodb->ViewFor(EvoDbIdentity::SNAPSHOT)};
        BOOST_CHECK_THROW(qblockman.HasMinedCommitment(llmq_type, normal_only_hash, snapshot_chainstate->m_chain),
                          BlockDataUnavailableError);
        BOOST_CHECK_THROW(qman.GetQuorum(llmq_type, normal_only_hash, snapshot_chainstate->m_chain),
                          BlockDataUnavailableError);
        BOOST_CHECK(!qblockman.HasMinedCommitment(llmq_type, snapshot_mined_block->GetBlockHash(),
                                                  snapshot_chainstate->m_chain));
    }

    // So a block whose EHF signal names that quorum stalls instead of failing.
    ChainstateManager& chainman{*Assert(m_node.chainman)};
    Consensus::Params& consensus{const_cast<Consensus::Params&>(Params().GetConsensus())};
    struct V20HeightRestore {
        Consensus::Params& params;
        const int height;
        ~V20HeightRestore() { params.V20Height = height; }
    } v20_restore{consensus, std::exchange(consensus.V20Height, 1)};
    MNHFTxPayload signal;
    signal.signal.versionBit = consensus.vDeployments[Consensus::DEPLOYMENT_V24].bit;
    signal.signal.quorumHash = normal_only_hash;
    CMutableTransaction signal_tx;
    signal_tx.nVersion = CTransaction::SPECIAL_VERSION;
    signal_tx.nType = TRANSACTION_MNHF_SIGNAL;
    SetTxPayload(signal_tx, signal);
    CBlock signal_block{MakeCbTxBlock(signal_template, snapshot_tip->nHeight + 1, CCbTx::Version::CLSIG_AND_BALANCE)};
    signal_block.vtx.push_back(MakeTransactionRef(std::move(signal_tx)));
    signal_block.hashMerkleRoot = BlockMerkleRoot(signal_block);
    while (!CheckProofOfWork(signal_block.GetHash(), signal_block.nBits, consensus))
        ++signal_block.nNonce;
    const auto signal_block_ref{std::make_shared<const CBlock>(signal_block)};
    chainman.ProcessNewBlock(signal_block_ref, /*force_processing=*/true, /*new_block=*/nullptr);
    BlockValidationState state;
    BOOST_CHECK(!snapshot_chainstate->ActivateBestChain(state, signal_block_ref));
    BOOST_CHECK(IsLocalEvoStateError(state));
    {
        LOCK(::cs_main);
        const CBlockIndex* index{chainman.m_blockman.LookupBlockIndex(signal_block.GetHash())};
        BOOST_REQUIRE(index);
        BOOST_CHECK_EQUAL(index->nStatus & BLOCK_FAILED_MASK, 0U);
        BOOST_CHECK_EQUAL(snapshot_chainstate->m_chain.Tip(), snapshot_tip);
    }

    // A scan by height the snapshot view cannot satisfy from the commitments
    // mined above the base is unknown, not short: a Platform quorum mined at or
    // below the base, recorded only in NORMAL, is missing from its result. The
    // view is isolated from NORMAL, so its answer cannot depend on that record;
    // NORMAL resolving it shows the history the scan needs does exist.
    {
        LOCK(::cs_main);
        const CBlockIndex* platform_mined_block{
            background_chainstate->m_chain[earlier_base->nHeight + llmq_params.dkgMiningWindowStart]};
        auto tx = m_node.evodb->BeginTransaction(EvoDbIdentity::NORMAL);
        // The keys mirror DB_MINED_COMMITMENT and DB_MINED_COMMITMENT_BY_INVERSED_HEIGHT
        // in llmq/blockprocessor.cpp.
        m_node.evodb->Write(std::make_pair(std::string{"q_mc"}, std::make_pair(platform_type, normal_only_hash)),
                            std::make_pair(llmq::CFinalCommitment{Params().GetLLMQ(platform_type).value(), normal_only_hash},
                                           platform_mined_block->GetBlockHash()));
        m_node.evodb->Write(std::make_tuple(std::string{"q_mcih"}, platform_type,
                                            htobe32_internal(std::numeric_limits<uint32_t>::max() -
                                                             platform_mined_block->nHeight)),
                            earlier_base->nHeight);
        tx->Commit();
    }
    {
        LOCK(::cs_main);
        EvoViewScope background_view{m_node.evodb->ViewFor(EvoDbIdentity::NORMAL)};
        BOOST_CHECK(qblockman.GetMinedCommitmentsUntilBlock(platform_type, background_chainstate->m_chain.Tip(), 1) ==
                    std::vector<const CBlockIndex*>{earlier_base});
    }
    {
        LOCK(::cs_main);
        EvoViewScope snapshot_view{m_node.evodb->ViewFor(EvoDbIdentity::SNAPSHOT)};
        BOOST_CHECK_THROW(qblockman.GetMinedCommitmentsUntilBlock(platform_type, snapshot_tip, 1),
                          BlockDataUnavailableError);
        BOOST_CHECK_THROW(qman.ScanQuorums(platform_type, snapshot_tip, 1, snapshot_chainstate->m_chain),
                          BlockDataUnavailableError);
    }

    // So a block withdrawing from Platform under that quorum stalls instead of
    // failing as signed by a quorum too old.
    CMutableTransaction unlock_tx;
    unlock_tx.nVersion = CTransaction::SPECIAL_VERSION;
    unlock_tx.nType = TRANSACTION_ASSET_UNLOCK;
    unlock_tx.vout.emplace_back(COIN, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
    SetTxPayload(unlock_tx,
                 CAssetUnlockPayload{CAssetUnlockPayload::INITIAL_VERSION, /*index=*/1, /*fee=*/1000,
                                     static_cast<uint32_t>(snapshot_tip->nHeight), normal_only_hash, CBLSSignature{}});
    CBlock unlock_block{MakeCbTxBlock(signal_template, snapshot_tip->nHeight + 1, CCbTx::Version::CLSIG_AND_BALANCE)};
    unlock_block.vtx.push_back(MakeTransactionRef(std::move(unlock_tx)));
    unlock_block.hashMerkleRoot = BlockMerkleRoot(unlock_block);
    while (!CheckProofOfWork(unlock_block.GetHash(), unlock_block.nBits, consensus))
        ++unlock_block.nNonce;
    const auto unlock_block_ref{std::make_shared<const CBlock>(unlock_block)};
    chainman.ProcessNewBlock(unlock_block_ref, /*force_processing=*/true, /*new_block=*/nullptr);
    BOOST_CHECK(!snapshot_chainstate->ActivateBestChain(state, unlock_block_ref));
    BOOST_CHECK(IsLocalEvoStateError(state));
    {
        LOCK(::cs_main);
        const CBlockIndex* index{chainman.m_blockman.LookupBlockIndex(unlock_block.GetHash())};
        BOOST_REQUIRE(index);
        BOOST_CHECK_EQUAL(index->nStatus & BLOCK_FAILED_MASK, 0U);
        BOOST_CHECK_EQUAL(snapshot_chainstate->m_chain.Tip(), snapshot_tip);
    }

    // The same holds for the rotating per-index scan, and a scan that the
    // commitments mined above the base do satisfy is complete.
    const auto rotating_type{Consensus::LLMQType::LLMQ_TEST_DIP0024};
    {
        LOCK(::cs_main);
        EvoViewScope background_view{m_node.evodb->ViewFor(EvoDbIdentity::NORMAL)};
        BOOST_CHECK(!qblockman.GetLastMinedCommitmentsByQuorumIndexUntilBlock(rotating_type, snapshot_tip, 0, 0));
    }
    {
        LOCK(::cs_main);
        auto tx = m_node.evodb->BeginTransaction(EvoDbIdentity::SNAPSHOT);
        // The keys mirror DB_MINED_COMMITMENT_BY_INVERSED_HEIGHT(_Q_INDEXED).
        const uint32_t mined_key{htobe32_internal(std::numeric_limits<uint32_t>::max() - snapshot_mined_block->nHeight)};
        m_node.evodb->Write(std::make_tuple(std::string{"q_mcih"}, platform_type, mined_key), quorum_base->nHeight);
        m_node.evodb->Write(std::make_tuple(std::string{"q_mcihi"}, rotating_type, 0, mined_key), quorum_base->nHeight);
        tx->Commit();
    }
    {
        LOCK(::cs_main);
        EvoViewScope snapshot_view{m_node.evodb->ViewFor(EvoDbIdentity::SNAPSHOT)};
        BOOST_CHECK_THROW(qblockman.GetLastMinedCommitmentsByQuorumIndexUntilBlock(rotating_type, snapshot_tip, 0, 1),
                          BlockDataUnavailableError);
        BOOST_CHECK(qblockman.GetLastMinedCommitmentsByQuorumIndexUntilBlock(rotating_type, snapshot_tip, 0, 0) ==
                    quorum_base);
        BOOST_CHECK(qblockman.GetMinedCommitmentsUntilBlock(platform_type, snapshot_tip, 1) ==
                    std::vector<const CBlockIndex*>{quorum_base});
        BOOST_CHECK_THROW(qblockman.GetMinedCommitmentsUntilBlock(platform_type, snapshot_tip, 2),
                          BlockDataUnavailableError);

        // Nothing can be mined below DIP3, so a short scan from there is complete.
        const CBlockIndex* pre_dip3{snapshot_tip->GetAncestor(Params().GetConsensus().DIP0003Height - 1)};
        BOOST_REQUIRE(pre_dip3);
        BOOST_CHECK(qblockman.GetMinedCommitmentsUntilBlock(platform_type, pre_dip3, 1).empty());
        BOOST_CHECK(!qblockman.GetLastMinedCommitmentsByQuorumIndexUntilBlock(rotating_type, pre_dip3, 0, 0));
    }
    SetMiscWarning(Untranslated(""));
}

BOOST_FIXTURE_TEST_CASE(chainstatemanager_evodb_snapshot_only_flush_restart, SnapshotTestSetup)
{
    auto chainstates = this->SetupSnapshot();
    Chainstate* background_chainstate = std::get<0>(chainstates);
    ChainstateManager& chainman = *Assert(m_node.chainman);

    // Keep this M2 marker-independence test below completion height; #25740 now
    // completes and cleans up immediately on restart when background is at base.
    DisconnectedBlockTransactions unused_pool{MAX_DISCONNECTED_TX_POOL_BYTES};
    BlockValidationState unused_state;
    {
        LOCK2(::cs_main, background_chainstate->MempoolMutex());
        BOOST_CHECK(background_chainstate->DisconnectTip(unused_state, &unused_pool));
        unused_pool.clear();
        background_chainstate->TryAddBlockIndexCandidate(background_chainstate->m_chain.Tip());
        background_chainstate->ForceFlushStateToDisk();
    }
    BOOST_CHECK_EQUAL(background_chainstate->m_chain.Height(), 109);

    uint256 normal_marker;
    BOOST_REQUIRE(m_node.evodb->ReadBestBlock(EvoDbIdentity::NORMAL, normal_marker));

    mineBlocks(1);
    const uint256 snapshot_marker = WITH_LOCK(chainman.GetMutex(), return chainman.ActiveTip()->GetBlockHash());
    WITH_LOCK(::cs_main, chainman.ActiveChainstate().ForceFlushStateToDisk());

    ChainstateManager& restarted = this->SimulateNodeRestart(/*flush_chainstates=*/false);
    this->LoadVerifyActivateChainstate();

    BOOST_CHECK(m_node.evodb->VerifyBestBlock(EvoDbIdentity::SNAPSHOT, snapshot_marker));
    BOOST_CHECK(m_node.evodb->VerifyBestBlock(EvoDbIdentity::NORMAL, normal_marker));
    {
        LOCK(::cs_main);
        BOOST_REQUIRE_EQUAL(restarted.GetAll().size(), 2);
        for (Chainstate* chainstate : restarted.GetAll()) {
            BOOST_CHECK(m_node.evodb->VerifyBestBlock(chainstate->EvoDbIdentity(), chainstate->CoinsTip().GetBestBlock()));
        }
    }
}

BOOST_FIXTURE_TEST_CASE(chainstatemanager_evodb_legacy_pair_after_snapshot, SnapshotTestSetup)
{
    auto [background_chainstate, snapshot_chainstate] = this->SetupSnapshot();
    WITH_LOCK(::cs_main, background_chainstate->ForceFlushStateToDisk());
    WITH_LOCK(::cs_main, snapshot_chainstate->ForceFlushStateToDisk());

    uint256 legacy_marker;
    BOOST_REQUIRE(m_node.evodb->GetRawDB().Read(EVODB_BEST_BLOCK, legacy_marker));
    const uint256 background_coins_tip = WITH_LOCK(::cs_main, return background_chainstate->CoinsTip().GetBestBlock());
    BOOST_CHECK_EQUAL(legacy_marker, background_coins_tip);
}

BOOST_FIXTURE_TEST_CASE(chainstatemanager_snapshot_init_missing_evodb_marker, SnapshotTestSetup)
{
    this->SetupSnapshot();

    {
        LOCK(::cs_main);
        auto tx = m_node.evodb->BeginTransaction(EvoDbIdentity::SNAPSHOT);
        m_node.evodb->Erase(std::make_pair(EVODB_BEST_BLOCK, uint8_t{1}));
        tx->Commit();
    }
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return m_node.evodb->CommitRootTransaction(EvoDbIdentity::SNAPSHOT)));

    ChainstateManager& restarted = this->SimulateNodeRestart();
    MakeDashChainContexts(/*llmq_dbs_wipe=*/false);
    WITH_LOCK(::cs_main, restarted.InitializeChainstate(
        m_node.mempool.get(), *m_node.evodb, *Assert(m_node.special_tx),
        *Assert(m_node.chainlocks), m_node.isman.get()));

    bilingual_str error;
    BOOST_CHECK(!WITH_LOCK(::cs_main, return restarted.DetectSnapshotChainstate(m_node.mempool.get(), error)));
    BOOST_CHECK(error.original.find("Snapshot chainstate EvoDB marker") != std::string::npos);

    WITH_LOCK(::cs_main, restarted.ResetChainstates());
    fs::remove_all(gArgs.GetDataDirNet() / "chainstate_snapshot");
    BOOST_REQUIRE(m_node.evodb->DiscardSnapshotMarkers());
    this->LoadVerifyActivateChainstate();
}

//! Reindexing wipes the shared EvoDB, taking the SNAPSHOT best-block marker with
//! it. Startup must discard the persisted snapshot chainstate too, or every
//! subsequent start would fail on the erased marker and tell the user to reindex.
BOOST_FIXTURE_TEST_CASE(chainstatemanager_snapshot_discarded_on_reindex, SnapshotTestSetup)
{
    this->SetupSnapshot();
    BOOST_REQUIRE(node::FindSnapshotChainstateDir());

    this->SimulateNodeRestart();
    m_args.ForceSetArg("-reindex-chainstate", "1");
    this->LoadVerifyActivateChainstate();
    m_args.ForceSetArg("-reindex-chainstate", "0");

    ChainstateManager& reindexed = *Assert(m_node.chainman);
    LOCK(::cs_main);
    BOOST_CHECK(!node::FindSnapshotChainstateDir());
    BOOST_CHECK(!reindexed.IsSnapshotActive());
    BOOST_CHECK_EQUAL(reindexed.GetAll().size(), 1);
    uint256 stale_marker;
    BOOST_CHECK(!m_node.evodb->ReadBestBlock(EvoDbIdentity::SNAPSHOT, stale_marker));
}

BOOST_FIXTURE_TEST_CASE(chainstatemanager_snapshot_completion, SnapshotTestSetup)
{
    this->SetupSnapshot();

    ChainstateManager& chainman = *Assert(m_node.chainman);
    Chainstate& active_cs = chainman.ActiveChainstate();
    auto tip_cache_before_complete = active_cs.m_coinstip_cache_size_bytes;
    auto db_cache_before_complete = active_cs.m_coinsdb_cache_size_bytes;

    SnapshotCompletionResult res;
    auto mock_shutdown = [](bilingual_str msg) {};

    fs::path snapshot_chainstate_dir = *node::FindSnapshotChainstateDir();
    BOOST_CHECK(fs::exists(snapshot_chainstate_dir));
    BOOST_CHECK_EQUAL(snapshot_chainstate_dir, gArgs.GetDataDirNet() / "chainstate_snapshot");

    BOOST_CHECK(chainman.IsSnapshotActive());
    const uint256 snapshot_tip_hash = WITH_LOCK(chainman.GetMutex(),
        return chainman.ActiveTip()->GetBlockHash());

    // Evo state the snapshot chainstate wrote above its base stays out of
    // NORMAL until promotion.
    const SnapshotViewRecords snapshot_records{
        WITH_LOCK(::cs_main, return SnapshotViewRecords::Write(*m_node.evodb, chainman.ActiveChainstate().m_chain[150]))};
    BOOST_CHECK(snapshot_records.ReadByNormal(m_node) == std::make_pair(false, false));

    res = WITH_LOCK(::cs_main,
        return chainman.MaybeCompleteSnapshotValidation(mock_shutdown));
    BOOST_CHECK_EQUAL(res, SnapshotCompletionResult::SUCCESS);

    WITH_LOCK(::cs_main, BOOST_CHECK(chainman.IsSnapshotValidated()));
    BOOST_CHECK(chainman.IsSnapshotActive());


    // Cache should have been rebalanced and reallocated to the "only" remaining
    // chainstate.
    BOOST_CHECK(active_cs.m_coinstip_cache_size_bytes > tip_cache_before_complete);
    BOOST_CHECK(active_cs.m_coinsdb_cache_size_bytes > db_cache_before_complete);

    auto all_chainstates = chainman.GetAll();
    BOOST_CHECK_EQUAL(all_chainstates.size(), 1);
    BOOST_CHECK_EQUAL(all_chainstates[0], &active_cs);

    // Trying completion again should return false.
    res = WITH_LOCK(::cs_main,
        return chainman.MaybeCompleteSnapshotValidation(mock_shutdown));
    BOOST_CHECK_EQUAL(res, SnapshotCompletionResult::SKIPPED);

    // The invalid snapshot path should not have been used.
    fs::path snapshot_invalid_dir = gArgs.GetDataDirNet() / "chainstate_snapshot_INVALID";
    BOOST_CHECK(!fs::exists(snapshot_invalid_dir));
    // chainstate_snapshot should still exist.
    BOOST_CHECK(fs::exists(snapshot_chainstate_dir));

    // Test that simulating a shutdown (reseting ChainstateManager) and then performing
    // chainstate reinitializing successfully cleans up the background-validation
    // chainstate data, and we end up with a single chainstate that is at tip.
    ChainstateManager& chainman_restarted = this->SimulateNodeRestart();

    BOOST_TEST_MESSAGE("Performing Load/Verify/Activate of chainstate");

    // This call reinitializes the chainstates, and should clean up the now unnecessary
    // background-validation leveldb contents.
    this->LoadVerifyActivateChainstate();

    BOOST_CHECK(!fs::exists(snapshot_invalid_dir));
    // chainstate_snapshot should now *not* exist.
    BOOST_CHECK(!fs::exists(snapshot_chainstate_dir));
    BOOST_CHECK(m_node.evodb->VerifyBestBlock(EvoDbIdentity::NORMAL, snapshot_tip_hash));
    uint256 obsolete_marker;
    BOOST_CHECK(!m_node.evodb->ReadBestBlock(EvoDbIdentity::SNAPSHOT, obsolete_marker));
    BOOST_CHECK(!m_node.evodb->ReadSnapshotBaseMNListHash(obsolete_marker));
    BOOST_CHECK(!m_node.evodb->HasDualChainstateMarker());
    // Promotion made the snapshot chainstate's post-base Evo state NORMAL.
    BOOST_CHECK(snapshot_records.ReadByNormal(m_node) == std::make_pair(true, true));
    BOOST_CHECK(snapshot_records.ListServedByNormal(m_node));

    const Chainstate& active_cs2 = chainman_restarted.ActiveChainstate();

    {
        LOCK(chainman_restarted.GetMutex());
        BOOST_CHECK_EQUAL(chainman_restarted.GetAll().size(), 1);
        BOOST_CHECK(!chainman_restarted.IsSnapshotActive());
        BOOST_CHECK(!chainman_restarted.IsSnapshotValidated());
        BOOST_CHECK(active_cs2.m_coinstip_cache_size_bytes > tip_cache_before_complete);
        BOOST_CHECK(active_cs2.m_coinsdb_cache_size_bytes > db_cache_before_complete);

        BOOST_CHECK_EQUAL(chainman_restarted.ActiveTip()->GetBlockHash(), snapshot_tip_hash);
        BOOST_CHECK_EQUAL(chainman_restarted.ActiveHeight(), 210);
    }

    BOOST_TEST_MESSAGE(
        "Ensure we can mine blocks on top of the \"new\" IBD chainstate");
    mineBlocks(10);
    {
        LOCK(chainman_restarted.GetMutex());
        BOOST_CHECK_EQUAL(chainman_restarted.ActiveHeight(), 220);
    }
}

//! What background validation derived reaches the snapshot chainstate only
//! when the next startup promotes it. Until then a block the snapshot
//! chainstate stalled on stays out of reach, and the node says so.
BOOST_FIXTURE_TEST_CASE(chainstatemanager_snapshot_completion_keeps_stall, SnapshotTestSetup)
{
    this->SetupSnapshot();
    ChainstateManager& chainman = *Assert(m_node.chainman);
    Chainstate& snapshot_cs{chainman.ActiveChainstate()};
    // Stay outside every regtest LLMQ mining window, so the block needs no
    // quorum commitment, only the pre-base MN list (DIP3 active at the base).
    while (WITH_LOCK(::cs_main, return (snapshot_cs.m_chain.Height() + 1) % 24) < 21) {
        mineBlocks(1);
    }
    const CBlockIndex* stall_parent{WITH_LOCK(::cs_main, return snapshot_cs.m_chain.Tip())};
    const CBlock stall_template{CreateBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()), snapshot_cs)};
    const auto stalled{std::make_shared<const CBlock>(MakeCbTxBlock(stall_template, stall_parent->nHeight + 1))};
    const auto stalls_on_it{[&] {
        ScopedDIP3FromGenesis dip3_active;
        BlockValidationState stall_state;
        chainman.ProcessNewBlock(stalled, /*force_processing=*/true, /*new_block=*/nullptr);
        return !snapshot_cs.ActivateBestChain(stall_state, stalled) && IsLocalEvoStateError(stall_state);
    }};
    BOOST_REQUIRE(stalls_on_it());
    const std::string warning_at_stall{GetWarnings(/*verbose=*/true).original};

    BOOST_REQUIRE_EQUAL(WITH_LOCK(::cs_main, return chainman.MaybeCompleteSnapshotValidation([](bilingual_str) {})),
                        SnapshotCompletionResult::SUCCESS);
    BOOST_CHECK(stalls_on_it());
    BOOST_CHECK(GetWarnings(/*verbose=*/true).original != warning_at_stall);
    BOOST_CHECK_EQUAL(WITH_LOCK(::cs_main, return snapshot_cs.m_chain.Tip()), stall_parent);
    SetMiscWarning(Untranslated(""));
}

BOOST_FIXTURE_TEST_CASE(chainstatemanager_snapshot_completion_incorrect_base_mn_list, SnapshotTestSetup)
{
    auto [validation_chainstate, snapshot_chainstate] = this->SetupSnapshot();
    ChainstateManager& chainman = *Assert(m_node.chainman);

    const CBlockIndex* base_index = WITH_LOCK(::cs_main, return validation_chainstate->m_chain.Tip());
    BOOST_REQUIRE(base_index);
    const CDeterministicMNList incorrect_base_list{
        base_index->GetBlockHash(), base_index->nHeight, /*totalRegisteredCount=*/1};
    const uint256 incorrect_hash{CanonicalDeterministicMNListHash(base_index, incorrect_base_list)};
    uint256 captured_block;
    uint256 captured_hash;
    BOOST_REQUIRE(m_node.evodb->ReadBackgroundMNListHash(captured_block, captured_hash));
    BOOST_CHECK_EQUAL(captured_block, base_index->GetBlockHash());
    BOOST_CHECK_NE(captured_hash, incorrect_hash);
    {
        EvoViewScope background_view{validation_chainstate->EvoView()};
        m_node.dmnman->SetListForBlockForTesting(incorrect_base_list);
    }

    // The regtest assumeutxo fixture predates DIP3. Temporarily make DIP3 active
    // so the legacy completion lookup would consume the poisoned shared cache.
    Consensus::Params& mutable_consensus{
        const_cast<Consensus::Params&>(Params().GetConsensus())};
    const int old_dip3_height{mutable_consensus.DIP0003Height};
    mutable_consensus.DIP0003Height = 1;
    // Boost's execution monitor longjmps past normal unwinding on fatal
    // failures, but for ordinary exceptions (BOOST_REQUIRE, deserialization
    // errors from the poisoned record below) this guard keeps the process-wide
    // consensus params from leaking into later test cases.
    struct Dip3HeightRestore {
        Consensus::Params& params;
        int height;
        ~Dip3HeightRestore() { params.DIP0003Height = height; }
    } dip3_restore{mutable_consensus, old_dip3_height};

    {
        LOCK(::cs_main);
        auto tx = m_node.evodb->BeginTransaction(EvoDbIdentity::SNAPSHOT);
        // Seed an incorrect full base list, not merely an incorrect comparison
        // hash. The key mirrors DB_LIST_SNAPSHOT in evo/deterministicmns.cpp
        // (file-local there); keep them in sync.
        m_node.evodb->Write(
            std::make_pair(std::string{"dmn_S3"}, base_index->GetBlockHash()),
            incorrect_base_list);
        m_node.evodb->WriteSnapshotBaseMNListHash(incorrect_hash);
        tx->Commit();
    }
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return m_node.evodb->CommitRootTransaction(EvoDbIdentity::SNAPSHOT, /*sync=*/true)));

    const auto result = WITH_LOCK(::cs_main,
        return chainman.MaybeCompleteSnapshotValidation([](bilingual_str) {}));
    BOOST_CHECK_EQUAL(result, SnapshotCompletionResult::EVO_STATE_MISMATCH);
    BOOST_CHECK_EQUAL(&chainman.ActiveChainstate(), validation_chainstate);
    BOOST_CHECK(snapshot_chainstate != &chainman.ActiveChainstate());
    BOOST_CHECK(!m_node.evodb->HasDualChainstateMarker());
    uint256 obsolete_marker;
    BOOST_CHECK(!m_node.evodb->ReadBestBlock(EvoDbIdentity::SNAPSHOT, obsolete_marker));
}

BOOST_FIXTURE_TEST_CASE(chainstatemanager_snapshot_cleanup_recovers_first_rename, SnapshotTestSetup)
{
    this->SetupSnapshot();
    ChainstateManager& chainman = *Assert(m_node.chainman);
    const uint256 snapshot_tip = WITH_LOCK(::cs_main, return chainman.ActiveChainstate().CoinsTip().GetBestBlock());
    BOOST_REQUIRE_EQUAL(WITH_LOCK(::cs_main,
        return chainman.MaybeCompleteSnapshotValidation([](bilingual_str) {})),
        SnapshotCompletionResult::SUCCESS);

    this->SimulateNodeRestart();
    const fs::path data_dir{gArgs.GetDataDirNet()};
    // ValidatedSnapshotCleanup journals the promotion before its first rename.
    BOOST_REQUIRE(m_node.evodb->PrepareSnapshotPromotion(snapshot_tip));
    fs::rename(data_dir / "chainstate", data_dir / "chainstate_todelete");

    this->LoadVerifyActivateChainstate();

    BOOST_CHECK(fs::exists(data_dir / "chainstate"));
    BOOST_CHECK(!fs::exists(data_dir / "chainstate_snapshot"));
    BOOST_CHECK(!fs::exists(data_dir / "chainstate_todelete"));
    BOOST_CHECK(m_node.evodb->VerifyBestBlock(EvoDbIdentity::NORMAL, snapshot_tip));
}

BOOST_FIXTURE_TEST_CASE(chainstatemanager_snapshot_cleanup_recovers_completed_swap, SnapshotTestSetup)
{
    this->SetupSnapshot();
    ChainstateManager& chainman = *Assert(m_node.chainman);
    const uint256 snapshot_tip = WITH_LOCK(::cs_main, return chainman.ActiveChainstate().CoinsTip().GetBestBlock());
    BOOST_REQUIRE_EQUAL(WITH_LOCK(::cs_main,
        return chainman.MaybeCompleteSnapshotValidation([](bilingual_str) {})),
        SnapshotCompletionResult::SUCCESS);

    this->SimulateNodeRestart();
    const fs::path data_dir{gArgs.GetDataDirNet()};
    // ValidatedSnapshotCleanup journals the promotion before its first rename.
    BOOST_REQUIRE(m_node.evodb->PrepareSnapshotPromotion(snapshot_tip));
    fs::rename(data_dir / "chainstate", data_dir / "chainstate_todelete");
    fs::rename(data_dir / "chainstate_snapshot", data_dir / "chainstate");

    this->LoadVerifyActivateChainstate();
    BOOST_CHECK(m_node.evodb->VerifyBestBlock(EvoDbIdentity::NORMAL, snapshot_tip));
    BOOST_CHECK(!m_node.evodb->HasDualChainstateMarker());

    // A restart of the already-promoted state is an idempotent no-op.
    this->SimulateNodeRestart();
    this->LoadVerifyActivateChainstate();
    BOOST_CHECK(m_node.evodb->VerifyBestBlock(EvoDbIdentity::NORMAL, snapshot_tip));
}

BOOST_FIXTURE_TEST_CASE(chainstatemanager_snapshot_cleanup_recovers_invalid_rename, SnapshotTestSetup)
{
    this->SetupSnapshot();
    ChainstateManager& restarted = this->SimulateNodeRestart();
    const fs::path data_dir{gArgs.GetDataDirNet()};
    fs::rename(data_dir / "chainstate_snapshot", data_dir / "chainstate_snapshot_INVALID");

    this->LoadVerifyActivateChainstate();

    BOOST_CHECK(fs::exists(data_dir / "chainstate_snapshot_INVALID"));
    BOOST_CHECK(!fs::exists(data_dir / "chainstate_snapshot"));
    BOOST_CHECK(!m_node.evodb->HasDualChainstateMarker());
    uint256 obsolete;
    BOOST_CHECK(!m_node.evodb->ReadBestBlock(EvoDbIdentity::SNAPSHOT, obsolete));
    BOOST_CHECK(m_node.evodb->VerifyBestBlock(
        EvoDbIdentity::NORMAL,
        WITH_LOCK(::cs_main, return restarted.ActiveChainstate().CoinsTip().GetBestBlock())));
}

//! At or below the base, the snapshot view holds only carried rotation
//! quarters. Reading an uncarried one as an input, to reconstruct or to
//! predict members, is unavailable data rather than fewer quarters or "not
//! available yet"; NORMAL keeps its legacy behavior.
BOOST_FIXTURE_TEST_CASE(chainstatemanager_uncarried_quarter_is_unavailable, SnapshotTestSetup)
{
    Chainstate* snapshot_chainstate{std::get<1>(this->SetupSnapshot())};
    ChainstateManager& chainman = *Assert(m_node.chainman);
    LOCK(::cs_main);
    const auto rotating{Consensus::LLMQType::LLMQ_TEST_DIP0024};
    const CBlockIndex* cycle_base{snapshot_chainstate->m_chain[96]};
    const CBlockIndex* next_work{snapshot_chainstate->m_chain[120 - llmq::WORK_DIFF_DEPTH]};
    const llmq::UtilParameters reconstruct{*m_node.dmnman, *m_node.llmq_ctx->qsnapman, chainman, cycle_base};
    const llmq::UtilParameters predict{*m_node.dmnman, *m_node.llmq_ctx->qsnapman, chainman, next_work};
    // Prediction needs V20's work-block modifier.
    Consensus::Params& consensus{const_cast<Consensus::Params&>(Params().GetConsensus())};
    struct V20HeightRestore {
        Consensus::Params& params;
        const int height;
        ~V20HeightRestore() { params.V20Height = height; }
    } v20_restore{consensus, std::exchange(consensus.V20Height, 1)};
    {
        EvoViewScope snapshot_view{m_node.evodb->ViewFor(EvoDbIdentity::SNAPSHOT)};
        BOOST_CHECK_THROW(llmq::utils::GetAllQuorumMembers(rotating, reconstruct), BlockDataUnavailableError);
        BOOST_CHECK_THROW(llmq::utils::ComputeQuorumMembersFromWorkBlock(rotating, predict, next_work, 120),
                          BlockDataUnavailableError);
    }
    {
        EvoViewScope background_view{m_node.evodb->ViewFor(EvoDbIdentity::NORMAL)};
        BOOST_CHECK_NO_THROW(llmq::utils::GetAllQuorumMembers(rotating, reconstruct));
        BOOST_CHECK(!llmq::utils::ComputeQuorumMembersFromWorkBlock(rotating, predict, next_work, 120));
    }
}

//! A rejection is persisted before the snapshot's coins directory is renamed. A
//! crash in between must leave startup able to finish the invalidation and run
//! on NORMAL alone, without a reindex.
BOOST_FIXTURE_TEST_CASE(chainstatemanager_snapshot_cleanup_recovers_rejection_before_rename, SnapshotTestSetup)
{
    this->SetupSnapshot();
    this->SimulateNodeRestart();
    BOOST_REQUIRE(m_node.evodb->DiscardSnapshotMarkers());
    const fs::path data_dir{gArgs.GetDataDirNet()};
    BOOST_REQUIRE(fs::exists(data_dir / "chainstate_snapshot"));

    BOOST_CHECK(std::get<0>(this->TryLoadChainstate()) == node::ChainstateLoadStatus::SUCCESS);
    BOOST_CHECK(fs::exists(data_dir / "chainstate_snapshot_INVALID"));
    BOOST_CHECK(!fs::exists(data_dir / "chainstate_snapshot"));
    BOOST_CHECK(!m_node.evodb->HasRejectedGeneration());
    BOOST_CHECK(WITH_LOCK(::cs_main, return !m_node.chainman->IsSnapshotActive()));
}

//! A rejected generation left behind by a failed collection must not take the
//! coins directory of a later, live snapshot with it at the next startup.
BOOST_FIXTURE_TEST_CASE(chainstatemanager_snapshot_cleanup_keeps_live_snapshot_dir, SnapshotTestSetup)
{
    this->SetupSnapshot();
    const uint256 snapshot_tip{WITH_LOCK(::cs_main, return m_node.chainman->ActiveTip()->GetBlockHash())};
    {
        // The record of an earlier snapshot whose collection failed.
        LOCK(::cs_main);
        CEvoDB& evodb{*m_node.evodb};
        EvoGenerationRecord rejected;
        rejected.generation = 1000;
        rejected.state = EvoGenerationState::REJECTED;
        EvoViewScope normal_view{evodb.ViewFor(EvoDbIdentity::NORMAL)};
        BOOST_REQUIRE(evodb.WriteDurable(std::make_pair(EVODB_GENERATION_RECORD, rejected.generation), rejected));
    }
    this->SimulateNodeRestart();

    // Startup drops the stale record, and the live snapshot carries on (here
    // it completes and is promoted) instead of being invalidated.
    BOOST_CHECK(std::get<0>(this->TryLoadChainstate()) == node::ChainstateLoadStatus::SUCCESS);
    BOOST_CHECK(!fs::exists(gArgs.GetDataDirNet() / "chainstate_snapshot_INVALID"));
    BOOST_CHECK(!m_node.evodb->HasRejectedGeneration());
    BOOST_CHECK_EQUAL(WITH_LOCK(::cs_main, return m_node.chainman->ActiveTip()->GetBlockHash()), snapshot_tip);
}

//! A crash between an invalid-snapshot rename and its marker discard while the
//! SNAPSHOT marker equals the background coins tip (completion failed with the
//! snapshot chainstate still at its base) looks like a completed coins swap.
//! Without a promotion journal, recovery must discard the snapshot generation
//! rather than merge it into NORMAL state.
BOOST_FIXTURE_TEST_CASE(chainstatemanager_snapshot_cleanup_overlap_promote_equals_discard, SnapshotTestSetup)
{
    Chainstate* background_chainstate = std::get<0>(this->SetupSnapshot());
    const uint256 bg_tip = WITH_LOCK(::cs_main, return background_chainstate->CoinsTip().GetBestBlock());

    ChainstateManager& restarted = this->SimulateNodeRestart();
    const fs::path data_dir{gArgs.GetDataDirNet()};
    // Rewind the on-disk SNAPSHOT marker to the base block, as if the snapshot
    // chainstate had never advanced past its base when completion failed, then
    // simulate the crash after InvalidateCoinsDBOnDisk's rename.
    SeedSnapshotMarker(*m_node.evodb, bg_tip);
    fs::rename(data_dir / "chainstate_snapshot", data_dir / "chainstate_snapshot_INVALID");

    this->LoadVerifyActivateChainstate();

    BOOST_CHECK(fs::exists(data_dir / "chainstate_snapshot_INVALID"));
    BOOST_CHECK(!fs::exists(data_dir / "chainstate_snapshot"));
    BOOST_CHECK(!m_node.evodb->HasDualChainstateMarker());
    uint256 obsolete;
    BOOST_CHECK(!m_node.evodb->ReadBestBlock(EvoDbIdentity::SNAPSHOT, obsolete));
    BOOST_CHECK(!m_node.evodb->ReadSnapshotBaseMNListHash(obsolete));
    {
        LOCK(::cs_main);
        BOOST_CHECK_EQUAL(restarted.GetAll().size(), 1);
        BOOST_CHECK(!restarted.IsSnapshotActive());
        BOOST_CHECK(m_node.evodb->VerifyBestBlock(
            EvoDbIdentity::NORMAL, restarted.ActiveChainstate().CoinsTip().GetBestBlock()));
    }
}

BOOST_FIXTURE_TEST_CASE(chainstatemanager_snapshot_cleanup_recovers_promoted_swap, SnapshotTestSetup)
{
    this->SetupSnapshot();
    ChainstateManager& chainman = *Assert(m_node.chainman);
    const uint256 snapshot_tip = WITH_LOCK(::cs_main, return chainman.ActiveChainstate().CoinsTip().GetBestBlock());
    BOOST_REQUIRE_EQUAL(WITH_LOCK(::cs_main,
        return chainman.MaybeCompleteSnapshotValidation([](bilingual_str) {})),
        SnapshotCompletionResult::SUCCESS);

    this->SimulateNodeRestart();
    const fs::path data_dir{gArgs.GetDataDirNet()};
    // ValidatedSnapshotCleanup journals the promotion before its first rename.
    BOOST_REQUIRE(m_node.evodb->PrepareSnapshotPromotion(snapshot_tip));
    fs::rename(data_dir / "chainstate", data_dir / "chainstate_todelete");
    fs::rename(data_dir / "chainstate_snapshot", data_dir / "chainstate");
    BOOST_REQUIRE(m_node.evodb->PromoteSnapshotMarkers(snapshot_tip));

    this->LoadVerifyActivateChainstate();
    BOOST_CHECK(!fs::exists(data_dir / "chainstate_todelete"));
    BOOST_CHECK(m_node.evodb->VerifyBestBlock(EvoDbIdentity::NORMAL, snapshot_tip));
}

//! Stop promotion at `failpoint` as if the node crashed there; the next startup
//! must finish it from the journal without losing any snapshot-written Evo
//! state.
static void CheckPromotionCrashRecovery(SnapshotTestSetup& setup, CEvoDB::PromotionFailpoint failpoint)
{
    node::NodeContext& node{setup.m_node};
    setup.SetupSnapshot();
    ChainstateManager& chainman = *Assert(node.chainman);
    const uint256 snapshot_tip = WITH_LOCK(::cs_main, return chainman.ActiveTip()->GetBlockHash());
    const SnapshotViewRecords snapshot_records{
        WITH_LOCK(::cs_main, return SnapshotViewRecords::Write(*node.evodb, chainman.ActiveChainstate().m_chain[150]))};
    BOOST_REQUIRE_EQUAL(WITH_LOCK(::cs_main, return chainman.MaybeCompleteSnapshotValidation([](bilingual_str) {})),
                        SnapshotCompletionResult::SUCCESS);

    node.evodb->SetMergeBatchSizeForTesting(1);
    node.evodb->SetPromotionFailpointForTesting(failpoint);
    setup.SimulateNodeRestart();
    BOOST_CHECK(std::get<0>(setup.TryLoadChainstate()) == node::ChainstateLoadStatus::FAILURE_FATAL);
    node.evodb->SetPromotionFailpointForTesting(CEvoDB::PromotionFailpoint::NONE);

    const auto journal{node.evodb->ReadPromotionJournal()};
    BOOST_REQUIRE(journal);
    BOOST_CHECK_EQUAL(journal->tip, snapshot_tip);
    if (failpoint == CEvoDB::PromotionFailpoint::MID_MERGE) {
        BOOST_CHECK(journal->phase == EvoPromotionPhase::MERGING);
        BOOST_CHECK(!journal->cursor.empty());
    } else {
        BOOST_CHECK(journal->phase == EvoPromotionPhase::MERGED);
    }
    BOOST_CHECK(node.evodb->HasDualChainstateMarker());

    if (failpoint == CEvoDB::PromotionFailpoint::AFTER_COLLECT) {
        // The unfinished promotion is still bound to the configuration it was
        // published under, even with its namespace already collected.
        Consensus::Params& consensus{const_cast<Consensus::Params&>(Params().GetConsensus())};
        const int dip1_height{consensus.DIP0001Height};
        setup.SimulateNodeRestart();
        ++consensus.DIP0001Height;
        const auto [status, error] = setup.TryLoadChainstate();
        consensus.DIP0001Height = dip1_height;
        BOOST_CHECK(status == node::ChainstateLoadStatus::FAILURE);
        BOOST_CHECK(error.original.find("consensus configuration or its fingerprint version changed") != std::string::npos);
        BOOST_CHECK(node.evodb->ReadPromotionJournal());
    }

    setup.SimulateNodeRestart();
    setup.LoadVerifyActivateChainstate();
    const fs::path data_dir{gArgs.GetDataDirNet()};
    BOOST_CHECK(!fs::exists(data_dir / "chainstate_snapshot"));
    BOOST_CHECK(!fs::exists(data_dir / "chainstate_todelete"));
    BOOST_CHECK(node.evodb->VerifyBestBlock(EvoDbIdentity::NORMAL, snapshot_tip));
    BOOST_CHECK(!node.evodb->HasDualChainstateMarker());
    BOOST_CHECK(!node.evodb->ReadPromotionJournal());
    BOOST_CHECK(!node.evodb->GetSnapshotGeneration());
    BOOST_CHECK(snapshot_records.ReadByNormal(node) == std::make_pair(true, true));
    BOOST_CHECK(snapshot_records.ListServedByNormal(node));
}

BOOST_FIXTURE_TEST_CASE(chainstatemanager_snapshot_promotion_crash_mid_merge, SnapshotTestSetup)
{
    CheckPromotionCrashRecovery(*this, CEvoDB::PromotionFailpoint::MID_MERGE);
}

BOOST_FIXTURE_TEST_CASE(chainstatemanager_snapshot_promotion_crash_after_merge, SnapshotTestSetup)
{
    CheckPromotionCrashRecovery(*this, CEvoDB::PromotionFailpoint::AFTER_MERGE);
}

BOOST_FIXTURE_TEST_CASE(chainstatemanager_snapshot_promotion_crash_after_collect, SnapshotTestSetup)
{
    CheckPromotionCrashRecovery(*this, CEvoDB::PromotionFailpoint::AFTER_COLLECT);
}

//! A journaled promotion that crashed before its first coins rename is redone
//! by the next startup.
BOOST_FIXTURE_TEST_CASE(chainstatemanager_snapshot_cleanup_recovers_prepared_promotion, SnapshotTestSetup)
{
    this->SetupSnapshot();
    ChainstateManager& chainman = *Assert(m_node.chainman);
    const uint256 snapshot_tip = WITH_LOCK(::cs_main, return chainman.ActiveTip()->GetBlockHash());
    BOOST_REQUIRE_EQUAL(WITH_LOCK(::cs_main, return chainman.MaybeCompleteSnapshotValidation([](bilingual_str) {})),
                        SnapshotCompletionResult::SUCCESS);

    this->SimulateNodeRestart();
    BOOST_REQUIRE(m_node.evodb->PrepareSnapshotPromotion(snapshot_tip));
    this->LoadVerifyActivateChainstate();

    BOOST_CHECK(!fs::exists(gArgs.GetDataDirNet() / "chainstate_snapshot"));
    BOOST_CHECK(m_node.evodb->VerifyBestBlock(EvoDbIdentity::NORMAL, snapshot_tip));
    BOOST_CHECK(!m_node.evodb->HasDualChainstateMarker());
    BOOST_CHECK(!m_node.evodb->ReadPromotionJournal());
}

//! A published snapshot generation is valid only for the consensus
//! configuration it was loaded under. Changing any covered field, including
//! ones an earlier fingerprint omitted, must make startup refuse it before
//! rebinding it.
BOOST_FIXTURE_TEST_CASE(chainstatemanager_snapshot_refused_after_consensus_change, SnapshotTestSetup)
{
    this->SetupSnapshot();
    const uint256 snapshot_tip{WITH_LOCK(::cs_main, return m_node.chainman->ActiveTip()->GetBlockHash())};
    Consensus::Params& consensus{const_cast<Consensus::Params&>(Params().GetConsensus())};
    const auto llmq_test{std::ranges::find(consensus.llmqs, Consensus::LLMQType::LLMQ_TEST, &Consensus::LLMQParams::type)};
    BOOST_REQUIRE(llmq_test != consensus.llmqs.end());

    const auto refused_under = [&](auto& field) {
        this->SimulateNodeRestart();
        const auto original{field};
        if constexpr (std::is_same_v<std::decay_t<decltype(field)>, uint256>) {
            field = ArithToUint256(UintToArith256(field) + 1);
        } else {
            ++field;
        }
        const auto [status, error] = this->TryLoadChainstate();
        field = original;
        BOOST_CHECK(status == node::ChainstateLoadStatus::FAILURE);
        BOOST_CHECK(error.original.find("consensus configuration or its fingerprint version changed") != std::string::npos);
        BOOST_CHECK(!m_node.evodb->GetSnapshotGeneration());
        BOOST_CHECK(WITH_LOCK(::cs_main, return !m_node.chainman->IsSnapshotActive()));
        // Refusing is not discarding: the snapshot survives for a restart under
        // the original configuration.
        BOOST_CHECK(m_node.evodb->HasDualChainstateMarker());
        BOOST_CHECK(fs::exists(gArgs.GetDataDirNet() / "chainstate_snapshot"));
    };
    refused_under(consensus.vDeployments[Consensus::DEPLOYMENT_V24].min_activation_height);
    refused_under(consensus.DIP0001Height);
    refused_under(llmq_test->size);
    refused_under(consensus.DIP0003EnforcementHash);
    refused_under(consensus.nMasternodeMinimumConfirmations);
    refused_under(consensus.nSubsidyHalvingInterval);
    refused_under(consensus.nBudgetPaymentsStartBlock);
    refused_under(consensus.nHighSubsidyBlocks);
    refused_under(consensus.nHighSubsidyFactor);
    refused_under(consensus.nSuperblockCycle);
    refused_under(consensus.nMasternodePaymentsIncreaseBlock);
    refused_under(consensus.nMasternodePaymentsIncreasePeriod);

    // Under the original configuration the generation is accepted again; with
    // the background chainstate at the base, startup completes and promotes it.
    this->SimulateNodeRestart();
    this->LoadVerifyActivateChainstate();
    BOOST_CHECK(m_node.evodb->VerifyBestBlock(EvoDbIdentity::NORMAL, snapshot_tip));
    BOOST_CHECK(!m_node.evodb->HasDualChainstateMarker());
}

BOOST_FIXTURE_TEST_CASE(chainstatemanager_snapshot_base_is_cached, SnapshotTestSetup)
{
    this->SetupSnapshot();
    ChainstateManager& chainman = *Assert(m_node.chainman);
    const uint256 base_hash{*chainman.SnapshotBlockhash()};

    {
        LOCK(::cs_main);
        const CBlockIndex* cached_base = chainman.ActiveChainstate().SnapshotBase();
        BOOST_REQUIRE(cached_base);
        auto node = chainman.BlockIndex().extract(base_hash);
        BOOST_REQUIRE(!node.empty());
        BOOST_CHECK_EQUAL(chainman.ActiveChainstate().SnapshotBase(), cached_base);
        chainman.BlockIndex().insert(std::move(node));
    }
}

//! A snapshot chainstate whose base block is missing from the on-disk block
//! index must fail startup with a recoverable error (reindex discards the
//! snapshot), not abort in candidate admission.
BOOST_FIXTURE_TEST_CASE(chainstatemanager_snapshot_missing_base_fails_load, SnapshotTestSetup)
{
    this->SetupSnapshot();
    this->SimulateNodeRestart();

    // Simulate a block-tree database that has lost the snapshot's history
    // (wiped or swapped blocks/index) while the snapshot chainstate directory
    // and its EvoDB marker survive.
    fs::remove_all(gArgs.GetDataDirNet() / "blocks" / "index");

    ChainstateManager& chainman = *Assert(m_node.chainman);
    MakeDashChainContexts(/*llmq_dbs_wipe=*/false);
    node::ChainstateLoadStatus status;
    bilingual_str error;
    {
        ASSERT_DEBUG_LOG("missing from the block index");
        std::tie(status, error) = node::LoadChainstate(chainman, m_cache_sizes, ChainstateLoadOptionsForTest(),
                                                       *m_node.evodb, *m_node.dmnman,
                                                       *m_node.special_tx);
    }
    BOOST_CHECK(status == node::ChainstateLoadStatus::FAILURE);
}

BOOST_FIXTURE_TEST_CASE(chainstatemanager_snapshot_completion_without_base_list_marker, SnapshotTestSetup)
{
    this->SetupSnapshot();
    ChainstateManager& chainman = *Assert(m_node.chainman);

    // Simulate a cold-start activation, where the base MN list was not
    // derivable and so no lifecycle hash markers were captured: completion
    // must fall back to the UTXO-set hash alone instead of quarantining a
    // valid snapshot.
    BOOST_REQUIRE(m_node.evodb->GetRawDB().Erase(EVODB_SNAPSHOT_MNLIST_HASH, /*fSync=*/true));
    BOOST_REQUIRE(m_node.evodb->GetRawDB().Erase(EVODB_BACKGROUND_MNLIST_HASH, /*fSync=*/true));

    const auto res = WITH_LOCK(::cs_main,
        return chainman.MaybeCompleteSnapshotValidation([](bilingual_str) {}));
    BOOST_CHECK_EQUAL(res, SnapshotCompletionResult::SUCCESS);
    WITH_LOCK(::cs_main, BOOST_CHECK(chainman.IsSnapshotValidated()));
}

BOOST_FIXTURE_TEST_CASE(chainstatemanager_snapshot_completion_hash_mismatch, SnapshotTestSetup)
{
    auto chainstates = this->SetupSnapshot();
    Chainstate& validation_chainstate = *std::get<0>(chainstates);
    ChainstateManager& chainman = *Assert(m_node.chainman);
    SnapshotCompletionResult res;
    auto mock_shutdown = [](bilingual_str msg) {};

    // Test tampering with the IBD UTXO set with an extra coin to ensure it causes
    // snapshot completion to fail.
    CCoinsViewCache& ibd_coins = WITH_LOCK(::cs_main,
        return validation_chainstate.CoinsTip());
    Coin badcoin;
    badcoin.out.nValue = InsecureRand32();
    badcoin.nHeight = 1;
    badcoin.out.scriptPubKey.assign(InsecureRandBits(6), 0);
    uint256 txid = InsecureRand256();
    ibd_coins.AddCoin(COutPoint(txid, 0), std::move(badcoin), false);

    fs::path snapshot_chainstate_dir = gArgs.GetDataDirNet() / "chainstate_snapshot";
    BOOST_CHECK(fs::exists(snapshot_chainstate_dir));

    // Evo state the snapshot chainstate wrote above its base.
    const SnapshotViewRecords snapshot_records{
        WITH_LOCK(::cs_main, return SnapshotViewRecords::Write(*m_node.evodb, chainman.ActiveChainstate().m_chain[150]))};

    {
        ASSERT_DEBUG_LOG("failed to validate the -assumeutxo snapshot state");
        res = WITH_LOCK(::cs_main,
            return chainman.MaybeCompleteSnapshotValidation(mock_shutdown));
        BOOST_CHECK_EQUAL(res, SnapshotCompletionResult::HASH_MISMATCH);
    }

    auto all_chainstates = chainman.GetAll();
    BOOST_CHECK_EQUAL(all_chainstates.size(), 1);
    BOOST_CHECK_EQUAL(all_chainstates[0], &validation_chainstate);
    BOOST_CHECK_EQUAL(&chainman.ActiveChainstate(), &validation_chainstate);

    fs::path snapshot_invalid_dir = gArgs.GetDataDirNet() / "chainstate_snapshot_INVALID";
    BOOST_CHECK(fs::exists(snapshot_invalid_dir));

    // Test that simulating a shutdown (reseting ChainstateManager) and then performing
    // chainstate reinitializing successfully loads only the fully-validated
    // chainstate data, and we end up with a single chainstate that is at tip.
    ChainstateManager& chainman_restarted = this->SimulateNodeRestart();

    BOOST_TEST_MESSAGE("Performing Load/Verify/Activate of chainstate");

    // This call reinitializes the chainstates, and should clean up the now unnecessary
    // background-validation leveldb contents.
    this->LoadVerifyActivateChainstate();

    BOOST_CHECK(fs::exists(snapshot_invalid_dir));
    BOOST_CHECK(!fs::exists(snapshot_chainstate_dir));

    {
        LOCK(::cs_main);
        BOOST_CHECK_EQUAL(chainman_restarted.GetAll().size(), 1);
        BOOST_CHECK(!chainman_restarted.IsSnapshotActive());
        BOOST_CHECK(!chainman_restarted.IsSnapshotValidated());
        BOOST_CHECK_EQUAL(chainman_restarted.ActiveHeight(), 210);
    }

    // NORMAL advanced past the base over the same blocks. It must build its
    // own Evo state there, never consuming what the rejected snapshot wrote.
    BOOST_CHECK(snapshot_records.ReadByNormal(m_node) == std::make_pair(false, false));
    BOOST_CHECK(!snapshot_records.ListServedByNormal(m_node));
    BOOST_CHECK(!m_node.evodb->GetSnapshotGeneration());

    BOOST_TEST_MESSAGE(
        "Ensure we can mine blocks on top of the \"new\" IBD chainstate");
    mineBlocks(10);
    {
        LOCK(::cs_main);
        BOOST_CHECK_EQUAL(chainman_restarted.ActiveHeight(), 220);
    }
}

//! The fixture's evo pin is the SHA256 of the section's wire bytes as an
//! assembler that writes the format table field by field produces them, and
//! the section holds what the fixture's construction implies.
BOOST_FIXTURE_TEST_CASE(chainstatemanager_evo_snapshot_fixture_pin, EvoSnapshotTestSetup)
{
    std::optional<evo::EvoSnapshot> section;
    (void)DumpEvoSnapshot([&](auto& evo_section) { section = evo_section; });
    BOOST_REQUIRE(section);
    const auto bytes{AssembleWireBytes(*section, evo::EVO_SNAPSHOT_VERSION)};
    uint256 hash;
    CSHA256().Write(bytes.data(), bytes.size()).Finalize(hash.begin());
    BOOST_CHECK(EvoSnapshotHash{hash} == ExpectedAssumeutxo(BASE_HEIGHT, Params())->evo_hash);

    std::set<uint256> registered;
    for (const auto& [protx_hash, _] : m_operator_keys)
        registered.insert(protx_hash);
    std::set<uint256> carried;
    section->mn_list.ForEachMN(/*onlyValid=*/false, [&](const auto& dmn) { carried.insert(dmn.proTxHash); });
    BOOST_CHECK(carried == registered);
    BOOST_CHECK_EQUAL(carried.size(), size_t{MN_COUNT});
    std::set<int> cycles;
    for (const auto& entry :
         EvoSnapshotChainSetupType(*section, Consensus::LLMQType::LLMQ_TEST_DIP0024).rotation_snapshots) {
        cycles.insert(WITH_LOCK(::cs_main,
                                return m_node.chainman->m_blockman.LookupBlockIndex(entry.cycle_base_block_hash)->nHeight));
    }
    BOOST_CHECK(cycles == (std::set<int>{144, 168, 192, 216}));
    const int window{Params().CreditPoolPeriodBlocks()};
    BOOST_REQUIRE_EQUAL(section->credit_history.size(), size_t(window));
    for (int i{0}; i < window; ++i) {
        if (BASE_HEIGHT - window + 1 + i < DIP3_HEIGHT) BOOST_CHECK(section->credit_history[i] == CreditHistoryEntry{});
    }
    for (const auto& data : section->quorums) {
        BOOST_CHECK(data.active_commitments.empty() && data.safety_commitments.empty());
    }
}

//! A DIP3-active base needs its evo section and a reviewed, non-null pin that
//! the section matches. Anything else is refused before any Evo state is
//! written, so no generation is created.
BOOST_FIXTURE_TEST_CASE(chainstatemanager_evo_snapshot_requires_its_pin, EvoSnapshotTestSetup)
{
    const auto refused_before_seeding = [&](const std::function<void(std::optional<evo::EvoSnapshot>&)>& mutate) {
        const auto next_generation{NextGeneration()};
        BOOST_CHECK(!ActivateEvoSnapshot(mutate));
        BOOST_CHECK(NextGeneration() == next_generation);
        BOOST_CHECK(!m_node.chainman->IsSnapshotActive());
    };
    {
        const AssumeutxoData pinned{*ExpectedAssumeutxo(BASE_HEIGHT, Params())};
        const ScopedAssumeutxoEntry null_pin{BASE_HEIGHT,
                                             AssumeutxoData{pinned.hash_serialized, EvoSnapshotHash{uint256::ZERO},
                                                            pinned.nChainTx, pinned.blockhash}};
        refused_before_seeding(nullptr);
    }
    refused_before_seeding([](auto& evo_section) { evo_section.reset(); });
    // Substitutions only the pin can catch: every later gate accepts them.
    refused_before_seeding([](auto& evo_section) { evo_section->credit_pool.currentLimit -= 1; });
    refused_before_seeding([](auto& evo_section) { evo_section->mnhf_signals.emplace(27, BASE_HEIGHT - 1); });
    // Bytes after the section, or a partial marker after the coins.
    for (const bool keep_section : {true, false}) {
        const fs::path path{DumpEvoSnapshot([keep_section](auto& evo_section) {
            if (!keep_section) evo_section.reset();
        })};
        {
            AutoFile out{fsbridge::fopen(path, "ab")};
            out << uint8_t{0};
        }
        const auto next_generation{NextGeneration()};
        BOOST_CHECK(!LoadEvoSnapshot(path));
        BOOST_CHECK(NextGeneration() == next_generation);
    }

    BOOST_CHECK(ActivateEvoSnapshot());
    BOOST_CHECK(m_node.chainman->IsSnapshotActive());
}

//! A node that has only the headers loads the snapshot, and then connects the
//! blocks a fully synced node mined above the base. Their coinbases commit to
//! the MN list, quorums and credit pool that node derived, so each block only
//! connects if the seeded state is exact: the B+1 ChainLock check without B's
//! body, the credit-pool window up to B+P+2, where it switches to the bodies of
//! post-base blocks (across a restart), and the next cycle, whose quorum is
//! computed from carried quarters, including the base cycle's own.
BOOST_FIXTURE_TEST_CASE(chainstatemanager_evo_snapshot_cold_node, EvoSnapshotTestSetup)
{
    constexpr auto ROTATING{Consensus::LLMQType::LLMQ_TEST_DIP0024};
    constexpr int CYCLE_LENGTH{24};
    constexpr int NEXT_CYCLE{BASE_HEIGHT - BASE_HEIGHT % CYCLE_LENGTH + CYCLE_LENGTH};
    const int window{Params().CreditPoolPeriodBlocks()};
    const int tip_height{BASE_HEIGHT + window + 2};
    // The base is before its cycle's mining window, so the next cycle's work
    // block is not mined yet.
    BOOST_REQUIRE_GT(NEXT_CYCLE - llmq::WORK_DIFF_DEPTH, BASE_HEIGHT);

    const fs::path snapshot{DumpEvoSnapshot()};
    MineTo(tip_height);
    const auto expected_base_cycle{Members(m_node.chainman->ActiveChainstate(), ROTATING, NEXT_CYCLE - CYCLE_LENGTH)};
    const auto expected_next_cycle{Members(m_node.chainman->ActiveChainstate(), ROTATING, NEXT_CYCLE)};
    ForgetBlocks();
    BOOST_REQUIRE(!expected_next_cycle.empty());
    BOOST_REQUIRE(!m_node.evodb->GetSnapshotGeneration());

    BOOST_REQUIRE(LoadEvoSnapshot(snapshot));
    Chainstate* active{&m_node.chainman->ActiveChainstate()};
    BOOST_CHECK(Members(*active, ROTATING, NEXT_CYCLE - CYCLE_LENGTH) == expected_base_cycle);

    const int restart_height{BASE_HEIGHT + window / 2};
    BOOST_CHECK(Feed(*active, BASE_HEIGHT + 1, restart_height).IsValid());
    BOOST_REQUIRE_EQUAL(Height(*active), restart_height);
    Restart();
    active = &m_node.chainman->ActiveChainstate();
    BOOST_REQUIRE(active->m_from_snapshot_blockhash);
    BOOST_CHECK(Feed(*active, restart_height + 1, tip_height).IsValid());
    BOOST_CHECK_EQUAL(Height(*active), tip_height);
    BOOST_CHECK(Members(*active, ROTATING, NEXT_CYCLE) == expected_next_cycle);
}

//! At the block before the next cycle, that cycle's work block is mined but no
//! carried quorum or quarter references it: the section bootstraps it, and the
//! dry run computes its members before its own block exists.
BOOST_FIXTURE_TEST_CASE(chainstatemanager_evo_snapshot_bootstraps_the_next_cycle, EvoSnapshotTestSetup)
{
    constexpr auto ROTATING{Consensus::LLMQType::LLMQ_TEST_DIP0024};
    constexpr int NEXT_CYCLE{240};
    constexpr int base_height{NEXT_CYCLE - 1};
    MineTo(base_height);
    // Pin this base for this deployment configuration: the evo hash from the
    // NORMAL state of this node, which never loaded a snapshot.
    std::optional<ScopedAssumeutxoEntry> pin;
    {
        LOCK(::cs_main);
        Chainstate& chainstate{m_node.chainman->ActiveChainstate()};
        const CBlockIndex* base{chainstate.m_chain.Tip()};
        evo::EvoSnapshot section;
        std::string error;
        {
            const EvoViewScope normal{m_node.evodb->ViewFor(EvoDbIdentity::NORMAL)};
            BOOST_REQUIRE_MESSAGE(evo::BuildEvoSnapshot(Params(), *m_node.chainman, *m_node.dmnman,
                                                        *m_node.llmq_ctx->quorum_block_processor,
                                                        *m_node.llmq_ctx->qsnapman, *m_node.special_tx->m_cpoolman,
                                                        *m_node.special_tx->m_mnhfman, base, section, error),
                                  error);
        }
        BOOST_REQUIRE_EQUAL(EvoSnapshotChainSetupType(section, ROTATING).bootstrap_work.size(), 1U);
        chainstate.ForceFlushStateToDisk();
        const auto stats{*kernel::ComputeUTXOStats(kernel::CoinStatsHashType::HASH_SERIALIZED, &chainstate.CoinsDB(),
                                                   m_node.chainman->m_blockman)};
        pin.emplace(base_height, AssumeutxoData{AssumeutxoHash{stats.hashSerialized},
                                                EvoSnapshotHash{evo::GetEvoSnapshotHash(section)}, base->nChainTx,
                                                base->GetBlockHash()});
    }
    const fs::path snapshot{DumpEvoSnapshot()};
    MineTo(NEXT_CYCLE + 1);
    const auto expected{Members(m_node.chainman->ActiveChainstate(), ROTATING, NEXT_CYCLE)};
    ForgetBlocks();
    BOOST_REQUIRE(!expected.empty());

    BOOST_REQUIRE(LoadEvoSnapshot(snapshot));
    Chainstate& active{m_node.chainman->ActiveChainstate()};
    {
        LOCK(::cs_main);
        const EvoViewScope view{active.EvoView()};
        const CBlockIndex* work{active.m_chain[NEXT_CYCLE - llmq::WORK_DIFF_DEPTH]};
        const auto predicted{llmq::utils::ComputeQuorumMembersFromWorkBlock(
            ROTATING, {*m_node.dmnman, *m_node.llmq_ctx->qsnapman, *m_node.chainman, work}, work, NEXT_CYCLE)};
        BOOST_REQUIRE(predicted);
        std::vector<uint256> hashes;
        for (const auto& member : *predicted)
            hashes.push_back(member->proTxHash);
        BOOST_CHECK(hashes == expected);
    }
    BOOST_CHECK(Feed(active, base_height + 1, NEXT_CYCLE + 1).IsValid());
    BOOST_CHECK(Members(active, ROTATING, NEXT_CYCLE) == expected);
}

//! A crash during activation leaves a generation that never activated:
//! unpublished if it came before publication, published but without the
//! snapshot directory's base block if it came after. Startup discards either,
//! together with every evo input captured for it; NORMAL is untouched.
static void CrashDuringActivation(EvoSnapshotTestSetup& setup, bool after_publication)
{
    node::NodeContext& node{setup.m_node};
    const uint256 base{WITH_LOCK(::cs_main, return node.chainman->ActiveTip()->GetBlockHash())};
    (after_publication ? node.chainman->m_stop_after_evo_publication_for_testing
                       : node.chainman->m_stop_before_evo_publication_for_testing) = true;
    BOOST_REQUIRE(!setup.ActivateEvoSnapshot());
    const uint32_t generation{*setup.NextGeneration() - 1};
    const auto count_keys = [&](const DataStream& prefix) {
        CDBWrapper& db{node.evodb->GetRawDB()};
        std::unique_ptr<CDBIterator> it{db.NewIterator()};
        size_t count{0};
        for (it->Seek(prefix); it->Valid(); it->Next()) {
            const DataStream key{it->GetKey()};
            if (key.size() < prefix.size() || !std::equal(prefix.begin(), prefix.end(), key.begin())) break;
            ++count;
        }
        return count;
    };
    const auto seeded_keys = [&] { return count_keys(DataStream{CEvoDBStore::GenerationPrefix(generation)}); };
    // NORMAL records written for the activation: the captured evo inputs. The
    // key prefix mirrors the captures in evo/snapshot_chain.cpp (file-local
    // there).
    const auto captures = [&] {
        DataStream prefix;
        prefix << std::string{"b_evo_cap"};
        return count_keys(prefix);
    };
    BOOST_CHECK_GT(seeded_keys(), 0U);
    BOOST_CHECK_GT(captures(), 0U);

    setup.Restart();
    BOOST_CHECK(!node.evodb->GetSnapshotGeneration());
    // A published generation is marked rejected instead; its records go with
    // the snapshot's coins directory.
    if (!after_publication) BOOST_CHECK_EQUAL(seeded_keys(), 0U);
    BOOST_CHECK_EQUAL(captures(), 0U);
    BOOST_CHECK(!node.chainman->IsSnapshotActive());
    BOOST_CHECK_EQUAL(WITH_LOCK(::cs_main, return node.chainman->ActiveTip()->GetBlockHash()), base);
    BOOST_CHECK(node.evodb->VerifyBestBlock(EvoDbIdentity::NORMAL, base));
    setup.mineBlocks(1);
    BOOST_CHECK_EQUAL(setup.Height(node.chainman->ActiveChainstate()), EvoSnapshotTestSetup::BASE_HEIGHT + 1);
}

BOOST_FIXTURE_TEST_CASE(chainstatemanager_evo_snapshot_crash_before_publication, EvoSnapshotTestSetup)
{
    CrashDuringActivation(*this, /*after_publication=*/false);
}

BOOST_FIXTURE_TEST_CASE(chainstatemanager_evo_snapshot_crash_after_publication, EvoSnapshotTestSetup)
{
    CrashDuringActivation(*this, /*after_publication=*/true);
}

//! A seeded record the snapshot chainstate needs but no longer has is a local
//! failure: the chainstate stalls, and the block is not marked invalid.
BOOST_FIXTURE_TEST_CASE(chainstatemanager_evo_snapshot_missing_seed_stalls, EvoSnapshotTestSetup)
{
    // The pool of the block whose window drops the first carried entry, the
    // one at DIP3 (earlier ones are known to be empty), is the starting pool
    // of the block after it.
    const int stall_height{DIP3_HEIGHT + Params().CreditPoolPeriodBlocks() + 1};
    BOOST_REQUIRE_GT(stall_height, BASE_HEIGHT + 1);
    const fs::path snapshot{DumpEvoSnapshot()};
    MineTo(stall_height);
    ForgetBlocks();
    BOOST_REQUIRE(LoadEvoSnapshot(snapshot));
    Chainstate& active{m_node.chainman->ActiveChainstate()};
    BOOST_REQUIRE(Feed(active, BASE_HEIGHT + 1, stall_height - 1).IsValid());
    {
        // The key mirrors DB_CREDITPOOL_HISTORY in evo/creditpool.cpp
        // (file-local there).
        LOCK(::cs_main);
        const EvoViewScope view{active.EvoView()};
        auto tx{m_node.evodb->BeginTransaction(EvoDbIdentity::SNAPSHOT)};
        m_node.evodb->Erase(std::make_pair(std::string{"cpm_H"}, m_blocks.at(DIP3_HEIGHT)->GetHash()));
        tx->Commit();
    }
    SetMiscWarning(Untranslated("unrelated warning"));
    const auto warned = [](const std::string& text) {
        return GetWarnings(/*verbose=*/true).original.find(text) != std::string::npos;
    };
    const BlockValidationState state{Feed(active, stall_height, stall_height)};
    BOOST_CHECK(state.IsError());
    BOOST_CHECK_EQUAL(Height(active), stall_height - 1);
    const CBlockIndex* stalled{
        WITH_LOCK(::cs_main, return m_node.chainman->m_blockman.LookupBlockIndex(m_blocks.at(stall_height)->GetHash()))};
    BOOST_CHECK(!(WITH_LOCK(::cs_main, return stalled->nStatus) & BLOCK_FAILED_MASK));
    BOOST_CHECK(warned("Snapshot state unavailable"));
    BOOST_CHECK(warned("unrelated warning"));

    // Completing the snapshot does not reach the snapshot chainstate's own
    // view, so it stays stalled and keeps warning. The restart that promotes
    // the snapshot clears the warning, leaving the other one, and the block
    // connects on the validated state.
    Feed(Background(), 1, BASE_HEIGHT);
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return m_node.chainman->IsSnapshotValidated()));
    BOOST_CHECK_EQUAL(Height(active), stall_height - 1);
    BOOST_CHECK(warned("Snapshot state unavailable"));
    Restart();
    BOOST_REQUIRE(!m_node.chainman->IsSnapshotActive());
    BOOST_CHECK(!warned("Snapshot state unavailable"));
    BOOST_CHECK(warned("unrelated warning"));
    BlockValidationState restarted_state;
    BOOST_CHECK(m_node.chainman->ActiveChainstate().ActivateBestChain(restarted_state));
    BOOST_CHECK_EQUAL(Height(m_node.chainman->ActiveChainstate()), stall_height);
    SetMiscWarning(Untranslated(""));
}

//! On restart, a published generation is used only while the evo section it
//! was seeded from still matches the pin of its base.
BOOST_FIXTURE_TEST_CASE(chainstatemanager_evo_snapshot_restart_rechecks_the_pin, EvoSnapshotTestSetup)
{
    // The node has only the headers, so the snapshot is not validated yet
    // when the node restarts.
    const fs::path snapshot{DumpEvoSnapshot()};
    ForgetBlocks();
    BOOST_REQUIRE(LoadEvoSnapshot(snapshot));
    Restart();
    BOOST_REQUIRE(m_node.chainman->IsSnapshotActive());

    auto retained{*m_node.evodb->ReadRetainedEvoSection()};
    evo::EvoSnapshot tampered;
    CDataStream{retained, SER_DISK, CLIENT_VERSION} >> tampered;
    tampered.credit_history.front().credit_pool += 1;
    CDataStream stream{SER_DISK, CLIENT_VERSION};
    stream << tampered;
    retained.assign(UCharCast(stream.data()), UCharCast(stream.data() + stream.size()));
    BOOST_REQUIRE(m_node.evodb->GetRawDB().Write(EVODB_SNAPSHOT_EVO_SECTION, retained, /*fSync=*/true));
    SimulateNodeRestart();
    BOOST_CHECK(std::get<0>(TryLoadChainstate()) == node::ChainstateLoadStatus::FAILURE);
}

//! A section that passes every gate up to completion, under a pin that was
//! reviewed against it, but differs from the state background validation
//! derives in one field the earlier gates cannot check, is rejected when
//! completion rebuilds the section, and only then.
void CheckRejectedAtCompletion(EvoSnapshotTestSetup& setup, const std::function<void(evo::EvoSnapshot&)>& mutate)
{
    const int base_height{setup.Height(setup.m_node.chainman->ActiveChainstate())};
    auto [path, pin]{setup.DumpMutatedAndPinned(mutate)};
    BOOST_REQUIRE(setup.LoadEvoSnapshot(path));
    // The snapshot chainstate's own processing accepts it.
    setup.mineBlocks(2);
    BOOST_REQUIRE_EQUAL(setup.Height(setup.m_node.chainman->ActiveChainstate()), base_height + 2);
    BOOST_CHECK_EQUAL(setup.Complete(), SnapshotCompletionResult::EVO_STATE_MISMATCH);
    BOOST_CHECK(!setup.m_node.chainman->IsSnapshotActive());
    BOOST_CHECK(!setup.m_node.evodb->GetSnapshotGeneration());
    BOOST_CHECK(!setup.AnyBlockFailed());
}

BOOST_FIXTURE_TEST_CASE(chainstatemanager_evo_snapshot_completion_rebuild_current_limit, EvoSnapshotTestSetup)
{
    CheckRejectedAtCompletion(*this, [](auto& evo_section) { evo_section.credit_pool.currentLimit -= 1; });
}

BOOST_FIXTURE_TEST_CASE(chainstatemanager_evo_snapshot_completion_rebuild_indexes, EvoSnapshotTestSetup)
{
    CheckRejectedAtCompletion(*this, [](auto& evo_section) { BOOST_REQUIRE(evo_section.credit_pool.indexes.Add(12345)); });
}

BOOST_FIXTURE_TEST_CASE(chainstatemanager_evo_snapshot_completion_rebuild_signal, EvoSnapshotTestSetup)
{
    CheckRejectedAtCompletion(*this, [](auto& evo_section) { evo_section.mnhf_signals.emplace(27, BASE_HEIGHT - 1); });
}

BOOST_FIXTURE_TEST_CASE(chainstatemanager_evo_snapshot_completion_rebuild_quarter, EvoSnapshotTestSetup)
{
    CheckRejectedAtCompletion(*this, [](auto& evo_section) {
        auto& quarter{EvoSnapshotChainSetupType(evo_section, Consensus::LLMQType::LLMQ_TEST_DIP0024)
                          .rotation_snapshots.front()
                          .snapshot.activeQuorumMembers};
        quarter.front() = !quarter.front();
    });
}

BOOST_FIXTURE_TEST_CASE(chainstatemanager_evo_snapshot_completion_rebuild_credit_history, EvoSnapshotTestSetup)
{
    // An entry after v20, where it may be non-zero, that the snapshot
    // chainstate does not consume before completion.
    const size_t entry{static_cast<size_t>(DIP3_HEIGHT - (BASE_HEIGHT - Params().CreditPoolPeriodBlocks() + 1)) + 50};
    CheckRejectedAtCompletion(*this,
                              [entry](auto& evo_section) { evo_section.credit_history.at(entry).credit_pool += 1; });
}

BOOST_FIXTURE_TEST_CASE(chainstatemanager_evo_snapshot_completion_honest_section, EvoSnapshotTestSetup)
{
    BOOST_REQUIRE(ActivateEvoSnapshot());
    mineBlocks(2);
    BOOST_CHECK_EQUAL(Complete(), SnapshotCompletionResult::SUCCESS);
}

//! A score modifier can only be checked at load against a work block the node
//! has. Without it the carried one is used, and completion catches a wrong one
//! once background validation has connected the work block.
BOOST_FIXTURE_TEST_CASE(chainstatemanager_evo_snapshot_completion_checks_carried_modifiers, EvoSnapshotTestSetup)
{
    auto [path, pin]{DumpMutatedAndPinned([&](auto& evo_section) {
        for (auto& entry : evo_section.quorum_modifiers) {
            if (entry.llmq_type == Consensus::LLMQType::LLMQ_TEST_DIP0024) {
                entry.modifier = uint256::ONE;
                return;
            }
        }
        BOOST_FAIL("no rotating modifier");
    })};
    ForgetBlocks();
    BOOST_REQUIRE(LoadEvoSnapshot(path));
    Chainstate& background{Background()};
    // Completion runs when the background chainstate connects the base, and
    // shuts the node down on a mismatch.
    static std::atomic<int> exit_status{EXIT_SUCCESS};
    BOOST_REQUIRE(InitShutdownState(exit_status));
    Feed(background, 1, BASE_HEIGHT);
    BOOST_CHECK_EQUAL(exit_status.load(), EXIT_FAILURE);
    BOOST_CHECK(!m_node.chainman->IsSnapshotActive());
    BOOST_CHECK(&m_node.chainman->ActiveChainstate() == &background);
    BOOST_CHECK(!m_node.evodb->GetSnapshotGeneration());
    BOOST_CHECK(!AnyBlockFailed());
    BOOST_CHECK(ShutdownRequested());
    AbortShutdown();
}

//! Completion reads the body-derived inputs from what background validation
//! captured: a corrupted capture is a mismatch, and a missing one whose block
//! is gone is a local failure that neither validates nor rejects the snapshot.
BOOST_FIXTURE_TEST_CASE(chainstatemanager_evo_snapshot_completion_uses_the_captures, EvoSnapshotTestSetup)
{
    // The key mirrors the base-coinbase capture in evo/snapshot_chain.cpp
    // (file-local there).
    const uint256 base{WITH_LOCK(::cs_main, return m_node.chainman->ActiveTip()->GetBlockHash())};
    const auto coinbase_key{std::make_tuple(std::string{"b_evo_cap"}, uint8_t{2}, base,
                                            static_cast<uint8_t>(Consensus::LLMQType::LLMQ_NONE))};
    const auto write_capture = [&](const std::function<void()>& write) {
        LOCK(::cs_main);
        const EvoViewScope normal{Background().EvoView()};
        {
            auto tx{m_node.evodb->BeginTransaction(EvoDbIdentity::NORMAL)};
            write();
            tx->Commit();
        }
        BOOST_REQUIRE(m_node.evodb->CommitRootTransaction(EvoDbIdentity::NORMAL, /*sync=*/true));
    };

    // Activation refuses a snapshot whose input the background chainstate has
    // connected but no longer has the body of: nothing could capture it.
    CBlockIndex* window_block{WITH_LOCK(::cs_main, return m_node.chainman->ActiveChain()[BASE_HEIGHT - 10])};
    const fs::path snapshot{DumpEvoSnapshot()};
    WITH_LOCK(::cs_main, window_block->nStatus &= ~BLOCK_HAVE_DATA);
    BOOST_CHECK(!LoadEvoSnapshot(snapshot));
    WITH_LOCK(::cs_main, window_block->nStatus |= BLOCK_HAVE_DATA);

    BOOST_REQUIRE(ActivateEvoSnapshot());
    {
        std::pair<CTransactionRef, std::vector<uint256>> captured;
        BOOST_REQUIRE(m_node.evodb->GetRawDB().Read(coinbase_key, captured));
        CMutableTransaction corrupted{*captured.first};
        corrupted.vin.front().scriptSig << OP_TRUE;
        captured.first = MakeTransactionRef(std::move(corrupted));
        write_capture([&] { m_node.evodb->Write(coinbase_key, captured); });
    }
    BOOST_CHECK_EQUAL(Complete(), SnapshotCompletionResult::EVO_STATE_MISMATCH);

    Restart();
    BOOST_REQUIRE(ActivateEvoSnapshot());
    write_capture([&] { m_node.evodb->Erase(coinbase_key); });
    CBlockIndex* base_index{WITH_LOCK(::cs_main, return m_node.chainman->m_blockman.LookupBlockIndex(base))};
    WITH_LOCK(::cs_main, base_index->nStatus &= ~BLOCK_HAVE_DATA);
    const auto warned = [] {
        return GetWarnings(/*verbose=*/true).original.find("Snapshot state unavailable") != std::string::npos;
    };
    BOOST_CHECK_EQUAL(Complete(), SnapshotCompletionResult::EVO_DATA_UNAVAILABLE);
    // Nothing retries before a restart, so pruning goes on meanwhile.
    BOOST_CHECK(!WITH_LOCK(::cs_main, return m_node.chainman->m_blockman.GetPruneLock("assumeutxo")));
    BOOST_CHECK(m_node.chainman->IsSnapshotActive());
    BOOST_CHECK(WITH_LOCK(::cs_main, return !m_node.chainman->IsSnapshotValidated()));
    BOOST_CHECK(m_node.evodb->GetSnapshotGeneration());
    BOOST_CHECK(warned());
    // Once the body is back, completion succeeds.
    WITH_LOCK(::cs_main, base_index->nStatus |= BLOCK_HAVE_DATA);
    BOOST_CHECK_EQUAL(Complete(), SnapshotCompletionResult::SUCCESS);
    BOOST_CHECK(!warned());
}

//! Completion after the background chainstate's input bodies are gone, as on
//! a pruned node restarted since: every input only a body provides (the v20
//! work-block modifiers, the credit-pool window, the base coinbase and the
//! base pool, which folds in blocks below the window) must come from what was
//! captured, and the snapshot completes from those alone.
BOOST_FIXTURE_TEST_CASE(chainstatemanager_evo_snapshot_completion_from_captures_alone, EvoQuorumSnapshotTestSetup)
{
    const int base_height{QUORUM_BASE_HEIGHT + 24};
    MineTo(base_height - 10);
    Withdraw(/*index=*/1, 30 * COIN);
    MineTo(base_height);
    PinTip();
    BOOST_REQUIRE(ActivateEvoSnapshot());
    std::vector<CBlockIndex*> pruned;
    {
        LOCK(::cs_main);
        const evo::EvoSnapshotCapturePlan* plan{m_node.chainman->EvoCapturePlan()};
        BOOST_REQUIRE(plan && !plan->modifier_blocks.empty());
        for (CBlockIndex* index{Background().m_chain.Tip()}; index; index = index->pprev) {
            if (index->nStatus & BLOCK_HAVE_DATA) {
                index->nStatus &= ~BLOCK_HAVE_DATA;
                pruned.push_back(index);
            }
        }
    }
    DropEvoCaches();
    BOOST_CHECK_EQUAL(Complete(), SnapshotCompletionResult::SUCCESS);
    LOCK(::cs_main);
    for (CBlockIndex* index : pruned)
        index->nStatus |= BLOCK_HAVE_DATA;
}

//! The honest run on a node that has only the headers: the snapshot chainstate
//! connects blocks above the base, the node restarts, its caches are dropped,
//! and background validation, which announces none of the MN-list changes it
//! connects, captures the evo inputs as it goes while the prune lock keeps the
//! rest. Completion then promotes the snapshot chainstate's Evo state, which
//! NORMAL consumers read afterwards.
BOOST_FIXTURE_TEST_CASE(chainstatemanager_evo_snapshot_honest_completion, EvoSnapshotTestSetup)
{
    const fs::path snapshot{DumpEvoSnapshot()};
    const int tip_height{BASE_HEIGHT + 10};
    MineTo(tip_height);
    const CAmount tip_pool{
        WITH_LOCK(::cs_main, return m_node.special_tx->m_cpoolman->GetCreditPool(m_node.chainman->ActiveTip()).locked)};
    ForgetBlocks();
    BOOST_REQUIRE(LoadEvoSnapshot(snapshot));
    BOOST_REQUIRE(Feed(m_node.chainman->ActiveChainstate(), BASE_HEIGHT + 1, BASE_HEIGHT + 5).IsValid());
    Restart();
    {
        const CEvoDB unrelated{util::DbWrapperParams{.path = {}, .memory = true, .wipe = true}};
    }
    BOOST_REQUIRE(Feed(m_node.chainman->ActiveChainstate(), BASE_HEIGHT + 6, tip_height).IsValid());

    const int window_first{BASE_HEIGHT - Params().CreditPoolPeriodBlocks() + 1};
    const auto prune_lock = [&] {
        LOCK(::cs_main);
        const auto lock{m_node.chainman->m_blockman.GetPruneLock("assumeutxo")};
        return lock ? std::optional<int>{lock->height_first} : std::nullopt;
    };
    // The key mirrors the base-coinbase capture in evo/snapshot_chain.cpp
    // (file-local there).
    const auto coinbase_captured = [&] {
        std::pair<CTransactionRef, std::vector<uint256>> captured;
        return m_node.evodb->GetRawDB().Read(std::make_tuple(std::string{"b_evo_cap"}, uint8_t{2},
                                                             m_blocks.at(BASE_HEIGHT)->GetHash(),
                                                             static_cast<uint8_t>(Consensus::LLMQType::LLMQ_NONE)),
                                             captured);
    };

    MnListChangeCounter events;
    RegisterValidationInterface(&events);
    int ui_mn_list_changed{0};
    auto ui_connection{uiInterface.NotifyMasternodeListChanged_connect(
        [&](const CDeterministicMNList&, const CBlockIndex*) { ++ui_mn_list_changed; })};
    Chainstate& background{Background()};
    // Every body above the background tip is still needed.
    BOOST_REQUIRE(Feed(background, 1, 50).IsValid());
    BOOST_CHECK(prune_lock() == 51);
    BOOST_REQUIRE(Feed(background, 51, window_first + 4).IsValid());
    BOOST_CHECK(prune_lock() == window_first + 5);
    BOOST_REQUIRE(Feed(background, window_first + 5, BASE_HEIGHT - 10).IsValid());

    // Recomputing the EHF signals at the background tip, past DIP3 and v20,
    // writes only into the background's view.
    {
        LOCK(::cs_main);
        const auto normal_key{std::make_pair(std::string{"test_force_signal"}, uint8_t{0})};
        const auto snapshot_key{std::make_pair(std::string{"test_force_signal"}, uint8_t{1})};
        const EvoViewToken snapshot_view{m_node.evodb->ViewFor(EvoDbIdentity::SNAPSHOT)};
        for (const auto& [view, key] :
             {std::make_pair(background.EvoView(), normal_key), std::make_pair(snapshot_view, snapshot_key)}) {
            const EvoViewScope scope{view};
            auto tx{m_node.evodb->BeginTransaction(view.identity)};
            m_node.evodb->Write(key, uint8_t{1});
            tx->Commit();
        }
        DataStream snapshot_raw_key{CEvoDBStore::GenerationPrefix(snapshot_view.generation)};
        snapshot_raw_key << snapshot_key;
        BOOST_REQUIRE(m_node.special_tx->m_mnhfman->ForceSignalDBUpdate(background.EvoView(), background.m_chain.Tip()));
        BOOST_CHECK(m_node.evodb->GetRawDB().Exists(normal_key));
        BOOST_CHECK(!m_node.evodb->GetRawDB().Exists(snapshot_raw_key));
        BOOST_REQUIRE(m_node.evodb->CommitRootTransaction(EvoDbIdentity::SNAPSHOT));
        BOOST_CHECK(m_node.evodb->GetRawDB().Exists(snapshot_raw_key));
    }

    Feed(background, BASE_HEIGHT - 9, BASE_HEIGHT);
    SyncWithValidationInterfaceQueue();
    ui_connection.disconnect();
    UnregisterValidationInterface(&events);
    // The masternodes were registered below the base.
    BOOST_CHECK_EQUAL(events.changes, 0);
    BOOST_CHECK_EQUAL(ui_mn_list_changed, 0);

    // Promotion at the next startup repeats the check, so it still needs the
    // captures and the bodies they spare.
    BOOST_CHECK(WITH_LOCK(::cs_main, return m_node.chainman->IsSnapshotValidated()));
    BOOST_CHECK(prune_lock() == BASE_HEIGHT);
    BOOST_CHECK(coinbase_captured());
    BOOST_CHECK(!ShutdownRequested());

    Restart();
    BOOST_CHECK(!prune_lock());
    BOOST_CHECK(!coinbase_captured());
    LOCK(::cs_main);
    BOOST_REQUIRE(!m_node.chainman->IsSnapshotActive());
    BOOST_REQUIRE_EQUAL(m_node.chainman->ActiveHeight(), tip_height);
    const EvoViewScope normal{m_node.evodb->ViewFor(EvoDbIdentity::NORMAL)};
    BOOST_CHECK_EQUAL(m_node.dmnman->GetListForBlock(m_node.chainman->ActiveTip()).GetCounts().total(), size_t{MN_COUNT});
    BOOST_CHECK_EQUAL(m_node.special_tx->m_cpoolman->GetCreditPool(m_node.chainman->ActiveTip()).locked, tip_pool);
}

//! A published generation is refused at startup once the consensus
//! configuration it was published under changed, even in a field only the
//! fingerprint covers.
BOOST_FIXTURE_TEST_CASE(chainstatemanager_evo_snapshot_refused_after_deployment_change, EvoSnapshotTestSetup)
{
    const fs::path snapshot{DumpEvoSnapshot()};
    ForgetBlocks();
    BOOST_REQUIRE(LoadEvoSnapshot(snapshot));
    auto& v24{const_cast<Consensus::Params&>(Params().GetConsensus()).vDeployments[Consensus::DEPLOYMENT_V24]};
    struct V24Restore {
        Consensus::BIP9Deployment& deployment;
        const Consensus::BIP9Deployment saved{deployment};
        ~V24Restore() { deployment = saved; }
    } v24_restore{v24};
    v24.min_activation_height += 1;
    SimulateNodeRestart();
    BOOST_CHECK(std::get<0>(TryLoadChainstate()) == node::ChainstateLoadStatus::FAILURE);
}

//! With real quorums: a node with only the headers loads the snapshot at the
//! block before the next cycle, after its base cycle's commitments are mined,
//! and connects the synced node's blocks through the next cycle's mining
//! window. Their commitments are verified against members computed from the
//! carried quarters, including the base cycle's own, and the ChainLocks every
//! coinbase carries against the carried quorums.
BOOST_FIXTURE_TEST_CASE(chainstatemanager_evo_snapshot_real_quorums_cold_node, EvoQuorumSnapshotTestSetup)
{
    constexpr auto ROTATING{Consensus::LLMQType::LLMQ_TEST_DIP0024};
    constexpr int NEXT_CYCLE{QUORUM_BASE_HEIGHT + 1};
    const int tip_height{NEXT_CYCLE + 24};
    PinTip();
    const fs::path snapshot{DumpEvoSnapshot()};
    MineTo(tip_height);
    const auto expected{Members(m_node.chainman->ActiveChainstate(), ROTATING, NEXT_CYCLE)};
    BOOST_REQUIRE(!expected.empty());
    const auto next_commitment_mined = [&] {
        LOCK(::cs_main);
        return m_node.llmq_ctx->quorum_block_processor->HasMinedCommitment(
            ROTATING, m_node.chainman->m_best_header->GetAncestor(NEXT_CYCLE)->GetBlockHash(),
            m_node.chainman->ActiveChain());
    };
    BOOST_REQUIRE(next_commitment_mined());
    ForgetBlocks();

    BOOST_REQUIRE(LoadEvoSnapshot(snapshot));
    Chainstate& active{m_node.chainman->ActiveChainstate()};
    BOOST_CHECK(Feed(active, QUORUM_BASE_HEIGHT + 1, tip_height).IsValid());
    BOOST_CHECK_EQUAL(Height(active), tip_height);
    BOOST_CHECK(Members(active, ROTATING, NEXT_CYCLE) == expected);

    // The commitment the snapshot chainstate mined, and the quorum built from
    // it, stay in its view: warming its caches gives NORMAL nothing.
    const uint256 next_cycle_hash{WITH_LOCK(::cs_main, return active.m_chain[NEXT_CYCLE]->GetBlockHash())};
    const auto mined_in = [&](const Chainstate& chainstate) {
        LOCK(::cs_main);
        const EvoViewScope view{chainstate.EvoView()};
        return m_node.llmq_ctx->quorum_block_processor->GetMinedCommitment(ROTATING, next_cycle_hash);
    };
    const auto commitment{mined_in(active)};
    BOOST_REQUIRE(!commitment.second.IsNull());
    BOOST_CHECK(WITH_LOCK(::cs_main, const EvoViewScope view{active.EvoView()};
                          return m_node.llmq_ctx->qman->GetQuorum(ROTATING, next_cycle_hash, active.m_chain)) != nullptr);
    BOOST_CHECK(mined_in(Background()).second.IsNull());

    // Background validation captures the evo inputs as it connects their
    // blocks, across a restart that reloads the capture plan. With the
    // retained section briefly gone, reaching the base cannot complete;
    // completion then succeeds from the captures alone, every body at or
    // below the base gone and the caches cold, as a pruned node's must when
    // startup repeats it. Promotion makes the mined commitment NORMAL's.
    const int restart_height{QUORUM_BASE_HEIGHT / 2};
    BOOST_REQUIRE(Feed(Background(), 1, restart_height).IsValid());
    Restart();
    BOOST_REQUIRE(Feed(Background(), restart_height + 1, QUORUM_BASE_HEIGHT - 1).IsValid());
    const auto retained{m_node.evodb->ReadRetainedEvoSection()};
    BOOST_REQUIRE(retained);
    BOOST_REQUIRE(m_node.evodb->GetRawDB().Erase(EVODB_SNAPSHOT_EVO_SECTION, /*fSync=*/true));
    Feed(Background(), QUORUM_BASE_HEIGHT, QUORUM_BASE_HEIGHT);
    BOOST_REQUIRE_EQUAL(Height(Background()), QUORUM_BASE_HEIGHT);
    BOOST_REQUIRE(!WITH_LOCK(::cs_main, return m_node.chainman->IsSnapshotValidated()));
    BOOST_REQUIRE(m_node.evodb->GetRawDB().Write(EVODB_SNAPSHOT_EVO_SECTION, *retained, /*fSync=*/true));
    std::vector<CBlockIndex*> pruned;
    {
        LOCK(::cs_main);
        for (CBlockIndex* index{Background().m_chain.Tip()}; index; index = index->pprev) {
            if (index->nStatus & BLOCK_HAVE_DATA) {
                index->nStatus &= ~BLOCK_HAVE_DATA;
                pruned.push_back(index);
            }
        }
    }
    DropEvoCaches();
    BOOST_CHECK_EQUAL(Complete(), SnapshotCompletionResult::SUCCESS);
    WITH_LOCK(::cs_main, for (CBlockIndex* index : pruned) index->nStatus |= BLOCK_HAVE_DATA);
    Restart();
    BOOST_REQUIRE(!m_node.chainman->IsSnapshotActive());
    LOCK(::cs_main);
    const EvoViewScope normal{m_node.evodb->ViewFor(EvoDbIdentity::NORMAL)};
    BOOST_CHECK(next_commitment_mined());
    const auto promoted{m_node.llmq_ctx->quorum_block_processor->GetMinedCommitment(ROTATING, next_cycle_hash)};
    BOOST_CHECK(SerializeHash(promoted) == SerializeHash(commitment));
    BOOST_CHECK(m_node.llmq_ctx->qman->GetQuorum(ROTATING, next_cycle_hash, m_node.chainman->ActiveChain()) != nullptr);
}

//! The base is h+5, so the next cycle's work block lies above it: activation
//! computes nothing for that cycle and is not refused for it. The node with
//! only the headers connects the synced node's blocks through the next
//! cycle's mining window, computing its members from the carried quarters
//! (the base cycle's included) and the lists it built itself, and accepts its
//! signed commitments.
BOOST_FIXTURE_TEST_CASE(chainstatemanager_evo_snapshot_real_quorums_base_before_work, EvoQuorumSnapshotTestSetup)
{
    constexpr auto ROTATING{Consensus::LLMQType::LLMQ_TEST_DIP0024};
    constexpr int CYCLE_LENGTH{24};
    constexpr int CYCLE_HEIGHT{QUORUM_BASE_HEIGHT + 1};
    constexpr int NEXT_CYCLE{CYCLE_HEIGHT + CYCLE_LENGTH};
    const int base_height{CYCLE_HEIGHT + 5};
    BOOST_REQUIRE_GT(NEXT_CYCLE - llmq::WORK_DIFF_DEPTH, base_height);
    MineTo(base_height);
    PinTip();
    const fs::path snapshot{DumpEvoSnapshot()};
    const int tip_height{NEXT_CYCLE + CYCLE_LENGTH};
    MineTo(tip_height);
    const auto expected{Members(m_node.chainman->ActiveChainstate(), ROTATING, NEXT_CYCLE)};
    BOOST_REQUIRE(!expected.empty());
    const uint256 next_cycle_hash{
        WITH_LOCK(::cs_main, return m_node.chainman->m_best_header->GetAncestor(NEXT_CYCLE)->GetBlockHash())};
    const auto mined_in = [&](const Chainstate& chainstate) {
        LOCK(::cs_main);
        const EvoViewScope view{chainstate.EvoView()};
        return m_node.llmq_ctx->quorum_block_processor->GetMinedCommitment(ROTATING, next_cycle_hash);
    };
    const auto commitment{mined_in(m_node.chainman->ActiveChainstate())};
    BOOST_REQUIRE(!commitment.second.IsNull());
    ForgetBlocks();

    BOOST_REQUIRE(LoadEvoSnapshot(snapshot));
    Chainstate& active{m_node.chainman->ActiveChainstate()};
    BOOST_CHECK(Feed(active, base_height + 1, tip_height).IsValid());
    BOOST_CHECK_EQUAL(Height(active), tip_height);
    BOOST_CHECK(Members(active, ROTATING, NEXT_CYCLE) == expected);
    BOOST_CHECK(SerializeHash(mined_in(active)) == SerializeHash(commitment));
}

//! Acceptance 4 on real quorums. Each quorum consumer the snapshot chainstate
//! warms at blocks at or below the base keeps what it cached in the snapshot
//! view: the quarters, the members of rotating and plain quorums, the quorums,
//! a smaller then a larger quorum scan, the commitment scan, the mined
//! commitment and the block it was mined in, the commitment indexes, the CbTx
//! commitment hashes and the remembered coinbase ChainLock. Before background
//! validation connects those blocks NORMAL finds nothing there or cannot tell;
//! once it has connected them it answers as the synced node did.
BOOST_FIXTURE_TEST_CASE(chainstatemanager_evo_snapshot_real_quorum_caches, EvoQuorumSnapshotTestSetup)
{
    constexpr auto ROTATING{Consensus::LLMQType::LLMQ_TEST_DIP0024};
    constexpr auto PLAIN{Consensus::LLMQType::LLMQ_TEST};
    constexpr int CYCLE_HEIGHT{QUORUM_BASE_HEIGHT - QUORUM_BASE_HEIGHT % 24};
    constexpr int LAST{QUORUM_BASE_HEIGHT - 1};
    using Observations = std::map<std::string, std::optional<uint256>>;
    // What each consumer returns at those blocks, hashed; zero for nothing,
    // nullopt when the view cannot tell.
    const auto observe = [&](const EvoViewToken& view) {
        LOCK(::cs_main);
        const EvoViewScope scope{view};
        const CBlockIndex* cycle{m_node.chainman->m_best_header->GetAncestor(CYCLE_HEIGHT)};
        const CBlockIndex* last{m_node.chainman->m_best_header->GetAncestor(LAST)};
        const CChain& chain{m_node.chainman->ActiveChain()};
        auto& qman{*m_node.llmq_ctx->qman};
        auto& qblockman{*m_node.llmq_ctx->quorum_block_processor};
        const auto hash_of = [](const auto& hashes) { return hashes.empty() ? uint256{} : SerializeHash(hashes); };
        const auto members = [&](Consensus::LLMQType type, const CBlockIndex* quorum) {
            std::vector<uint256> hashes;
            for (const auto& member : llmq::utils::GetAllQuorumMembers(type, Util(quorum)))
                hashes.push_back(member->proTxHash);
            return hash_of(hashes);
        };
        const auto scan = [&](size_t count) {
            std::vector<uint256> hashes;
            for (const auto& quorum : qman.ScanQuorums(ROTATING, last, count))
                hashes.push_back(quorum->qc->quorumHash);
            return hash_of(hashes);
        };
        const auto blocks = [&](const std::vector<const CBlockIndex*>& indexes) {
            std::vector<uint256> hashes;
            for (const CBlockIndex* index : indexes)
                hashes.push_back(index->GetBlockHash());
            return hash_of(hashes);
        };
        std::vector<std::pair<std::string, std::function<uint256()>>> consumers{
            {"quarters",
             [&] {
                 const auto quarters{m_node.llmq_ctx->qsnapman->GetSnapshotForBlock(ROTATING, cycle)};
                 return quarters ? SerializeHash(*quarters) : uint256{};
             }},
            {"rotating members", [&] { return members(ROTATING, last->GetAncestor(CYCLE_HEIGHT + 1)); }},
            {"plain members", [&] { return members(PLAIN, cycle); }},
            {"quorum",
             [&]() EXCLUSIVE_LOCKS_REQUIRED(::cs_main) {
                 const auto quorum{qman.GetQuorum(ROTATING, cycle->GetBlockHash(), chain)};
                 return quorum ? SerializeHash(quorum->qc->quorumPublicKey) : uint256{};
             }},
            {"smaller scan", [&] { return scan(1); }},
            {"larger scan", [&] { return scan(4); }},
            {"commitment scan",
             [&] {
                 std::vector<uint256> hashes;
                 for (const auto& qc : qman.ScanCommitments(ROTATING, last, 4))
                     hashes.push_back(SerializeHash(qc));
                 return hash_of(hashes);
             }},
            {"mined commitment",
             [&] {
                 const auto mined{qblockman.GetMinedCommitment(ROTATING, cycle->GetBlockHash())};
                 return mined.second.IsNull() ? uint256{} : SerializeHash(mined);
             }},
            {"mined in chain",
             [&]() EXCLUSIVE_LOCKS_REQUIRED(::cs_main) {
                 return qblockman.HasMinedCommitment(ROTATING, cycle->GetBlockHash(), chain) ? uint256::ONE : uint256{};
             }},
            // The section carries the active quorums only.
            {"mined until",
             [&] {
                 return blocks(qblockman.GetMinedCommitmentsUntilBlock(PLAIN, last,
                                                                       Params().GetLLMQ(PLAIN)->signingActiveQuorumCount));
             }},
            {"mined per index",
             [&] { return blocks(qblockman.GetLastMinedCommitmentsPerQuorumIndexUntilBlock(ROTATING, last, 0)); }},
            {"qc hashes",
             [&] {
                 const auto qc_hashes{qblockman.GetQcHashes(last)};
                 if (!qc_hashes) return uint256{};
                 std::vector<uint256> hashes;
                 for (const auto& [_, type_hashes] : qc_hashes->first)
                     hashes.insert(hashes.end(), type_hashes.begin(), type_hashes.end());
                 for (const auto& [_, indexed] : qc_hashes->second) {
                     for (const auto& [quorum_index, hash] : indexed)
                         hashes.push_back(hash);
                 }
                 return hash_of(hashes);
             }},
        };
        Observations result;
        for (const auto& [name, consumer] : consumers) {
            try {
                result[name] = consumer();
            } catch (const BlockDataUnavailableError&) {
                result[name] = std::nullopt;
            }
        }
        return result;
    };

    PinTip();
    const Observations synced{observe(m_node.evodb->ViewFor(EvoDbIdentity::NORMAL))};
    for (const auto& [name, value] : synced)
        BOOST_REQUIRE_MESSAGE(value && !value->IsNull(), name);
    const fs::path snapshot{DumpEvoSnapshot()};
    const int tip_height{QUORUM_BASE_HEIGHT + 4};
    MineTo(tip_height);
    ForgetBlocks();
    BOOST_REQUIRE(LoadEvoSnapshot(snapshot));
    Chainstate& active{m_node.chainman->ActiveChainstate()};
    BOOST_REQUIRE(Feed(active, QUORUM_BASE_HEIGHT + 1, tip_height).IsValid());

    const auto check_synced = [&](const Observations& observed) {
        for (const auto& [name, value] : synced) {
            const auto& got{observed.at(name)};
            BOOST_CHECK_MESSAGE(got == value, name << ": " << (got ? got->ToString() : "unavailable"));
        }
    };
    check_synced(observe(active.EvoView()));
    for (const auto& [name, value] : observe(Background().EvoView())) {
        BOOST_CHECK_MESSAGE(!value || value->IsNull(), name << ": " << value->ToString());
    }

    // The snapshot chainstate remembers its tip's coinbase ChainLock. Without
    // the tip's body NORMAL knows of none, so a null ChainLock in the block
    // after it passes there.
    const CBlockIndex* tip{WITH_LOCK(::cs_main, return active.m_chain.Tip())};
    BOOST_REQUIRE(WITH_LOCK(::cs_main, const EvoViewScope view{active.EvoView()};
                            return m_node.special_tx->GetCoinbaseChainlock(tip).status) ==
                  CoinbaseChainlock::Status::CERTIFIED);
    CBlockIndex next;
    next.pprev = const_cast<CBlockIndex*>(tip);
    next.nHeight = tip->nHeight + 1;
    CCbTx null_chainlock;
    null_chainlock.nVersion = CCbTx::Version::CLSIG_AND_BALANCE;
    const auto check_chainlock = [&](const EvoViewToken& view) {
        LOCK(::cs_main);
        const EvoViewScope scope{view};
        BlockValidationState state;
        CheckCbTxBestChainlock(null_chainlock, &next, Params().GetConsensus(), active.m_chain, *m_node.llmq_ctx->qman,
                               *m_node.chainlocks, *m_node.special_tx, state);
        return state.GetRejectReason();
    };
    BOOST_CHECK_EQUAL(check_chainlock(active.EvoView()), "bad-cbtx-null-clsig");
    WITH_LOCK(::cs_main, const_cast<CBlockIndex*>(tip)->nStatus &= ~BLOCK_HAVE_DATA);
    BOOST_CHECK_EQUAL(check_chainlock(Background().EvoView()), "");
    WITH_LOCK(::cs_main, const_cast<CBlockIndex*>(tip)->nStatus |= BLOCK_HAVE_DATA);

    // Caches keyed by what NORMAL derives itself, the quorum base blocks, can
    // only show a difference when the snapshot view holds different values
    // for the same keys. A carried commitment for the base cycle with another
    // quorum key gives it different quorums, scans and CbTx hashes; none of
    // them reaches NORMAL, whose validation connecting those blocks would
    // otherwise verify ChainLocks against the wrong quorum key.
    const uint256 quorum_hash{
        WITH_LOCK(::cs_main, return m_node.chainman->m_best_header->GetAncestor(CYCLE_HEIGHT)->GetBlockHash())};
    // The key mirrors DB_MINED_COMMITMENT in llmq/blockprocessor.cpp
    // (file-local there).
    const auto commitment_key{std::make_pair(std::string{"q_mc"}, std::make_pair(ROTATING, quorum_hash))};
    {
        LOCK(::cs_main);
        const EvoViewScope view{active.EvoView()};
        {
            auto tx{m_node.evodb->BeginTransaction(EvoDbIdentity::SNAPSHOT)};
            std::pair<llmq::CFinalCommitment, uint256> mined;
            BOOST_REQUIRE(m_node.evodb->Read(commitment_key, mined));
            CBLSSecretKey other_key;
            other_key.MakeNewKey();
            mined.first.quorumPublicKey = other_key.GetPublicKey();
            m_node.evodb->Write(commitment_key, mined);
            tx->Commit();
        }
        BOOST_REQUIRE(m_node.evodb->CommitRootTransaction(EvoDbIdentity::SNAPSHOT, /*sync=*/true));
    }
    DropEvoCaches();
    const Observations other{observe(active.EvoView())};
    for (const char* name : {"quorum", "commitment scan", "mined commitment", "qc hashes"}) {
        BOOST_CHECK_MESSAGE(other.at(name) != synced.at(name), name);
    }

    BOOST_REQUIRE(Feed(Background(), 1, LAST).IsValid());
    check_synced(observe(Background().EvoView()));
    // NORMAL now asks for the keys the snapshot view uses, and drops some
    // caches whenever it writes commitments; with no write in between, the
    // snapshot view warms them first.
    DropEvoCaches();
    BOOST_CHECK(observe(active.EvoView()) == other);
    check_synced(observe(Background().EvoView()));
}

//! The DKG of rotating index 1 fails for ten cycles in a row, so the newest
//! index-1 commitment a base carries targets h-10C and the one before it
//! h-11C. Activation computes their members, in the dry run with cold caches,
//! from the quarters their own cycles need (h-11C to h-14C), and they match
//! the synced node's. The quarter of h-10C is only an output of that
//! computation, so the snapshot view neither carries nor keeps it; without a
//! quarter the computation needs, the members are unavailable, never wrong.
BOOST_FIXTURE_TEST_CASE(chainstatemanager_evo_snapshot_failed_dkg_old_target, EvoQuorumSnapshotTestSetup)
{
    constexpr auto ROTATING{Consensus::LLMQType::LLMQ_TEST_DIP0024};
    constexpr int CYCLE_LENGTH{24};
    constexpr int FAILED_CYCLES{10};
    // The fixture has mined the commitments up to its base already.
    const int h{QUORUM_BASE_HEIGHT + 1 + (FAILED_CYCLES - 1) * CYCLE_LENGTH};
    for (int cycle{h - (FAILED_CYCLES - 1) * CYCLE_LENGTH}; cycle <= h; cycle += CYCLE_LENGTH) {
        m_failed_dkgs.emplace(ROTATING, cycle + 1);
    }
    const int target{h - FAILED_CYCLES * CYCLE_LENGTH};
    MineTo(h + 3);
    PinTip();
    std::vector<int> old_targets;
    {
        LOCK(::cs_main);
        auto evo_section{BuildEvoSnapshotAtTip()};
        BOOST_REQUIRE(evo_section);
        for (const auto& entry : EvoSnapshotChainSetupType(*evo_section, ROTATING).active_commitments) {
            if (entry.commitment.quorumIndex == 1) {
                old_targets.push_back(m_node.chainman->m_blockman.LookupBlockIndex(entry.quorum_base_block_hash)->nHeight);
            }
        }
    }
    BOOST_REQUIRE_EQUAL(old_targets.size(), 1U);
    BOOST_REQUIRE_EQUAL(old_targets.front(), target + 1);
    std::map<int, std::vector<uint256>> expected;
    for (const int height : {target + 1, target - CYCLE_LENGTH + 1}) {
        expected[height] = Members(m_node.chainman->ActiveChainstate(), ROTATING, height);
        BOOST_REQUIRE(!expected[height].empty());
    }
    const fs::path snapshot{DumpEvoSnapshot()};
    ForgetBlocks();
    DropEvoCaches();
    BOOST_REQUIRE(LoadEvoSnapshot(snapshot));
    Chainstate& active{m_node.chainman->ActiveChainstate()};
    for (const auto& [height, members] : expected) {
        BOOST_CHECK_MESSAGE(Members(active, ROTATING, height) == members, height);
    }

    const auto quarter_at = [&](int height) {
        LOCK(::cs_main);
        const EvoViewScope view{active.EvoView()};
        return m_node.llmq_ctx->qsnapman->GetSnapshotForBlock(ROTATING,
                                                              m_node.chainman->m_best_header->GetAncestor(height));
    };
    BOOST_CHECK(!quarter_at(target));
    const int predecessor{target - 3 * CYCLE_LENGTH};
    BOOST_REQUIRE(quarter_at(predecessor));
    {
        LOCK(::cs_main);
        const EvoViewScope view{active.EvoView()};
        {
            auto tx{m_node.evodb->BeginTransaction(EvoDbIdentity::SNAPSHOT)};
            // The key mirrors DB_QUORUM_SNAPSHOT in llmq/snapshot.cpp
            // (file-local there).
            m_node.evodb->Erase(
                std::make_pair(std::string{"llmq_S"},
                               SerializeHash(std::make_pair(
                                   ROTATING, m_node.chainman->m_best_header->GetAncestor(predecessor)->GetBlockHash()))));
            tx->Commit();
        }
        BOOST_REQUIRE(m_node.evodb->CommitRootTransaction(EvoDbIdentity::SNAPSHOT, /*sync=*/true));
    }
    DropEvoCaches();
    BOOST_CHECK(!quarter_at(predecessor));
    BOOST_CHECK_THROW(Members(active, ROTATING, target + 1), BlockDataUnavailableError);
}

//! Withdrawals against the credit-pool window: two are mined before the base,
//! inside its window, so the carried entries decide the limits after it. The
//! synced node withdraws exactly its limit right after the base, and again
//! once the first carried withdrawal leaves the window and frees its part. A
//! node with only the headers connects those blocks across a restart. On both
//! sides of that window edge its limit is the synced node's: the synced chain's
//! exact-limit withdrawals connect, and anything more is rejected. A carried
//! index cannot be used again.
BOOST_FIXTURE_TEST_CASE(chainstatemanager_evo_snapshot_credit_window_withdrawals, EvoQuorumSnapshotTestSetup)
{
    const int window{Params().CreditPoolPeriodBlocks()};
    const int base_height{QUORUM_BASE_HEIGHT + 48};
    const int first_unlock{base_height - 41};
    MineTo(first_unlock - 1);
    Withdraw(/*index=*/1, 30 * COIN);
    MineTo(base_height - 11);
    Withdraw(/*index=*/2, 30 * COIN);
    MineTo(base_height);
    PinTip();
    {
        LOCK(::cs_main);
        auto evo_section{BuildEvoSnapshotAtTip()};
        BOOST_REQUIRE(evo_section);
        BOOST_REQUIRE_GT(evo_section->credit_pool.latelyUnlocked, 0);
    }
    const fs::path snapshot{DumpEvoSnapshot()};
    MineTo(base_height + 2);
    Withdraw(/*index=*/3);
    // The last block whose limit still counts the first withdrawal, and the
    // first that no longer does.
    const int last_counted{first_unlock + window};
    MineTo(last_counted);
    Withdraw(/*index=*/4);
    const int tip_height{base_height + window + 2};
    MineTo(tip_height);
    std::map<int, CAmount> synced_limit;
    for (const int height : {last_counted, last_counted + 1}) {
        synced_limit[height] = WITH_LOCK(
            ::cs_main,
            return m_node.special_tx->m_cpoolman->GetCreditPool(m_node.chainman->ActiveChain()[height - 1]).currentLimit);
    }
    BOOST_REQUIRE_GT(synced_limit.at(last_counted + 1), synced_limit.at(last_counted));
    ForgetBlocks();

    BOOST_REQUIRE(LoadEvoSnapshot(snapshot));
    Chainstate& active{m_node.chainman->ActiveChainstate()};
    // The synced node's best ChainLock is ahead of the blocks tested here.
    m_node.chainlocks->ResetChainlock();
    const auto rejection = [&](const CTransactionRef& unlock) {
        const CBlock block{
            CreateBlock({CMutableTransaction{*unlock}}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()), active)};
        LOCK(::cs_main);
        BlockValidationState state;
        TestBlockValidity(state, *m_node.chainlocks, *m_node.evodb, Params(), active, block, active.m_chain.Tip(),
                          /*fCheckPOW=*/false);
        return state.GetRejectReason();
    };
    constexpr uint32_t FEE{1000};
    for (const auto& [height, limit] : synced_limit) {
        BOOST_REQUIRE(Feed(active, Height(active) + 1, height - 1).IsValid());
        BOOST_CHECK_EQUAL(rejection(MakeAssetUnlock(/*index=*/5, std::max<CAmount>(limit - FEE + 1, 1), FEE)),
                          "failed-creditpool-unlock-too-much");
    }
    for (const uint64_t carried : {1, 2}) {
        BOOST_CHECK_EQUAL(rejection(MakeAssetUnlock(carried, 1000, FEE)), "bad-assetunlock-duplicated-index");
    }
    Restart();
    Chainstate& restarted{m_node.chainman->ActiveChainstate()};
    BOOST_REQUIRE(restarted.m_from_snapshot_blockhash);
    BOOST_CHECK(Feed(restarted, Height(restarted) + 1, tip_height).IsValid());
    BOOST_CHECK_EQUAL(Height(restarted), tip_height);
}

//! A quorum-signed hard-fork signal for a bit no deployment uses is valid and
//! mined exactly at the base. The section carries the raw record, which the
//! snapshot chainstate filters itself for the blocks after the base, and the
//! honest completion and promotion keep NORMAL's own raw record.
BOOST_FIXTURE_TEST_CASE(chainstatemanager_evo_snapshot_unknown_bit_signal, EvoQuorumSnapshotTestSetup)
{
    constexpr uint8_t UNKNOWN_BIT{5};
    const int base_height{QUORUM_BASE_HEIGHT + 24};
    MineTo(base_height - 1);
    {
        MNHFTxPayload payload;
        payload.nVersion = 1;
        payload.signal.versionBit = UNKNOWN_BIT;
        LOCK(::cs_main);
        const auto type{Params().GetConsensus().llmqTypeMnhf};
        const auto quorums{m_node.llmq_ctx->qman->ScanQuorums(type, m_node.chainman->ActiveTip(), 1)};
        BOOST_REQUIRE(!quorums.empty());
        payload.signal.quorumHash = quorums.front()->qc->quorumHash;
        const uint256 msg_hash{payload.PrepareTx().GetHash()};
        payload.signal.sig = SignAs(type, payload.signal.quorumHash, payload.GetRequestId(), msg_hash);
        m_mnhf_signal = payload.PrepareTx();
    }
    CreateAndProcessBlock({*m_mnhf_signal}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()));
    BOOST_REQUIRE_EQUAL(Height(m_node.chainman->ActiveChainstate()), base_height);
    const auto raw_at_base = [&](const EvoViewToken& view) {
        LOCK(::cs_main);
        const EvoViewScope scope{view};
        const CBlockIndex* base{m_node.chainman->m_best_header->GetAncestor(base_height)};
        return m_node.special_tx->m_mnhfman->GetForBlock(base);
    };
    const CMNHFManager::Signals expected{{UNKNOWN_BIT, base_height}};
    BOOST_REQUIRE(raw_at_base(m_node.evodb->ViewFor(EvoDbIdentity::NORMAL)) == expected);
    PinTip();
    {
        LOCK(::cs_main);
        BOOST_REQUIRE(BuildEvoSnapshotAtTip()->mnhf_signals == expected);
    }
    const auto staged = [&](const EvoViewToken& view) {
        LOCK(::cs_main);
        const EvoViewScope scope{view};
        return m_node.special_tx->m_mnhfman->GetSignalsStage(m_node.chainman->m_best_header->GetAncestor(base_height));
    };
    const auto expected_staged{staged(m_node.evodb->ViewFor(EvoDbIdentity::NORMAL))};
    const fs::path snapshot{DumpEvoSnapshot()};
    const int tip_height{base_height + 3};
    MineTo(tip_height);
    ForgetBlocks();

    BOOST_REQUIRE(LoadEvoSnapshot(snapshot));
    Chainstate& active{m_node.chainman->ActiveChainstate()};
    BOOST_CHECK(raw_at_base(active.EvoView()) == expected);
    BOOST_CHECK(staged(active.EvoView()) == expected_staged);
    BOOST_REQUIRE(Feed(active, base_height + 1, tip_height).IsValid());
    Feed(Background(), 1, base_height);
    BOOST_REQUIRE(WITH_LOCK(::cs_main, return m_node.chainman->IsSnapshotValidated()));
    Restart();
    BOOST_REQUIRE(!m_node.chainman->IsSnapshotActive());
    BOOST_CHECK(raw_at_base(m_node.evodb->ViewFor(EvoDbIdentity::NORMAL)) == expected);
}

//! A rejected snapshot leaves its mined commitments behind: NORMAL, once it
//! connects the same blocks, derives and stores them itself.
BOOST_FIXTURE_TEST_CASE(chainstatemanager_evo_snapshot_real_quorums_rejected, EvoQuorumSnapshotTestSetup)
{
    constexpr auto ROTATING{Consensus::LLMQType::LLMQ_TEST_DIP0024};
    constexpr int NEXT_CYCLE{QUORUM_BASE_HEIGHT + 1};
    const int tip_height{NEXT_CYCLE + 24};
    PinTip();
    const fs::path snapshot{DumpEvoSnapshot()};
    MineTo(tip_height);
    ForgetBlocks();
    BOOST_REQUIRE(LoadEvoSnapshot(snapshot));
    BOOST_REQUIRE(Feed(m_node.chainman->ActiveChainstate(), QUORUM_BASE_HEIGHT + 1, tip_height).IsValid());
    const uint256 next_cycle_hash{m_blocks.at(NEXT_CYCLE)->GetHash()};
    const auto mined_in = [&](const EvoViewToken& view) {
        LOCK(::cs_main);
        const EvoViewScope scope{view};
        return m_node.llmq_ctx->quorum_block_processor->GetMinedCommitment(ROTATING, next_cycle_hash);
    };
    const auto snapshot_commitment{mined_in(m_node.chainman->ActiveChainstate().EvoView())};
    BOOST_REQUIRE(!snapshot_commitment.second.IsNull());
    BOOST_CHECK(mined_in(Background().EvoView()).second.IsNull());

    BOOST_REQUIRE(m_node.chainman->HandleSnapshotStateMismatch("test rejection", [](bilingual_str) {}));
    Restart();
    BOOST_REQUIRE(!m_node.chainman->IsSnapshotActive());
    BOOST_CHECK(mined_in(m_node.evodb->ViewFor(EvoDbIdentity::NORMAL)).second.IsNull());
    BOOST_REQUIRE(Feed(m_node.chainman->ActiveChainstate(), 1, tip_height).IsValid());
    const auto rebuilt{mined_in(m_node.evodb->ViewFor(EvoDbIdentity::NORMAL))};
    BOOST_CHECK(SerializeHash(rebuilt) == SerializeHash(snapshot_commitment));
}

//! Safety commitments are carried but their signatures are not checked at
//! load, and their mined block only has to sit in the quorum's mining window:
//! a wrong one of each passes every earlier gate and is caught by completion.
void CheckRejectedSafetyCommitment(EvoQuorumSnapshotTestSetup& setup,
                                   const std::function<void(MinedQuorumCommitmentRef)>& mutate)
{
    setup.PinTip();
    CheckRejectedAtCompletion(setup, [&](auto& evo_section) {
        auto& data{EvoSnapshotChainSetupType(evo_section, Consensus::LLMQType::LLMQ_TEST)};
        BOOST_REQUIRE(!data.safety_commitments.empty());
        mutate(data.safety_commitments.front());
    });
}

BOOST_FIXTURE_TEST_CASE(chainstatemanager_evo_snapshot_completion_rebuild_safety_public_key, EvoQuorumSnapshotTestSetup)
{
    CheckRejectedSafetyCommitment(*this, [](auto& entry) {
        CBLSSecretKey other;
        other.MakeNewKey();
        entry.commitment.quorumPublicKey = other.GetPublicKey();
    });
}

BOOST_FIXTURE_TEST_CASE(chainstatemanager_evo_snapshot_completion_rebuild_safety_signature, EvoQuorumSnapshotTestSetup)
{
    CheckRejectedSafetyCommitment(*this, [](auto& entry) {
        CBLSSecretKey other;
        other.MakeNewKey();
        entry.commitment.quorumSig = other.Sign(uint256::ONE, /*specificLegacyScheme=*/false);
    });
}

BOOST_FIXTURE_TEST_CASE(chainstatemanager_evo_snapshot_completion_rebuild_safety_mined_block, EvoQuorumSnapshotTestSetup)
{
    CheckRejectedSafetyCommitment(*this, [this](auto& entry) {
        LOCK(::cs_main);
        const CBlockIndex* mined{m_node.chainman->m_blockman.LookupBlockIndex(entry.mined_block_hash)};
        BOOST_REQUIRE(mined);
        // Another block of the same mining window.
        entry.mined_block_hash = m_node.chainman->ActiveChain()[mined->nHeight + 1]->GetBlockHash();
    });
}

BOOST_FIXTURE_TEST_CASE(chainstatemanager_evo_snapshot_completion_real_quorums_honest, EvoQuorumSnapshotTestSetup)
{
    PinTip();
    BOOST_REQUIRE(ActivateEvoSnapshot());
    mineBlocks(2);
    BOOST_CHECK_EQUAL(Complete(), SnapshotCompletionResult::SUCCESS);
}

//! A real quorum-signed ChainLock carried in the base coinbase, end to end: a
//! node without the base block's body loads the snapshot, and the block
//! after the base, which builds on that ChainLock, connects. The rejection
//! rows on a carried coinbase are evo_snapshot_tests'.
BOOST_FIXTURE_TEST_CASE(chainstatemanager_evo_snapshot_base_chainlock_rules, EvoQuorumSnapshotTestSetup)
{
    PinTip();
    const fs::path snapshot{DumpEvoSnapshot()};
    ForgetBlocks();
    BOOST_REQUIRE(LoadEvoSnapshot(snapshot));
    // This node knows no ChainLock of its own, so blocks build on the base's.
    m_node.chainlocks->ResetChainlock();
    Chainstate& active{m_node.chainman->ActiveChainstate()};
    const CBlock next{CreateBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()), active)};
    BOOST_REQUIRE(GetTxPayload<CCbTx>(*next.vtx[0])->bestCLSignature.Get().IsValid());
    BOOST_CHECK(m_node.chainman->ProcessNewBlock(std::make_shared<const CBlock>(next), /*force_processing=*/true,
                                                 /*new_block=*/nullptr));
    BOOST_CHECK_EQUAL(Height(active), QUORUM_BASE_HEIGHT + 1);
}

//! Background validation connects far past the retention horizon of the
//! MN-list cache while NORMAL receives no tip notifications: the cache still
//! drops the lists no quorum can use any more, and current lookups work.
BOOST_FIXTURE_TEST_CASE(chainstatemanager_evo_snapshot_bounded_normal_caches, EvoSnapshotTestSetup)
{
    const int base_height{459};
    MineTo(base_height);
    PinTip();
    const fs::path snapshot{DumpEvoSnapshot()};
    ForgetBlocks();
    BOOST_REQUIRE(LoadEvoSnapshot(snapshot));
    Chainstate& background{Background()};
    const int processed_height{base_height - 1};
    BOOST_REQUIRE(Feed(background, 1, processed_height).IsValid());
    // Lists a live quorum may still use stay cached for this many blocks, and
    // cleanup runs once more than 64 lists are cached.
    int quorum_window{0};
    for (const auto& params : Params().GetConsensus().llmqs) {
        quorum_window = std::max(quorum_window, params.dkgInterval * (params.keepOldConnections + 1));
    }
    BOOST_REQUIRE_LT(DIP3_HEIGHT + quorum_window + 64, processed_height);
    LOCK(::cs_main);
    const EvoViewScope normal{background.EvoView()};
    const auto heights{m_node.dmnman->CachedListHeightsForTesting()};
    BOOST_CHECK(!heights.empty());
    for (const int height : heights) {
        BOOST_CHECK_MESSAGE(height + quorum_window + 64 >= processed_height,
                            "list at height " << height << " outlived its quorums");
    }
    BOOST_CHECK_EQUAL(m_node.dmnman->GetListForBlock(background.m_chain.Tip()).GetCounts().total(), size_t{MN_COUNT});
}

//! A carried record missing on the way back is a local failure for the callers
//! that disconnect blocks above the base, too: a reorg and ChainLock
//! enforcement of a competing block both stall where they are, mark nothing
//! invalid or conflicting, and succeed once the record is back.
BOOST_FIXTURE_TEST_CASE(chainstatemanager_evo_snapshot_stalls_on_the_way_back, EvoSnapshotTestSetup)
{
    const fs::path snapshot{DumpEvoSnapshot()};
    MineTo(BASE_HEIGHT + 3);
    ForgetBlocks();
    BOOST_REQUIRE(LoadEvoSnapshot(snapshot));
    Chainstate& active{m_node.chainman->ActiveChainstate()};
    BOOST_REQUIRE(Feed(active, BASE_HEIGHT + 1, BASE_HEIGHT + 3).IsValid());

    // A competing block at B+1, as a ChainLock may later select.
    auto competing{std::make_shared<CBlock>(*m_blocks.at(BASE_HEIGHT + 1))};
    {
        CMutableTransaction coinbase{*competing->vtx[0]};
        coinbase.vout[0].scriptPubKey = CScript{} << OP_TRUE;
        competing->vtx[0] = MakeTransactionRef(std::move(coinbase));
        competing->hashMerkleRoot = BlockMerkleRoot(*competing);
        while (!CheckProofOfWork(competing->GetHash(), competing->nBits, m_node.chainman->GetConsensus()))
            ++competing->nNonce;
        LOCK(::cs_main);
        BlockValidationState state;
        CBlockIndex* index{nullptr};
        BOOST_REQUIRE(m_node.chainman->AcceptBlock(competing, state, &index, /*fRequested=*/true, nullptr, nullptr));
    }
    const auto index_of = [&](const uint256& hash) {
        return WITH_LOCK(::cs_main, return m_node.chainman->m_blockman.LookupBlockIndex(hash));
    };
    CBlockIndex* first{index_of(m_blocks.at(BASE_HEIGHT + 1)->GetHash())};
    CBlockIndex* competing_index{index_of(competing->GetHash())};

    // Every list above the base derives from the carried base list. The key
    // mirrors DB_LIST_SNAPSHOT in evo/deterministicmns.cpp (file-local there).
    const auto base_list_key{std::make_pair(std::string{"dmn_S3"}, m_blocks.at(BASE_HEIGHT)->GetHash())};
    CDeterministicMNList base_list;
    const auto with_snapshot_tx = [&](const std::function<void()>& change) {
        LOCK(::cs_main);
        const EvoViewScope view{active.EvoView()};
        {
            auto tx{m_node.evodb->BeginTransaction(EvoDbIdentity::SNAPSHOT)};
            change();
            tx->Commit();
        }
        BOOST_REQUIRE(m_node.evodb->CommitRootTransaction(EvoDbIdentity::SNAPSHOT, /*sync=*/true));
        DropEvoCaches();
    };
    with_snapshot_tx([&] {
        BOOST_REQUIRE(m_node.evodb->Read(base_list_key, base_list));
        m_node.evodb->Erase(base_list_key);
    });
    const auto unchanged = [&] {
        LOCK(::cs_main);
        return active.m_chain.Height() == BASE_HEIGHT + 3 &&
               !(first->nStatus & (BLOCK_FAILED_MASK | BLOCK_CONFLICT_CHAINLOCK)) &&
               !(competing_index->nStatus & BLOCK_FAILED_MASK);
    };

    // A stall, as the InstantSend conflict path requires before it defers.
    const auto stall_warned = [] {
        return GetWarnings(/*verbose=*/true).original.find("Snapshot state unavailable") != std::string::npos;
    };
    BOOST_REQUIRE(!stall_warned());
    {
        BlockValidationState state;
        BOOST_CHECK(!active.InvalidateBlock(state, first));
        BOOST_CHECK(IsLocalEvoStateError(state));
        BOOST_CHECK(unchanged());
        BOOST_CHECK(stall_warned());
    }
    {
        BlockValidationState state;
        active.EnforceBlock(state, competing_index);
        BOOST_CHECK(IsLocalEvoStateError(state));
        BOOST_CHECK(unchanged());
    }
    BOOST_CHECK(!ShutdownRequested());

    // With the record back, and after the restart that a stalled block waits
    // for, enforcement moves the chain to the competing block.
    with_snapshot_tx([&] { m_node.evodb->Write(base_list_key, base_list); });
    Restart();
    Chainstate& restarted{m_node.chainman->ActiveChainstate()};
    BOOST_REQUIRE(restarted.m_from_snapshot_blockhash);
    competing_index = index_of(competing->GetHash());
    {
        BlockValidationState state;
        restarted.EnforceBlock(state, competing_index);
        BOOST_CHECK(state.IsValid());
        BOOST_REQUIRE(restarted.ActivateBestChain(state));
    }
    BOOST_CHECK(WITH_LOCK(::cs_main, return restarted.m_chain.Contains(competing_index)));
}

//! Every block of a period after the fixture's base signals a bit no
//! deployment uses, so on the synced node the bit is active two periods
//! later and NORMAL warns about unknown rules, its warning cache warm up to
//! that point. A node that got there by its own sync, still holding those
//! cached states, loads a snapshot at a later base without the bodies below
//! it. The snapshot chainstate does not reuse NORMAL's states: its own walk
//! needs the versions expected of the signalling blocks, which its view
//! cannot compute without their bodies, so it goes without the warning.
BOOST_FIXTURE_TEST_CASE(chainstatemanager_evo_snapshot_warning_cache, EvoSnapshotTestSetup)
{
    constexpr int UNKNOWN_BIT{5};
    const auto& consensus{Params().GetConsensus()};
    const int window{static_cast<int>(consensus.nMinerConfirmationWindow)};
    const int signal_start{(BASE_HEIGHT / window + 1) * window};
    MineTo(signal_start - 1);
    while (Height(m_node.chainman->ActiveChainstate()) < signal_start + window - 1) {
        CBlock block{CreateBlock({}, GetScriptForRawPubKey(coinbaseKey.GetPubKey()), m_node.chainman->ActiveChainstate())};
        block.nVersion |= VERSIONBITS_TOP_BITS | (1 << UNKNOWN_BIT);
        while (!CheckProofOfWork(block.GetHash(), block.nBits, consensus))
            ++block.nNonce;
        BOOST_REQUIRE(m_node.chainman->ProcessNewBlock(std::make_shared<const CBlock>(block), /*force_processing=*/true,
                                                       /*new_block=*/nullptr));
        SetMockTime(GetTime() + 1);
    }
    const auto warned = [&] {
        return GetWarnings(/*verbose=*/true).original.find(strprintf("versionbit %d", UNKNOWN_BIT)) != std::string::npos;
    };
    // The tip's state is the next block's.
    const int active_height{signal_start + 2 * window};
    MineTo(active_height - 2);
    BOOST_REQUIRE(!warned());
    MineTo(active_height);
    BOOST_REQUIRE(warned());

    PinTip();
    const fs::path snapshot{DumpEvoSnapshot()};
    MineTo(active_height + 1);
    ForgetBlocks(/*keep_caches=*/true);
    SetMiscWarning(bilingual_str{});
    BOOST_REQUIRE(LoadEvoSnapshot(snapshot));
    Chainstate& active{m_node.chainman->ActiveChainstate()};
    BOOST_REQUIRE(Feed(active, active_height + 1, active_height + 1).IsValid());
    BOOST_CHECK_EQUAL(Height(active), active_height + 1);
    BOOST_CHECK(!warned());
}

BOOST_AUTO_TEST_SUITE_END()
