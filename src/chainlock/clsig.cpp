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
    const int first_carrier = std::max(minimum_height + 1, Params().GetConsensus().V20Height);
    const int tip_height = m_chain.Height();
    if (first_carrier > tip_height) return std::nullopt;

    // Empty carriers are possible, so the predicate used by a binary search is
    // not monotonic. Scan the bounded carrier range instead; Read() keeps the
    // request-local disk-read budget and cache. Valid certificates are ordered
    // by signed height, so a certificate above the requested maximum ends the
    // search.
    for (int carrier_height = first_carrier;; ++carrier_height) {
        auto entry = Read(carrier_height);
        if (entry) {
            if (entry->clsig.getHeight() > maximum_height) return std::nullopt;
            if (entry->clsig.getHeight() >= minimum_height) return entry;
        }
        if (carrier_height == tip_height) break;
    }
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
