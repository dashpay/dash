// Copyright (c) 2025-2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying file COPYING.
#ifndef BITCOIN_LLMQ_QUORUMPROOFDATA_H
#define BITCOIN_LLMQ_QUORUMPROOFDATA_H
#include <primitives/block.h>
#include <serialize.h>
#include <uint256.h>
#include <vector>
namespace llmq {
inline constexpr size_t MAX_PROOF_BYTES = 1024 * 1024;
inline constexpr size_t MAX_PROOF_CERTIFICATES = 4096;
inline constexpr size_t MAX_PROOF_HEADERS = 4096;
struct ProofMerklePath {
    uint32_t index{0};
    uint32_t count{0};
    std::vector<uint256> siblings;
    SERIALIZE_METHODS(ProofMerklePath, obj) { READWRITE(obj.index, obj.count, obj.siblings); }
    bool Verify(uint256 leaf, const uint256& root) const;
    static ProofMerklePath Build(const std::vector<uint256>& leaves, uint32_t index);
};
struct ProofTransaction {
    std::vector<unsigned char> transaction;
    ProofMerklePath path;
    SERIALIZE_METHODS(ProofTransaction, obj) { READWRITE(obj.transaction, obj.path); }
    bool Verify(const CBlockHeader& header) const;
    static ProofTransaction Build(const CBlock& block, uint32_t index);
};
} // namespace llmq
#endif // BITCOIN_LLMQ_QUORUMPROOFDATA_H
