// Copyright (c) 2020 Julian Meyer
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <vector>
#include "consensus/merkle.h"
#include "proof.h"

uint256 CalcCoinMerkleRoot(CCoinsViewCache *cc) {
    auto coins = cc->GetAllCoins();

    std::vector<uint256> outpointHashes;
    outpointHashes.resize(coins.size());

    std::transform(coins.begin(), coins.end(), outpointHashes.begin(), [](const std::pair<COutPoint, Coin> in) -> uint256 {
        CHashWriter ss(SER_GETHASH, 0);
        ss << in.first;
        ss << in.second.out.nValue;
        ss << in.second.out.scriptPubKey;
        return ss.GetHash();
    });

    return ComputeMerkleRoot(outpointHashes);
}