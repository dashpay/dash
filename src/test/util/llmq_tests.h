// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_TEST_UTIL_LLMQ_TESTS_H
#define BITCOIN_TEST_UTIL_LLMQ_TESTS_H

#include <test/util/random.h>
#include <test/util/setup_common.h>

#include <arith_uint256.h>
#include <bls/bls.h>
#include <compat/endian.h>
#include <consensus/params.h>
#include <evo/evodb.h>
#include <primitives/transaction.h>
#include <random.h>
#include <serialize.h>
#include <streams.h>
#include <uint256.h>

#include <chainlock/chainlock.h>
#include <llmq/commitment.h>
#include <llmq/params.h>

#include <util/fs.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <limits>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace llmq {
namespace testutils {

// Helper function to get LLMQ params from available_llmqs
inline const Consensus::LLMQParams& GetLLMQParams(Consensus::LLMQType type)
{
    for (const auto& params : Consensus::available_llmqs) {
        if (params.type == type) {
            return params;
        }
    }
    throw std::runtime_error("LLMQ type not found");
}

// Helper functions to create test data
inline CBLSPublicKey CreateRandomBLSPublicKey()
{
    CBLSSecretKey sk;
    sk.MakeNewKey();
    return sk.GetPublicKey();
}

inline CBLSSignature CreateRandomBLSSignature()
{
    CBLSSecretKey sk;
    sk.MakeNewKey();
    uint256 hash = InsecureRand256();
    return sk.Sign(hash, false);
}

inline CFinalCommitment CreateValidCommitment(const Consensus::LLMQParams& params, const uint256& quorumHash)
{
    CFinalCommitment commitment;
    commitment.llmqType = params.type;
    commitment.quorumHash = quorumHash;
    commitment.validMembers.resize(params.size, true);
    commitment.signers.resize(params.size, true);
    commitment.quorumVvecHash = InsecureRand256();
    commitment.quorumPublicKey = CreateRandomBLSPublicKey();
    commitment.quorumSig = CreateRandomBLSSignature();
    commitment.membersSig = CreateRandomBLSSignature();
    return commitment;
}

inline chainlock::ChainLockSig CreateChainLock(int32_t height, const uint256& blockHash)
{
    CBLSSignature sig = CreateRandomBLSSignature();
    return chainlock::ChainLockSig(height, blockHash, sig);
}

// Helper to create bit vectors with specific patterns
inline std::vector<bool> CreateBitVector(size_t size, const std::vector<size_t>& trueBits)
{
    std::vector<bool> result(size, false);
    for (size_t idx : trueBits) {
        if (idx < size) {
            result[idx] = true;
        }
    }
    return result;
}

// Serialization round-trip test helper
template <typename T>
inline bool TestSerializationRoundtrip(const T& obj)
{
    // Datastreams we will compare at the end
    CDataStream ss_obj(SER_NETWORK, PROTOCOL_VERSION);
    CDataStream ss_obj_rt(SER_NETWORK, PROTOCOL_VERSION);
    // A temporary datastream to perform serialization round-trip
    CDataStream ss_tmp(SER_NETWORK, PROTOCOL_VERSION);
    T obj_rt;

    ss_obj << obj;
    ss_tmp << obj;
    ss_tmp >> obj_rt;
    ss_obj_rt << obj_rt;

    return ss_obj.str() == ss_obj_rt.str();
}

// Helper to create deterministic test data
inline uint256 GetTestQuorumHash(uint32_t n) { return ArithToUint256(arith_uint256(n)); }

inline uint256 GetTestBlockHash(uint32_t n) { return ArithToUint256(arith_uint256(n) << 32); }

// Mirrors private DB keys in llmq/blockprocessor.cpp so tests can install
// mined-commitment state without a full DKG/mining path.
inline const std::string DB_MINED_COMMITMENT = "q_mc";
inline const std::string DB_MINED_COMMITMENT_BY_INVERSED_HEIGHT = "q_mcih";
inline const std::string DB_MINED_COMMITMENT_BY_INVERSED_HEIGHT_Q_INDEXED = "q_mcihi";

inline std::tuple<std::string, Consensus::LLMQType, uint32_t> BuildInversedHeightKey(Consensus::LLMQType llmqType,
                                                                                    int nMinedHeight)
{
    return std::make_tuple(DB_MINED_COMMITMENT_BY_INVERSED_HEIGHT, llmqType,
                           htobe32_internal(std::numeric_limits<uint32_t>::max() - nMinedHeight));
}

inline std::tuple<std::string, Consensus::LLMQType, int, uint32_t> BuildInversedHeightKeyIndexed(Consensus::LLMQType llmqType,
                                                                                                 int nMinedHeight,
                                                                                                 int quorumIndex)
{
    return std::make_tuple(DB_MINED_COMMITMENT_BY_INVERSED_HEIGHT_Q_INDEXED, llmqType, quorumIndex,
                           htobe32_internal(std::numeric_limits<uint32_t>::max() - nMinedHeight));
}

// An inversed-height index value padded past LevelDB's 4 KiB table block size, so that the entry
// ends the table block it is written to. The padding repeats the quorum height's low byte for
// CorruptTableBlock to find; readers deserialize only the leading quorum height.
inline std::pair<int, std::vector<uint8_t>> PaddedIndexValue(int quorum_height)
{
    return {quorum_height, std::vector<uint8_t>(8192, static_cast<uint8_t>(quorum_height))};
}

// Flips a byte of the first occurrence of `needle` in a table file of the closed LevelDB at
// `db_dir`, so that the table block holding it fails its checksum.
inline bool CorruptTableBlock(const fs::path& db_dir, const std::vector<uint8_t>& needle)
{
    for (const auto& entry : fs::directory_iterator(db_dir)) {
        if (entry.path().extension() != ".ldb") continue;
        std::vector<char> bytes;
        {
            std::ifstream in{entry.path(), std::ios::binary};
            bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        const auto pos{std::search(bytes.begin(), bytes.end(), needle.begin(), needle.end(),
                                   [](char a, uint8_t b) { return static_cast<uint8_t>(a) == b; })};
        if (pos == bytes.end()) continue;
        *pos ^= 1;
        std::ofstream out{entry.path(), std::ios::binary | std::ios::trunc};
        out.write(bytes.data(), bytes.size());
        out.close();
        return !out.fail();
    }
    return false;
}

// Store a mined commitment as if it was mined at `mined_height` for the quorum whose base block
// is the active-chain ancestor at `quorum_height`. GetMinedCommitmentsUntilBlock iterates
// inverted-height keys in [pindex->nHeight, 0), so scan height must be >= mined_height and
// mined_height must be > 0 for the entry to be returned. `qc.quorumHash` must be the block hash
// at `quorum_height` for GetMinedCommitment() to find the commitment again.
inline void WriteMinedCommitment(CEvoDB& evoDb, const CFinalCommitment& qc, const uint256& mined_block_hash,
                                 int mined_height, int quorum_height)
{
    assert(mined_height > 0);
    evoDb.Write(std::make_pair(DB_MINED_COMMITMENT, std::make_pair(qc.llmqType, qc.quorumHash)),
                std::make_pair(qc, mined_block_hash));
    evoDb.Write(BuildInversedHeightKey(qc.llmqType, mined_height), quorum_height);
}

} // namespace testutils
} // namespace llmq

#endif // BITCOIN_TEST_UTIL_LLMQ_TESTS_H
