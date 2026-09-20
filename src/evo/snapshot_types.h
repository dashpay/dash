// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_EVO_SNAPSHOT_TYPES_H
#define BITCOIN_EVO_SNAPSHOT_TYPES_H

#include <stdexcept>

namespace evo {

class SnapshotStateMismatchError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

} // namespace evo

#endif // BITCOIN_EVO_SNAPSHOT_TYPES_H
