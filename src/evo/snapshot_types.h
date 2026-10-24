// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_EVO_SNAPSHOT_TYPES_H
#define BITCOIN_EVO_SNAPSHOT_TYPES_H

#include <stdexcept>

namespace evo {

/** Base of the errors that describe this node's own Evo state rather than the
 *  block, transaction or peer being processed. Generic catch sites rethrow it
 *  unchanged so that the chainstate boundary classifies it: it never becomes
 *  consensus invalidity or a peer penalty. */
class LocalStateError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

/** Evo state carried by an assumeutxo snapshot disagrees with state derived
 *  independently. The snapshot is rejected; the block is not at fault. */
class SnapshotStateMismatchError : public LocalStateError
{
public:
    using LocalStateError::LocalStateError;
};

} // namespace evo

/** Thrown when Evo state for a block cannot be reconstructed because the data
 *  is not on this node yet (pruned, or below an unvalidated snapshot base, or
 *  pending in another chainstate's unflushed EvoDB overlay). Distinct from the
 *  plain std::runtime_error that CDeterministicMNList::ApplyDiff raises for
 *  genuine local corruption, which must never be swallowed. Messages about a
 *  masternode list carry BLOCK_DATA_UNAVAILABLE_SUFFIX, matched by
 *  IsBlockDataUnavailableError(). */
class BlockDataUnavailableError : public evo::LocalStateError
{
public:
    using evo::LocalStateError::LocalStateError;
};

/** An Evo read, write or cache lookup was not bound to the view of the
 *  chainstate it serves, or was bound to a conflicting or stale view. */
class EvoViewBindingError : public evo::LocalStateError
{
public:
    using evo::LocalStateError::LocalStateError;
};

#endif // BITCOIN_EVO_SNAPSHOT_TYPES_H
