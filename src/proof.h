// Copyright (c) 2020 Julian Meyer
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_PROOF_H
#define BITCOIN_PROOF_H

#include "serialize.h"
#include "uint256.h"
#include "coins.h"

class COutPoint;

class CCoinsProof
{
public:
    CCoinsProof(std::vector<uint256> merkleBranch, CTransaction tx, uint32_t index, CScript scriptPubKey):
        merkleBranch(merkleBranch), tx(tx), merkleIndex(index), scriptPubKey(scriptPubKey) {};

    ADD_SERIALIZE_METHODS;

    template <typename Stream, typename Operation>
    inline void SerializationOp(Stream& s, Operation ser_action)
    {
        READWRITE(merkleIndex);
        READWRITE(merkleBranch);
        READWRITE(tx);
        READWRITE(scriptPubKey);
    }

private:

    uint32_t merkleIndex;
    std::vector<uint256> merkleBranch;
    CTransaction tx;

    CScript scriptPubKey;
};

uint256 CalcCoinMerkleRoot(CCoinsViewCache *pCache);

/**
Example of usage
================

auto *pcoinstip = new CCoinsViewCache(pcoinsdbview);
uint256 transactionMerkleRoot = CalcCoinMerkleRoot(pcoinstip);
LogPrintf("Coin merkle root: %s at %d\n", transactionMerkleRoot.ToString(), chainActive.Height());

std::vector<COutput> coins;
pwalletMain->AvailableCoins(coins);

auto proofs = CalcCoinMerkleBranch(pcoinstip, coins, "julian test");

CDataStream ss(SER_GETHASH, 0);
for (const CCoinsProof& proof : proofs) {
    ss << proof;
}

LogPrintf("generated proof %s\n", EncodeBase64(ss.str()));
*/

#endif // BITCOIN_PROTOCOL_H