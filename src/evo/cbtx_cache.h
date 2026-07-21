// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_EVO_CBTX_CACHE_H
#define BITCOIN_EVO_CBTX_CACHE_H

/**
 * Drop process-lifetime CbTx quorum-commitment hash caches.
 *
 * Required when mined commitment data for a quorum base block can change without the
 * active base-block list changing (e.g. disconnect/reorg of the block that mined a
 * different valid CFinalCommitment for the same quorumHash).
 */
void InvalidateCachedQcHashes();

#endif // BITCOIN_EVO_CBTX_CACHE_H
