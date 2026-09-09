// Copyright (c) 2021-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainlock/clsig.h>

#include <chain.h>
#include <chainlock/chainlock.h>
#include <chainparams.h>
#include <evo/cbtx.h>
#include <evo/specialtx.h>
#include <llmq/quorumsman.h>
#include <node/blockstorage.h>
#include <shutdown.h>

#include <algorithm>
#include <stdexcept>
#include <string_view>

namespace chainlock {
static constexpr std::string_view CLSIG_REQUESTID_PREFIX{"clsig"};
static constexpr size_t MAX_HISTORICAL_CARRIER_READS{16384};

std::optional<CoinbaseChainLock> CoinbaseChainLockReader::Read(int carrier_height)
{
    const auto* carrier = m_chain[carrier_height];
    if (!carrier || carrier_height < Params().GetConsensus().V20Height) return std::nullopt;
    if (const auto it = m_cache.find(carrier_height); it != m_cache.end()) return it->second;
    if (ShutdownRequested()) throw std::runtime_error("ChainLock lookup interrupted");
    if (m_cache.size() >= MAX_HISTORICAL_CARRIER_READS)
        throw std::runtime_error("ChainLock disk-read budget exhausted");
    CBlock block;
    if (!node::ReadBlockFromDisk(block, carrier, Params().GetConsensus()) || block.vtx.empty()) {
        throw std::runtime_error("Historical ChainLock block data unavailable");
    }
    const auto cb = GetTxPayload<CCbTx>(*block.vtx[0]);
    if (!block.vtx[0]->IsCoinBase() || block.vtx[0]->nType != TRANSACTION_COINBASE || !cb ||
        cb->nVersion < CCbTx::Version::CLSIG_AND_BALANCE || cb->nHeight != carrier_height) {
        throw std::runtime_error("Invalid historical ChainLock coinbase");
    }
    if (!cb->bestCLSignature.IsValid()) {
        return m_cache.emplace(carrier_height, std::nullopt).first->second;
    }
    if (cb->bestCLHeightDiff >= uint32_t(carrier_height)) {
        throw std::runtime_error("Invalid historical coinbase ChainLock height");
    }
    const int height = carrier_height - int(cb->bestCLHeightDiff) - 1;
    return m_cache
        .emplace(carrier_height,
                 CoinbaseChainLock{ChainLockSig{height, m_chain[height]->GetBlockHash(), cb->bestCLSignature}, carrier})
        .first->second;
}

std::optional<CoinbaseChainLock> CoinbaseChainLockReader::Find(int minimum_height, int maximum_height)
{
    if (minimum_height < 0 || minimum_height > maximum_height || minimum_height >= m_chain.Height())
        return std::nullopt;
    int low = std::max(minimum_height + 1, Params().GetConsensus().V20Height);
    if (low > m_chain.Height()) return std::nullopt;
    int high = low;
    int64_t step = 1;
    // Valid coinbases never move backwards in certified height. Exponential
    // search finds a nearby carrier quickly, even after a long signing gap.
    while (true) {
        const auto entry = Read(high);
        if (entry && entry->clsig.getHeight() >= minimum_height) break;
        if (high == m_chain.Height()) return std::nullopt;
        low = high + 1;
        high = int(std::min<int64_t>(m_chain.Height(), int64_t(high) + step));
        step *= 2;
    }
    while (low < high) {
        const int middle = low + (high - low) / 2;
        const auto entry = Read(middle);
        if (entry && entry->clsig.getHeight() >= minimum_height)
            high = middle;
        else
            low = middle + 1;
    }
    auto entry = Read(low);
    if (entry && entry->clsig.getHeight() <= maximum_height) return entry;
    return std::nullopt;
}

uint256 GenSigRequestId(const int32_t nHeight)
{
    return ::SerializeHash(std::make_pair(CLSIG_REQUESTID_PREFIX, nHeight));
}

llmq::VerifyRecSigStatus VerifyChainLock(const Consensus::Params& params, const CChain& chain,
                                         const llmq::CQuorumManager& qman, const chainlock::ChainLockSig& clsig)
{
    const auto llmqType = params.llmqTypeChainLocks;
    const uint256 request_id = GenSigRequestId(clsig.getHeight());

    return llmq::VerifyRecoveredSig(llmqType, chain, qman, clsig.getHeight(), request_id, clsig.getBlockHash(),
                                    clsig.getSig());
}

llmq::VerifyRecSigStatus VerifyChainLock(const Consensus::Params& params, const llmq::CQuorumManager& qman,
                                         const chainlock::ChainLockSig& clsig, const CBlockIndex* pindexStart)
{
    const auto llmqType = params.llmqTypeChainLocks;
    const uint256 request_id = GenSigRequestId(clsig.getHeight());

    return llmq::VerifyRecoveredSig(llmqType, qman, pindexStart, request_id, clsig.getBlockHash(), clsig.getSig());
}
} // namespace chainlock
