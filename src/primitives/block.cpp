// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2015 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "primitives/block.h"

#include "hash.h"
#include "streams.h"
#include "tinyformat.h"
#include "utilstrencodings.h"
#include "crypto/common.h"
#include <arith_uint256.h>

uint256 CBlockHeader::GetHash() const
{
    if (nTime > nTimeOfAlgorithmChange)
        return hash_Argon2d(BEGIN(nVersion), END(nNonce), 2);
    else
        return hash_Argon2d(BEGIN(nVersion), END(nNonce), 1);

}

int CBlockHeader::GetAlgo() const
{
    switch (nVersion & BLOCK_VERSION_ALGO)
    {
        case BLOCK_VERSION_ARGON2D:
            return ALGO_ARGON2D;
        case BLOCK_VERSION_RANDOMX:
            return ALGO_RANDOMX;
    }
    return ALGO_UNKNOWN;
}

uint256 CBlockHeader::GetPoWAlgoHash(const Consensus::Params& params) const
{
    switch (GetAlgo())
    {
        case ALGO_ARGON2D:
            return GetHash();
        case ALGO_RANDOMX:
            return GetHash();
        case ALGO_UNKNOWN:
            // This block will be rejected anyway, but returning an always-invalid
            // PoW hash will allow it to be rejected sooner.
            return ArithToUint256(~arith_uint256(0));
    }
    assert(false);
}

std::string CBlock::ToString() const
{
    std::stringstream s;
    s << strprintf("CBlock(hash=%s, ver=0x%08x, hashPrevBlock=%s, hashMerkleRoot=%s, nTime=%u, nBits=%08x, nNonce=%u, vtx=%u)\n",
        GetHash().ToString(),
        nVersion,
        hashPrevBlock.ToString(),
        hashMerkleRoot.ToString(),
        nTime, nBits, nNonce,
        vtx.size());
    for (unsigned int i = 0; i < vtx.size(); i++)
    {
        s << "  " << vtx[i]->ToString() << "\n";
    }
    return s.str();
}
