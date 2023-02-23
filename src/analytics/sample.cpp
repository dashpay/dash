// Copyright (c) 2014-2023 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <analytics/sample.h>

#include <node/coinstats.h>
#include <rpc/blockchain.h>
#include <rpc/server.h>
#include <policy/policy.h>
#include <util/system.h>
#include <validation.h>

#include <analytics/sdclient.h>

#include <algorithm>

void SampleStats(const Statsd::StatsdClient& client, const ArgsManager* args, const CTxMemPool* mempool)
{
    if (args == nullptr)
        return;

    assert(args->GetBoolArg("-statsenabled", DEFAULT_STATSD_ENABLE));
    CCoinsStats stats{CoinStatsHashType::NONE};
    ::ChainstateActive().ForceFlushStateToDisk();
    if (WITH_LOCK(cs_main, return GetUTXOStats(&::ChainstateActive().CoinsDB(), std::ref(g_chainman.m_blockman), stats, RpcInterruptionPoint, ::ChainActive().Tip()))) {
        client.gauge("utxoset.tx", stats.nTransactions, 1.0f);
        client.gauge("utxoset.txOutputs", stats.nTransactionOutputs, 1.0f);
        client.gauge("utxoset.dbSizeBytes", stats.nDiskSize, 1.0f);
        client.gauge("utxoset.blockHeight", stats.nHeight, 1.0f);
        client.gauge("utxoset.totalAmount", (double)stats.nTotalAmount / (double)COIN, 1.0f);
    } else {
        // something went wrong
        LogPrintf("%s: GetUTXOStats failed\n", __func__);
    }

    // short version of GetNetworkHashPS(120, -1);
    CBlockIndex *tip = ::ChainActive().Tip();
    CBlockIndex *pindex = tip;
    int64_t minTime = pindex->GetBlockTime();
    int64_t maxTime = minTime;
    for (int i = 0; i < 120 && pindex->pprev != nullptr; i++) {
        pindex = pindex->pprev;
        int64_t time = pindex->GetBlockTime();
        minTime = std::min(time, minTime);
        maxTime = std::max(time, maxTime);
    }
    arith_uint256 workDiff = tip->nChainWork - pindex->nChainWork;
    int64_t timeDiff = maxTime - minTime;
    double nNetworkHashPS = workDiff.getdouble() / timeDiff;

    client.gauge("network.hashesPerSecond", nNetworkHashPS);
    client.gauge("network.terahashesPerSecond", nNetworkHashPS / 1e12);
    client.gauge("network.petahashesPerSecond", nNetworkHashPS / 1e15);
    client.gauge("network.exahashesPerSecond", nNetworkHashPS / 1e18);
    // No need for cs_main, we never use null tip here
    client.gauge("network.difficulty", (double)GetDifficulty(tip));

    client.gauge("transactions.txCacheSize", WITH_LOCK(cs_main, return ::ChainstateActive().CoinsTip().GetCacheSize()), 1.0f);
    client.gauge("transactions.totalTransactions", tip->nChainTx, 1.0f);

    if (mempool != nullptr)
    {
        LOCK(mempool->cs);
        client.gauge("transactions.mempool.totalTransactions", mempool->size(), 1.0f);
        client.gauge("transactions.mempool.totalTxBytes", (int64_t) mempool->GetTotalTxSize(), 1.0f);
        client.gauge("transactions.mempool.memoryUsageBytes", (int64_t) mempool->DynamicMemoryUsage(), 1.0f);
        client.gauge("transactions.mempool.minFeePerKb", mempool->GetMinFee(args->GetArg("-maxmempool", DEFAULT_MAX_MEMPOOL_SIZE) * 1000000).GetFeePerK(), 1.0f);
    }
}
