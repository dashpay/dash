// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2021 The Bitcoin Core developers
// Copyright (c) 2014-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/miner.h>

#include <chain.h>
#include <chainparams.h>
#include <consensus/amount.h>
#include <consensus/consensus.h>
#include <consensus/merkle.h>
#include <consensus/tx_verify.h>
#include <consensus/validation.h>
#include <deploymentstatus.h>
#include <node/context.h>
#include <policy/feerate.h>
#include <policy/policy.h>
#include <pow.h>
#include <primitives/transaction.h>
#include <timedata.h>
#include <util/moneystr.h>
#include <util/system.h>
#include <validation.h>

#include <chainlock/chainlock.h>
#include <chainlock/handler.h>
#include <evo/specialtx.h>
#include <evo/cbtx.h>
#include <evo/chainhelper.h>
#include <evo/creditpool.h>
#include <evo/mnhftx.h>
#include <evo/deterministicmns.h>
#include <evo/simplifiedmns.h>
#include <evo/specialtxman.h>
#include <governance/governance.h>
#include <llmq/blockprocessor.h>
#include <llmq/context.h>
#include <llmq/options.h>
#include <llmq/snapshot.h>
#include <masternode/payments.h>

#include <algorithm>
#include <utility>

namespace node {
int64_t UpdateTime(CBlockHeader* pblock, const Consensus::Params& consensusParams, const CBlockIndex* pindexPrev)
{
    int64_t nOldTime = pblock->nTime;
    int64_t nNewTime{std::max<int64_t>(pindexPrev->GetMedianTimePast() + 1, TicksSinceEpoch<std::chrono::seconds>(NodeClock::now()))};

    if (nOldTime < nNewTime) {
        pblock->nTime = nNewTime;
    }

    // Updating time can change work required on testnet:
    if (consensusParams.fPowAllowMinDifficultyBlocks) {
        pblock->nBits = GetNextWorkRequired(pindexPrev, pblock, consensusParams);
    }

    return nNewTime - nOldTime;
}

static BlockAssembler::Options ClampOptions(BlockAssembler::Options options)
{
    options.nBlockMaxSize = std::clamp<size_t>(options.nBlockMaxSize, 1000, DEFAULT_BLOCK_MAX_SIZE);
    return options;
}

BlockAssembler::BlockAssembler(Chainstate& chainstate, const NodeContext& node, const CTxMemPool* mempool, const Options& options) :
      m_chain_helper(chainstate.ChainHelper()),
      m_chainstate{chainstate},
      m_evoDb(*Assert(node.evodb)),
      m_chainlocks(*Assert(node.chainlocks)),
      m_clhandler(*Assert(node.clhandler)),
      chainparams(chainstate.m_chainman.GetParams()),
      m_mempool{mempool},
      m_quorum_block_processor(*Assert(Assert(node.llmq_ctx)->quorum_block_processor)),
      m_options{ClampOptions(options)}
{
}

void ApplyArgsManOptions(const ArgsManager& args, BlockAssembler::Options& options)
{
    // Block resource limits
    options.nBlockMaxSize  = args.GetIntArg("-blockmaxsize", options.nBlockMaxSize);
    if (const auto blockmintxfee{args.GetArg("-blockmintxfee")}) {
        if (const auto parsed{ParseMoney(*blockmintxfee)}) options.blockMinFeeRate = CFeeRate{*parsed};
    }
}
static BlockAssembler::Options ConfiguredOptions()
{
    BlockAssembler::Options options;
    ApplyArgsManOptions(gArgs, options);
    return options;
}

BlockAssembler::BlockAssembler(Chainstate& chainstate, const NodeContext& node, const CTxMemPool* mempool)
    : BlockAssembler(chainstate, node, mempool, ConfiguredOptions()) {}

void BlockAssembler::resetBlock()
{
    inBlock.clear();

    // Reserve space for coinbase tx
    nBlockSize = 1000;
    nBlockSigOps = 100;

    // These counters do not include coinbase tx
    nBlockTx = 0;
    nFees = 0;
}

// Helper to calculate best chainlock
static bool CalcCbTxBestChainlock(const chainlock::Chainlocks& chainlocks, const CBlockIndex* pindexPrev,
                                  uint32_t& bestCLHeightDiff, CBLSSignature& bestCLSignature)
{
    auto best_clsig = chainlocks.GetBestChainLock();
    if (best_clsig.getHeight() < Params().GetConsensus().DeploymentHeight(Consensus::DEPLOYMENT_V19)) {
        // We don't want legacy BLS ChainLocks in CbTx (can happen on regtest/devenets)
        best_clsig = chainlock::ChainLockSig{};
    }
    if (best_clsig.getHeight() == pindexPrev->nHeight) {
        // Our best CL is the newest one possible
        bestCLHeightDiff = 0;
        bestCLSignature = best_clsig.getSig();
        return true;
    }

    auto prevBlockCoinbaseChainlock = GetNonNullCoinbaseChainlock(pindexPrev);
    if (prevBlockCoinbaseChainlock.has_value()) {
        // Previous block Coinbase contains a non-null CL: We must insert the same sig or a better (newest) one
        if (best_clsig.IsNull()) {
            // We don't know any CL, therefore inserting the CL of the previous block
            bestCLHeightDiff = prevBlockCoinbaseChainlock->second + 1;
            bestCLSignature = prevBlockCoinbaseChainlock->first;
            return true;
        }

        // We check if our best CL is newer than the one from previous block Coinbase
        int curCLHeight = best_clsig.getHeight();
        int prevCLHeight = pindexPrev->nHeight - static_cast<int>(prevBlockCoinbaseChainlock->second) - 1;
        if (curCLHeight < prevCLHeight) {
            // Our best CL isn't newer: inserting CL from previous block
            bestCLHeightDiff = prevBlockCoinbaseChainlock->second + 1;
            bestCLSignature = prevBlockCoinbaseChainlock->first;
        }
        else {
            // Our best CL is newer
            bestCLHeightDiff = pindexPrev->nHeight - best_clsig.getHeight();
            bestCLSignature = best_clsig.getSig();
        }

        return true;
    }
    else {
        // Previous block Coinbase has no CL. We can either insert null or any valid CL
        if (best_clsig.IsNull()) {
            // We don't know any CL, therefore inserting a null CL
            bestCLHeightDiff = 0;
            bestCLSignature.Reset();
            return false;
        }

        // Inserting our best CL
        bestCLHeightDiff = pindexPrev->nHeight - best_clsig.getHeight();
        bestCLSignature = best_clsig.getSig();

        return true;
    }
}


std::unique_ptr<CBlockTemplate> BlockAssembler::CreateNewBlock(const CScript& scriptPubKeyIn)
{
    const auto time_start{SteadyClock::now()};

    resetBlock();

    pblocktemplate.reset(new CBlockTemplate());

    if (!pblocktemplate.get()) {
        return nullptr;
    }
    CBlock* const pblock = &pblocktemplate->block; // pointer for convenience

    // Add dummy coinbase tx as first transaction
    pblock->vtx.emplace_back();
    pblocktemplate->vTxFees.push_back(-1); // updated at end
    pblocktemplate->vTxSigOps.push_back(-1); // updated at end

    LOCK(::cs_main);
    CBlockIndex* pindexPrev = m_chainstate.m_chain.Tip();
    assert(pindexPrev != nullptr);
    nHeight = pindexPrev->nHeight + 1;

    const bool fDIP0001Active_context{DeploymentActiveAfter(pindexPrev, chainparams.GetConsensus(), Consensus::DEPLOYMENT_DIP0001)};
    const bool fDIP0003Active_context{DeploymentActiveAfter(pindexPrev, chainparams.GetConsensus(), Consensus::DEPLOYMENT_DIP0003)};
    const bool fDIP0008Active_context{DeploymentActiveAfter(pindexPrev, chainparams.GetConsensus(), Consensus::DEPLOYMENT_DIP0008)};
    const bool fV20Active_context{DeploymentActiveAfter(pindexPrev, chainparams.GetConsensus(), Consensus::DEPLOYMENT_V20)};
    const bool fV24Active_context{DeploymentActiveAfter(pindexPrev, m_chainstate.m_chainman, Consensus::DEPLOYMENT_V24)};

    // Limit size to between 1K and MaxBlockSize()-1K for sanity:
    m_options.nBlockMaxSize = std::max<unsigned int>(1000, std::min<unsigned int>(MaxBlockSize(fDIP0001Active_context) - 1000, m_options.nBlockMaxSize));
    m_options.nBlockMaxSigOps = MaxBlockSigOps(fDIP0001Active_context);

    pblock->nVersion = m_chainstate.m_chainman.m_versionbitscache.ComputeBlockVersion(pindexPrev, chainparams.GetConsensus());
    // Non-mainnet only: allow overriding block.nVersion with
    // -blockversion=N to test forking scenarios
    if (chainparams.NetworkIDString() != CBaseChainParams::MAIN) {
        pblock->nVersion = gArgs.GetIntArg("-blockversion", pblock->nVersion);
    }

    pblock->nTime = TicksSinceEpoch<std::chrono::seconds>(NodeClock::now());
    m_lock_time_cutoff = pindexPrev->GetMedianTimePast();

    if (fDIP0003Active_context) {
        for (const Consensus::LLMQParams& params : llmq::GetEnabledQuorumParams(m_chainstate.m_chainman, pindexPrev)) {
            std::vector<CTransactionRef> vqcTx;
            if (m_quorum_block_processor.GetMineableCommitmentsTx(params,
                                                                  nHeight,
                                                                  vqcTx)) {
                for (const auto& qcTx : vqcTx) {
                    pblock->vtx.emplace_back(qcTx);
                    pblocktemplate->vTxFees.emplace_back(0);
                    pblocktemplate->vTxSigOps.emplace_back(0);
                    nBlockSize += qcTx->GetTotalSize();
                    ++nBlockTx;
                }
            }
        }
    }

    int nPackagesSelected = 0;
    int nDescendantsUpdated = 0;
    if (m_mempool) {
        LOCK(m_mempool->cs);
        addPackageTxs(*m_mempool, nPackagesSelected, nDescendantsUpdated, pindexPrev);
    }

    const auto time_1{SteadyClock::now()};

    m_last_block_num_txs = nBlockTx;
    m_last_block_size = nBlockSize;
    LogPrintf("CreateNewBlock(): total size %u txs: %u fees: %ld sigops %d\n", nBlockSize, nBlockTx, nFees, nBlockSigOps);

    // Create coinbase transaction.
    CMutableTransaction coinbaseTx;
    coinbaseTx.vin.resize(1);
    coinbaseTx.vin[0].prevout.SetNull();
    coinbaseTx.vout.resize(1);
    coinbaseTx.vout[0].scriptPubKey = scriptPubKeyIn;

    // NOTE: unlike in bitcoin, we need to pass PREVIOUS block height here
    CAmount blockSubsidy = GetBlockSubsidyInner(pindexPrev->nBits, pindexPrev->nHeight, chainparams.GetConsensus(), fV20Active_context);
    CAmount blockReward = blockSubsidy + nFees;

    // Compute regular coinbase transaction.
    coinbaseTx.vout[0].nValue = blockReward;

    if (!fDIP0003Active_context) {
        coinbaseTx.vin[0].scriptSig = CScript() << nHeight << OP_0;
    } else {
        coinbaseTx.vin[0].scriptSig = CScript() << OP_RETURN;

        coinbaseTx.nVersion = 3;
        coinbaseTx.nType = TRANSACTION_COINBASE;

        CCbTx cbTx;

        if (fV24Active_context) {
            cbTx.nVersion = CCbTx::Version::MERKLE_ROOT_ASSETUNLOCKS;
        } else if (fV20Active_context) {
            cbTx.nVersion = CCbTx::Version::CLSIG_AND_BALANCE;
        } else if (fDIP0008Active_context) {
            cbTx.nVersion = CCbTx::Version::MERKLE_ROOT_QUORUMS;
        } else {
            cbTx.nVersion = CCbTx::Version::MERKLE_ROOT_MNLIST;
        }

        cbTx.nHeight = nHeight;

        BlockValidationState state;
        CDeterministicMNList mn_list;
        if (!m_chain_helper.special_tx->BuildNewListFromBlock(*pblock, pindexPrev, m_chainstate.CoinsTip(), true, state, mn_list)) {
            throw std::runtime_error(strprintf("%s: BuildNewListFromBlock failed: %s", __func__, state.ToString()));
        }
        if (!CalcCbTxMerkleRootMNList(cbTx.merkleRootMNList, mn_list.to_sml(), state)) {
            throw std::runtime_error(strprintf("%s: CalcCbTxMerkleRootMNList failed: %s", __func__, state.ToString()));
        }
        if (fDIP0008Active_context) {
            if (!CalcCbTxMerkleRootQuorums(*pblock, pindexPrev, m_quorum_block_processor, cbTx.merkleRootQuorums, state)) {
                throw std::runtime_error(strprintf("%s: CalcCbTxMerkleRootQuorums failed: %s", __func__, state.ToString()));
            }
            if (fV20Active_context) {
                if (CalcCbTxBestChainlock(m_chainlocks, pindexPrev, cbTx.bestCLHeightDiff, cbTx.bestCLSignature)) {
                    LogPrintf("CreateNewBlock() h[%d] CbTx bestCLHeightDiff[%d] CLSig[%s]\n", nHeight, cbTx.bestCLHeightDiff, cbTx.bestCLSignature.ToString());
                } else {
                    // not an error
                    LogPrintf("CreateNewBlock() h[%d] CbTx failed to find best CL. Inserting null CL\n", nHeight);
                }
                BlockValidationState state;
                const auto creditPoolDiff = GetCreditPoolDiffForBlock(*m_chain_helper.credit_pool_manager, *pblock, pindexPrev, chainparams.GetConsensus(), blockSubsidy, state);
                if (creditPoolDiff == std::nullopt) {
                    throw std::runtime_error(strprintf("%s: GetCreditPoolDiffForBlock failed: %s", __func__, state.ToString()));
                }

                cbTx.creditPoolBalance = creditPoolDiff->GetTotalLocked();

                if (fV24Active_context) {
                    cbTx.merkleRootAssetUnlocks = CalcCbTxMerkleRootAssetUnlocks(*pblock);
                }
            }
        }

        SetTxPayload(coinbaseTx, cbTx);
    }

    // Update coinbase transaction with additional info about masternode and governance payments,
    // get some info back to pass to getblocktemplate
    const MnRewardEra mn_reward_era{GetMnRewardEraAfter(pindexPrev, m_chainstate.m_chainman)};
    m_chain_helper.mn_payments->FillBlockPayments(coinbaseTx, pindexPrev, blockSubsidy, nFees, mn_reward_era, pblocktemplate->voutMasternodePayments, pblocktemplate->voutSuperblockPayments);

    pblock->vtx[0] = MakeTransactionRef(std::move(coinbaseTx));
    pblocktemplate->vTxFees[0] = -nFees;

    // Fill in header
    pblock->hashPrevBlock  = pindexPrev->GetBlockHash();
    UpdateTime(pblock, chainparams.GetConsensus(), pindexPrev);
    pblock->nBits          = GetNextWorkRequired(pindexPrev, pblock, chainparams.GetConsensus());
    pblock->nNonce         = 0;
    pblocktemplate->nPrevBits = pindexPrev->nBits;
    pblocktemplate->vTxSigOps[0] = GetLegacySigOpCount(*pblock->vtx[0]);

    BlockValidationState state;
    if (m_options.test_block_validity && !TestBlockValidity(state, m_chainlocks, m_evoDb, chainparams, m_chainstate, *pblock, pindexPrev, /*fCheckPOW=*/false, /*fCheckMerkleRoot=*/false)) {
        throw std::runtime_error(strprintf("%s: TestBlockValidity failed: %s", __func__, state.ToString()));
    }
    const auto time_2{SteadyClock::now()};

    LogPrint(BCLog::BENCHMARK, "CreateNewBlock() packages: %.2fms (%d packages, %d updated descendants), validity: %.2fms (total %.2fms)\n",
             Ticks<MillisecondsDouble>(time_1 - time_start), nPackagesSelected, nDescendantsUpdated,
             Ticks<MillisecondsDouble>(time_2 - time_1),
             Ticks<MillisecondsDouble>(time_2 - time_start));

    return std::move(pblocktemplate);
}

void BlockAssembler::onlyUnconfirmed(CTxMemPool::setEntries& testSet)
{
    for (CTxMemPool::setEntries::iterator iit = testSet.begin(); iit != testSet.end(); ) {
        // Only test txs not already in the block
        if (inBlock.count(*iit)) {
            testSet.erase(iit++);
        } else {
            iit++;
        }
    }
}

bool BlockAssembler::TestPackage(uint64_t packageSize, unsigned int packageSigOps) const
{
    if (nBlockSize + packageSize >= m_options.nBlockMaxSize) {
        return false;
    }

    if (nBlockSigOps + packageSigOps >= m_options.nBlockMaxSigOps) {
        return false;
    }
    return true;
}

// Perform transaction-level checks before adding to block:
// - transaction finality (locktime)
// - safe TXs in regard to ChainLocks
bool BlockAssembler::TestPackageTransactions(const CTxMemPool::setEntries& package) const
{
    for (CTxMemPool::txiter it : package) {
        if (!IsFinalTx(it->GetTx(), nHeight, m_lock_time_cutoff)) {
            return false;
        }

        // A special transaction that was valid when it entered the mempool can be invalidated by
        // intervening state, most sharply by a fork activating a rule it violates, and nothing
        // evicts it. Selection otherwise trusts mempool validity, so the entry would be picked into
        // every template and TestBlockValidity would then reject the whole block, leaving an honest
        // miner unable to produce one at all. Recheck here, at package granularity: the caller adds
        // every member of a package that passes, so dropping one member while keeping its
        // descendants would itself yield an invalid template. check_sigs is off because signatures
        // were verified on entry and cannot have changed; note CheckMNHFTx() verifies its quorum
        // signature regardless, which is cheap enough here given how rare MNHF signals are.
        if (it->GetTx().IsSpecialTxVersion()) {
            TxValidationState tx_state;
            if (!m_chain_helper.special_tx->CheckSpecialTx(it->GetTx(), m_chainstate.m_chain.Tip(),
                                                           m_chainstate.CoinsTip(), /*check_sigs=*/false, tx_state)) {
                return false;
            }
        }

        const auto& txid = it->GetTx().GetHash();
        if (!m_chain_helper.IsInstantSendEnabled() || m_chain_helper.IsInstantSendLocked(txid)) {
            continue;
        }

        if (!it->GetTx().vin.empty() && !m_clhandler.IsTxSafeForMining(txid)) {
            return false;
        }
    }
    return true;
}

void BlockAssembler::AddToBlock(CTxMemPool::txiter iter)
{
    pblocktemplate->block.vtx.emplace_back(iter->GetSharedTx());
    pblocktemplate->vTxFees.push_back(iter->GetFee());
    pblocktemplate->vTxSigOps.push_back(iter->GetSigOpCount());
    nBlockSize += iter->GetTxSize();
    ++nBlockTx;
    nBlockSigOps += iter->GetSigOpCount();
    nFees += iter->GetFee();
    inBlock.insert(iter);

    bool fPrintPriority = gArgs.GetBoolArg("-printpriority", DEFAULT_PRINTPRIORITY);
    if (fPrintPriority) {
        LogPrintf("fee rate %s txid %s\n",
                  CFeeRate(iter->GetModifiedFee(), iter->GetTxSize()).ToString(),
                  iter->GetTx().GetHash().ToString());
    }
}

/** Add descendants of given transactions to mapModifiedTx with ancestor
 * state updated assuming given transactions are inBlock. Returns number
 * of updated descendants. */
static int UpdatePackagesForAdded(const CTxMemPool& mempool,
                                  const CTxMemPool::setEntries& alreadyAdded,
                                  indexed_modified_transaction_set& mapModifiedTx) EXCLUSIVE_LOCKS_REQUIRED(mempool.cs)
{
    AssertLockHeld(mempool.cs);

    int nDescendantsUpdated = 0;
    for (CTxMemPool::txiter it : alreadyAdded) {
        CTxMemPool::setEntries descendants;
        mempool.CalculateDescendants(it, descendants);
        // Insert all descendants (not yet in block) into the modified set
        for (CTxMemPool::txiter desc : descendants) {
            if (alreadyAdded.count(desc)) {
                continue;
            }
            ++nDescendantsUpdated;
            modtxiter mit = mapModifiedTx.find(desc);
            if (mit == mapModifiedTx.end()) {
                CTxMemPoolModifiedEntry modEntry(desc);
                mit = mapModifiedTx.insert(modEntry).first;
            }
            mapModifiedTx.modify(mit, update_for_parent_inclusion(it));
        }
    }
    return nDescendantsUpdated;
}

void BlockAssembler::SortForBlock(const CTxMemPool::setEntries& package, std::vector<CTxMemPool::txiter>& sortedEntries)
{
    // Sort package by ancestor count
    // If a transaction A depends on transaction B, then A's ancestor count
    // must be greater than B's.  So this is sufficient to validly order the
    // transactions for block inclusion.
    sortedEntries.clear();
    sortedEntries.insert(sortedEntries.begin(), package.begin(), package.end());
    std::sort(sortedEntries.begin(), sortedEntries.end(), CompareTxIterByAncestorCount());
}

// This transaction selection algorithm orders the mempool based
// on feerate of a transaction including all unconfirmed ancestors.
// Since we don't remove transactions from the mempool as we select them
// for block inclusion, we need an alternate method of updating the feerate
// of a transaction with its not-yet-selected ancestors as we go.
// This is accomplished by walking the in-mempool descendants of selected
// transactions and storing a temporary modified state in mapModifiedTxs.
// Each time through the loop, we compare the best transaction in
// mapModifiedTxs with the next transaction in the mempool to decide what
// transaction package to work on next.
void BlockAssembler::addPackageTxs(const CTxMemPool& mempool, int& nPackagesSelected, int& nDescendantsUpdated, const CBlockIndex* const pindexPrev)
{
    AssertLockHeld(mempool.cs);

    // This credit pool is used only to check withdrawal limits and to find
    // duplicates of indexes. There's used `BlockSubsidy` equaled to 0
    std::optional<CCreditPoolDiff> creditPoolDiff;
    if (DeploymentActiveAfter(pindexPrev, chainparams.GetConsensus(), Consensus::DEPLOYMENT_V20)) {
        CCreditPool creditPool = m_chain_helper.GetCreditPool(pindexPrev);
        creditPoolDiff.emplace(std::move(creditPool), pindexPrev, chainparams.GetConsensus(), 0);
    }

    // This map with signals is used only to find duplicates
    auto signals = m_chain_helper.ehf_manager->GetSignalsStage(pindexPrev);

    // mapModifiedTx will store sorted packages after they are modified
    // because some of their txs are already in the block
    indexed_modified_transaction_set mapModifiedTx;
    // Keep track of entries that failed inclusion, to avoid duplicate work
    CTxMemPool::setEntries failedTx;

    CTxMemPool::indexed_transaction_set::index<ancestor_score>::type::iterator mi = mempool.mapTx.get<ancestor_score>().begin();
    CTxMemPool::txiter iter;

    // Limit the number of attempts to add transactions to the block when it is
    // close to full; this is just a simple heuristic to finish quickly if the
    // mempool has a lot of entries.
    const int64_t MAX_CONSECUTIVE_FAILURES = 1000;
    int64_t nConsecutiveFailed = 0;

    while (mi != mempool.mapTx.get<ancestor_score>().end() || !mapModifiedTx.empty()) {
        // First try to find a new transaction in mapTx to evaluate.
        //
        // Skip entries in mapTx that are already in a block or are present
        // in mapModifiedTx (which implies that the mapTx ancestor state is
        // stale due to ancestor inclusion in the block)
        // Also skip transactions that we've already failed to add. This can happen if
        // we consider a transaction in mapModifiedTx and it fails: we can then
        // potentially consider it again while walking mapTx.  It's currently
        // guaranteed to fail again, but as a belt-and-suspenders check we put it in
        // failedTx and avoid re-evaluation, since the re-evaluation would be using
        // cached size/sigops/fee values that are not actually correct.
        /** Return true if given transaction from mapTx has already been evaluated,
         * or if the transaction's cached data in mapTx is incorrect. */
        if (mi != mempool.mapTx.get<ancestor_score>().end()) {
            auto it = mempool.mapTx.project<0>(mi);
            assert(it != mempool.mapTx.end());
            if (mapModifiedTx.count(it) || inBlock.count(it) || failedTx.count(it)) {
                ++mi;
                continue;
            }
        }

        // Now that mi is not stale, determine which transaction to evaluate:
        // the next entry from mapTx, or the best from mapModifiedTx?
        bool fUsingModified = false;

        modtxscoreiter modit = mapModifiedTx.get<ancestor_score>().begin();
        if (mi == mempool.mapTx.get<ancestor_score>().end()) {
            // We're out of entries in mapTx; use the entry from mapModifiedTx
            iter = modit->iter;
            fUsingModified = true;
        } else {
            // Try to compare the mapTx entry to the mapModifiedTx entry
            iter = mempool.mapTx.project<0>(mi);
            if (modit != mapModifiedTx.get<ancestor_score>().end() &&
                    CompareTxMemPoolEntryByAncestorFee()(*modit, CTxMemPoolModifiedEntry(iter))) {
                // The best entry in mapModifiedTx has higher score
                // than the one from mapTx.
                // Switch which transaction (package) to consider
                iter = modit->iter;
                fUsingModified = true;
            } else {
                // Either no entry in mapModifiedTx, or it's worse than mapTx.
                // Increment mi for the next loop iteration.
                ++mi;
            }
        }

        // We skip mapTx entries that are inBlock, and mapModifiedTx shouldn't
        // contain anything that is inBlock.
        assert(!inBlock.count(iter));

        uint64_t packageSize = iter->GetSizeWithAncestors();
        CAmount packageFees = iter->GetModFeesWithAncestors();
        unsigned int packageSigOps = iter->GetSigOpCountWithAncestors();
        if (fUsingModified) {
            packageSize = modit->nSizeWithAncestors;
            packageFees = modit->nModFeesWithAncestors;
            packageSigOps = modit->nSigOpCountWithAncestors;
        }

        if (packageFees < m_options.blockMinFeeRate.GetFee(packageSize)) {
            // Everything else we might consider has a lower fee rate
            return;
        }

        if (!TestPackage(packageSize, packageSigOps)) {
            if (fUsingModified) {
                // Since we always look at the best entry in mapModifiedTx,
                // we must erase failed entries so that we can consider the
                // next best entry on the next loop iteration
                mapModifiedTx.get<ancestor_score>().erase(modit);
                failedTx.insert(iter);
            }

            ++nConsecutiveFailed;

            if (nConsecutiveFailed > MAX_CONSECUTIVE_FAILURES && nBlockSize > m_options.nBlockMaxSize - 1000) {
                // Give up if we're close to full and haven't succeeded in a while
                break;
            }
            continue;
        }

        CTxMemPool::setEntries ancestors;
        std::string dummy;
        mempool.CalculateMemPoolAncestors(*iter, ancestors, CTxMemPool::Limits::NoLimits(), dummy, false);

        onlyUnconfirmed(ancestors);
        ancestors.insert(iter);

        // Test if all tx's are Final and safe
        if (!TestPackageTransactions(ancestors)) {
            if (fUsingModified) {
                mapModifiedTx.get<ancestor_score>().erase(modit);
                failedTx.insert(iter);
            }
            continue;
        }

        // Package can be added. Sort the entries in a valid order.
        std::vector<CTxMemPool::txiter> sortedEntries;
        SortForBlock(ancestors, sortedEntries);

        auto packageSignals = signals;
        std::vector<CTransactionRef> creditPoolTransactions;
        bool validPackage{true};
        for (const auto& entry : sortedEntries) {
            const auto& tx = entry->GetTx();
            if (std::optional<uint8_t> signal = extractEHFSignal(tx); signal != std::nullopt) {
                if (!packageSignals.emplace(*signal, 0).second) {
                    LogPrintf("%s: package tx %s skipped due to duplicate EHF signal %d\n", __func__,
                              tx.GetHash().ToString(), *signal);
                    validPackage = false;
                    break;
                }
            }
            if (tx.IsSpecialTxVersion() && (tx.nType == TRANSACTION_ASSET_LOCK || tx.nType == TRANSACTION_ASSET_UNLOCK)) {
                creditPoolTransactions.emplace_back(entry->GetSharedTx());
            }
        }

        if (validPackage && creditPoolDiff != std::nullopt && !creditPoolTransactions.empty()) {
            TxValidationState state;
            if (!creditPoolDiff->ProcessLockUnlockTransactions(creditPoolTransactions, state)) {
                LogPrintf("%s: package tx %s skipped due to credit pool state: %s\n", __func__,
                          iter->GetTx().GetHash().ToString(), state.ToString());
                validPackage = false;
            }
        }
        if (!validPackage) {
            if (fUsingModified) {
                mapModifiedTx.get<ancestor_score>().erase(modit);
                failedTx.insert(iter);
            }
            continue;
        }

        // This transaction will make it in; reset the failed counter.
        nConsecutiveFailed = 0;
        signals = std::move(packageSignals);

        for (size_t i = 0; i < sortedEntries.size(); ++i) {
            AddToBlock(sortedEntries[i]);
            // Erase from the modified set, if present
            mapModifiedTx.erase(sortedEntries[i]);
        }

        ++nPackagesSelected;

        // Update transactions that depend on each of these
        nDescendantsUpdated += UpdatePackagesForAdded(mempool, ancestors, mapModifiedTx);
    }
}
} // namespace node
