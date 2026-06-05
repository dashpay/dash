// Copyright (c) 2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <node/caches.h>

#include <index/addressindex.h>
#include <index/spentindex.h>
#include <index/timestampindex.h>
#include <index/txindex.h>
#include <txdb.h>
#include <util/system.h>

namespace node {
CacheSizes CalculateCacheSizes(const ArgsManager& args, size_t n_indexes)
{
    int64_t nTotalCache = (args.GetIntArg("-dbcache", nDefaultDbCache) << 20);
    nTotalCache = std::max(nTotalCache, nMinDbCache << 20); // total cache cannot be less than nMinDbCache
    nTotalCache = std::min(nTotalCache, nMaxDbCache << 20); // total cache cannot be greater than nMaxDbcache
    CacheSizes sizes;
    sizes.block_tree_db = std::min(nTotalCache / 8, nMaxBlockDBCache << 20);
    nTotalCache -= sizes.block_tree_db;
    sizes.tx_index = std::min(nTotalCache / 8, args.GetBoolArg("-txindex", DEFAULT_TXINDEX) ? nMaxTxIndexCache << 20 : 0);
    nTotalCache -= sizes.tx_index;
    sizes.address_index = std::min(nTotalCache / 8, args.GetBoolArg("-addressindex", DEFAULT_ADDRESSINDEX) ? int64_t(256 << 20) : int64_t(0));
    nTotalCache -= sizes.address_index;
    sizes.timestamp_index = std::min(nTotalCache / 64, args.GetBoolArg("-timestampindex", DEFAULT_TIMESTAMPINDEX) ? int64_t(8 << 20) : int64_t(0));
    nTotalCache -= sizes.timestamp_index;
    sizes.spent_index = std::min(nTotalCache / 8, args.GetBoolArg("-spentindex", DEFAULT_SPENTINDEX) ? int64_t(128 << 20) : int64_t(0));
    nTotalCache -= sizes.spent_index;
    sizes.filter_index = 0;
    if (n_indexes > 0) {
        int64_t max_cache = std::min(nTotalCache / 8, max_filter_index_cache << 20);
        sizes.filter_index = max_cache / n_indexes;
        nTotalCache -= sizes.filter_index * n_indexes;
    }
    sizes.coins_db = std::min(nTotalCache / 2, (nTotalCache / 4) + (1 << 23)); // use 25%-50% of the remainder for disk cache
    sizes.coins_db = std::min(sizes.coins_db, nMaxCoinsDBCache << 20); // cap total coins db cache
    nTotalCache -= sizes.coins_db;
    sizes.coins = nTotalCache; // the rest goes to in-memory cache
    return sizes;
}
} // namespace node
