// Copyright (c) 2017-2021 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_EVO_CBTX_H
#define BITCOIN_EVO_CBTX_H

#include <primitives/transaction.h>
#include <univalue.h>

class CBlock;
class CBlockIndex;
class CCoinsViewCache;
class CValidationState;

// coinbase transaction
class CCbTx
{
public:
    static constexpr uint16_t CURRENT_VERSION = 2;

    uint16_t nVersion{CURRENT_VERSION};
    int32_t nHeight{0};
    uint256 merkleRootMNList;
    uint256 merkleRootQuorums;

    SERIALIZE_METHODS(CCbTx, obj)
    {
        READWRITE(obj.nVersion, obj.nHeight, obj.merkleRootMNList);

        if (obj.nVersion >= 2) {
            READWRITE(obj.merkleRootQuorums);
        }
    }

    std::string ToString() const;

    void ToJson(UniValue& obj) const
    {
        obj.clear();
        obj.setObject();
        obj.pushKV("version", (int)nVersion);
        obj.pushKV("height", nHeight);
        obj.pushKV("merkleRootMNList", merkleRootMNList.ToString());
        if (nVersion >= 2) {
            obj.pushKV("merkleRootQuorums", merkleRootQuorums.ToString());
        }
    }
};

bool CheckCbTx(const CTransaction& tx, const CBlockIndex* pindexPrev, CValidationState& state);

bool CheckCbTxMerkleRoots(const CBlock& block, const CBlockIndex* pindex, CValidationState& state, const CCoinsViewCache& view);
std::optional<uint256> CalcCbTxMerkleRootMNList(const CBlock& block, const CBlockIndex* pindexPrev, CValidationState& state, const CCoinsViewCache& view);
std::optional<uint256> CalcCbTxMerkleRootQuorums(const CBlock& block, const CBlockIndex* pindexPrev, CValidationState& state);

#endif // BITCOIN_EVO_CBTX_H
