// Copyright (c) 2019-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
//
#include <chainparams.h>
#include <consensus/merkle.h>
#include <consensus/validation.h>
#include <evo/cbtx.h>
#include <evo/deterministicmns.h>
#include <evo/mnhftx.h>
#include <evo/snapshot_config.h>
#include <evo/specialtx.h>
#include <evo/specialtxman.h>
#include <governance/superblock.h>
#include <instantsend/net_instantsend.h>
#include <kernel/disconnected_transactions.h>
#include <logging.h>
#include <node/chainstate.h>
#include <node/utxo_snapshot.h>
#include <pow.h>
#include <random.h>
#include <rpc/blockchain.h>
#include <shutdown.h>
#include <sync.h>
#include <test/util/chainstate.h>
#include <test/util/logging.h>
#include <test/util/random.h>
#include <test/util/setup_common.h>
#include <timedata.h>
#include <uint256.h>
#include <util/translation.h>
#include <validation.h>
#include <validationinterface.h>
#include <warnings.h>

#include <evo/evodb.h>
#include <llmq/blockprocessor.h>
#include <llmq/commitment.h>
#include <llmq/context.h>
#include <llmq/quorumsman.h>
#include <llmq/signing.h>
#include <llmq/signing_shares.h>
#include <llmq/snapshot.h>
#include <llmq/utils.h>
#include <node/blockstorage.h>

#include <tinyformat.h>

#include <optional>
#include <vector>

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
        ChainstateManager& chainman = *Assert(m_node.chainman);

        BOOST_TEST_MESSAGE("Simulating node restart");
        {
            LOCK(::cs_main);
            if (flush_chainstates) {
                for (Chainstate* cs : chainman.GetAll()) {
                    cs->ForceFlushStateToDisk();
                }
            }
            m_node.special_tx.reset();
            m_node.llmq_ctx.reset();
            chainman.ResetChainstates();
            BOOST_CHECK_EQUAL(chainman.GetAll().size(), 0);
            const ChainstateManager::Options chainman_opts{
                .chainparams = ::Params(),
                .datadir = m_args.GetDataDirNet(),
                .superblock_status = [&sbman = *m_node.sbman](const CDeterministicMNList& mn_list, int height) { return sbman.GetStatus(mn_list, height); },
                .mn_sync = m_node.mn_sync.get(),
            };
            // For robustness, ensure the old manager is destroyed before creating a
            // new one.
            m_node.chainman.reset();
            m_node.chainman.reset(new ChainstateManager(chainman_opts));
        }
        return *Assert(m_node.chainman);
    }

    //! Load the chainstates like startup does, reporting failure instead of
    //! asserting success.
    node::ChainstateLoadResult TryLoadChainstate()
    {
        MakeDashChainContexts(/*llmq_dbs_wipe=*/false);
        return node::LoadChainstate(*Assert(m_node.chainman), m_cache_sizes, ChainstateLoadOptionsForTest(),
                                    *m_node.evodb, *m_node.dmnman, *m_node.special_tx);
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

//! Test basic snapshot activation.
BOOST_FIXTURE_TEST_CASE(chainstatemanager_activate_snapshot, SnapshotTestSetup)
{
    auto [background_chainstate, snapshot_chainstate] = this->SetupSnapshot();
    ChainstateManager& chainman = *Assert(m_node.chainman);

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
    // Connecting it announces the stall as a local error, which blames no peer.
    const auto checked{std::make_shared<BlockCheckedCatcher>(block->GetHash())};
    RegisterSharedValidationInterface(checked);
    BOOST_CHECK(!chainman.ProcessNewBlock(block, /*force_processing=*/true, /*new_block=*/nullptr));
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
    SetMiscWarning(Untranslated(""));
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
    DisconnectedBlockTransactions unused_pool{MAX_DISCONNECTED_TX_POOL_SIZE * 1000};
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
        BOOST_CHECK(qblockman.ProcessBlock(*background_chainstate, block, snapshot_mined_block, state,
                                           /*fJustCheck=*/true, /*fBLSChecks=*/false));
    }
}

BOOST_FIXTURE_TEST_CASE(chainstatemanager_evodb_snapshot_only_flush_restart, SnapshotTestSetup)
{
    auto chainstates = this->SetupSnapshot();
    Chainstate* background_chainstate = std::get<0>(chainstates);
    ChainstateManager& chainman = *Assert(m_node.chainman);

    // Keep this M2 marker-independence test below completion height; #25740 now
    // completes and cleans up immediately on restart when background is at base.
    DisconnectedBlockTransactions unused_pool{MAX_DISCONNECTED_TX_POOL_SIZE * 1000};
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
    const uint256 incorrect_hash{SerializeHash(incorrect_base_list)};
    uint256 captured_block;
    uint256 captured_hash;
    BOOST_REQUIRE(m_node.evodb->ReadBackgroundMNListHash(captured_block, captured_hash));
    BOOST_CHECK_EQUAL(captured_block, base_index->GetBlockHash());
    BOOST_CHECK_NE(captured_hash, incorrect_hash);
    m_node.dmnman->SetListForBlockForTesting(incorrect_base_list);

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
        BOOST_CHECK(error.original.find("consensus configuration changed") != std::string::npos);
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

    const auto refused_under = [&](int& field) {
        this->SimulateNodeRestart();
        const int original{field};
        ++field;
        const auto [status, error] = this->TryLoadChainstate();
        field = original;
        BOOST_CHECK(status == node::ChainstateLoadStatus::FAILURE);
        BOOST_CHECK(error.original.find("consensus configuration changed") != std::string::npos);
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

BOOST_AUTO_TEST_SUITE_END()
