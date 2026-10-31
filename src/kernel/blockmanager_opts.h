// Copyright (c) 2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_KERNEL_BLOCKMANAGER_OPTS_H
#define BITCOIN_KERNEL_BLOCKMANAGER_OPTS_H

#include <util/zstd.h>

class CChainParams;

namespace kernel {

/**
 * An options struct for `BlockManager`, more ergonomically referred to as
 * `BlockManager::Options` due to the using-declaration in `BlockManager`.
 */
struct BlockManagerOpts {
    const CChainParams& chainparams;
    //! zstd compression level for newly written block and undo records, 0 to store them uncompressed
    int block_compression{util::DefaultZstdLevel()};
};

} // namespace kernel

#endif // BITCOIN_KERNEL_BLOCKMANAGER_OPTS_H
