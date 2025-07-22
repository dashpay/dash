// Copyright (c) 2017-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_EVO_CBTX_H
#define BITCOIN_EVO_CBTX_H

#include <bls/bls.h>
#include <primitives/transaction.h>
#include <univalue.h>

#include <optional>

class BlockValidationState;
class CBlock;
class CBlockIndex;
class TxValidationState;
namespace llmq {
class CQuorumBlockProcessor;
}// namespace llmq

// coinbase transaction
class CCbTx
{
public:
    enum class Version : uint16_t {
        INVALID = 0,
        MERKLE_ROOT_MNLIST = 1,
        MERKLE_ROOT_QUORUMS = 2,
        CLSIG_AND_BALANCE = 3,
        UNKNOWN,
    };

    static constexpr auto SPECIALTX_TYPE = TRANSACTION_COINBASE;
    Version nVersion{Version::MERKLE_ROOT_QUORUMS};
    int32_t nHeight{0};
    uint256 merkleRootMNList;
    uint256 merkleRootQuorums;
    uint32_t bestCLHeightDiff{0};
    CBLSSignature bestCLSignature;
    CAmount creditPoolBalance{0};

    SERIALIZE_METHODS(CCbTx, obj)
    {
        READWRITE(obj.nVersion, obj.nHeight, obj.merkleRootMNList);

        if (obj.nVersion >= Version::MERKLE_ROOT_QUORUMS) {
            READWRITE(obj.merkleRootQuorums);
            if (obj.nVersion >= Version::CLSIG_AND_BALANCE) {
                READWRITE(COMPACTSIZE(obj.bestCLHeightDiff));
                READWRITE(obj.bestCLSignature);
                READWRITE(obj.creditPoolBalance);
            }
        }

    }

    std::string ToString() const;

    [[nodiscard]] UniValue ToJson() const;
};
template<> struct is_serializable_enum<CCbTx::Version> : std::true_type {};

bool CheckCbTx(const CCbTx& cbTx, const CBlockIndex* pindexPrev, TxValidationState& state);

// This can only be done after the block has been fully processed, as otherwise we won't have the finished MN list
bool CheckCbTxMerkleRoots(const CBlock& block, const CCbTx& cbTx, const CBlockIndex* pindex,
                          const llmq::CQuorumBlockProcessor& quorum_block_processor, BlockValidationState& state);
bool CalcCbTxMerkleRootQuorums(const CBlock& block, const CBlockIndex* pindexPrev,
                               const llmq::CQuorumBlockProcessor& quorum_block_processor, uint256& merkleRootRet,
                               BlockValidationState& state);

class CCoinbaseChainlock
{
public:
    CBLSSignature signature;
    uint32_t heightDiff{0};

    CCoinbaseChainlock() = default;
    CCoinbaseChainlock(const CBLSSignature& sig, uint32_t diff) : signature(sig), heightDiff(diff) {}

    [[nodiscard]] bool IsNull() const { return !signature.IsValid(); }
    [[nodiscard]] std::string ToString() const;

    SERIALIZE_METHODS(CCoinbaseChainlock, obj)
    {
        READWRITE(obj.signature, obj.heightDiff);
    }
};

std::optional<CCoinbaseChainlock> GetCoinbaseChainlock(const CBlock& block, const CBlockIndex* pindex);
std::optional<CCoinbaseChainlock> GetNonNullCoinbaseChainlock(const CBlockIndex* pindex);

#endif // BITCOIN_EVO_CBTX_H
