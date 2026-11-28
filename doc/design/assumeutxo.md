# assumeutxo

Assumeutxo is a feature that allows fast bootstrapping of a validating dashd
instance.

## Loading a snapshot

There is currently no canonical source for snapshots, but any downloaded snapshot
will be checked against a hash that's been hardcoded in source code.

Dash Core accepts one snapshot per network:

| network | base height | base block hash | snapshot file size | snapshot file sha256 |
| ------- | ----------- | --------------- | ------------------ | -------------------- |
| mainnet | 2579482 | `000000000000001cfbe4751211d7e753bab093e5d63de1fa947cf20714a0e727` | 289606403 bytes | `bf65a65ef575989e58df25f5781187a6b4dec25e0211b329fb2413b203dc952f` |
| testnet | 1599687 | `0000006e4b85a1f38f3224ead2865de8e7f29d98229b58b00a1b1bedab624872` | 64126131 bytes | `651e5b29b8c9ff9b50606cbd95c515e7877fb0004432bfa9d4bdce228fc99786` |

The snapshot file is deterministic: `dumptxoutset` writes the same bytes at the same
base block on every node, so a file can be checked with `sha256sum` before loading it.
Loading checks the file again, independently of where it came from: the coins against
the pinned UTXO set hash and the Dash evo section (masternode list, quorums, credit
pool and hard-fork signals) against the pinned evo hash.

Once you've obtained the snapshot, start the node and wait until its headers have
passed the base block (`getblockchaininfo` reports `headers` at or above the base
height), then load it with the RPC command `loadtxoutset`, for example

```
dash-cli loadtxoutset /path/to/utxo-2579482.dat
```

The node then syncs to the network tip from the base block, and validates the chain
up to the base block in the background. `getchainstates` shows both chainstates and
whether the snapshot has been validated yet. Once background validation has reached
the base block and a restart has promoted the snapshot chainstate, the node is
indistinguishable from one that synced from genesis.

A masternode does not load snapshots, and a node does not start with
`-masternodeblsprivkey` while it has a snapshot chainstate, validated or not,
until a restart has promoted it.

### Pruning

A pruned node cannot load a snapshot, and a node does not start with `-prune`
while it has a snapshot chainstate, validated or not, until a restart has
promoted it. It's possible to delete the snapshot file as soon as
`loadtxoutset` finishes.

The minimum `-prune` setting is 550 MiB, but this functionality ignores that
minimum and uses at least 1100 MiB.

As the background sync continues there will be temporarily two chainstate
directories, each multiple gigabytes in size (likely growing larger than the
downloaded snapshot).

### Indexes

A node with `-addressindex`, `-spentindex` or `-timestampindex` cannot load a
snapshot, and does not start with them while it has a snapshot chainstate.

Other indexes work but don't take advantage of this feature. They always start building
from the genesis block. Once the background validation reaches the snapshot block,
indexes will continue to build all the way to the tip.

Governance waits until background validation has completed and the governance
collateral index has caught up: until then the node neither processes nor relays
governance objects, and `gobject prepare` and `gobject submit` refuse.

For indexes that support pruning, note that no pruning will take place between
the snapshot and the tip, until the background sync has completed - after which
everything is pruned. Depending on how old the snapshot is, this may temporarily
use a significant amount of disk space.

## Generating a snapshot

The RPC command `dumptxoutset` can be used to generate a snapshot. This can be used
to create a snapshot on one node that you wish to load on another node.
It can also be used to verify the hardcoded snapshot hash in the source code.

`dumptxoutset` writes the snapshot at the node's current tip, and only a snapshot whose
base is pinned can be loaded. To produce the pinned mainnet or testnet file, use a copy
of a synced datadir, disconnect it from the network (`setnetworkactive false`), call
`invalidateblock` on the block after the base, and run `dumptxoutset` there. Its
`txoutset_hash`, `evo.hash` and `nchaintx` fields are the values pinned in
`src/kernel/chainparams.cpp`.

The utility script
`./contrib/devtools/utxo_snapshot.sh` may be of use.

## General background

- [assumeutxo proposal](https://github.com/jamesob/assumeutxo-docs/tree/2019-04-proposal/proposal)
- [Github issue](https://github.com/bitcoin/bitcoin/issues/15605) (Bitcoin)
- [draft PR](https://github.com/bitcoin/bitcoin/pull/15606) (Bitcoin)

## Design notes

- The concept of UTXO snapshots is treated as an implementation detail that lives
  behind the ChainstateManager interface. The external presentation of the changes
  required to facilitate the use of UTXO snapshots is the understanding that there are
  now certain regions of the chain that can be temporarily assumed to be valid.
  In certain cases, e.g. wallet rescanning, this is very similar to dealing with
  a pruned chain.

  Logic outside ChainstateManager should try not to know about snapshots, instead
  preferring to work in terms of more general states like assumed-valid.


## Chainstate phases

Chainstate within the system goes through a number of phases when UTXO snapshots are
used, as managed by `ChainstateManager`. At various points there can be multiple
`Chainstate` objects in existence to facilitate both maintaining the network tip and
performing historical validation of the assumed-valid chain.

It is worth noting that though there are multiple separate chainstates, those
chainstates share use of a common block index (i.e. they hold the same `BlockManager`
reference).

The subheadings below outline the phases and the corresponding changes to chainstate
data.

### "Normal" operation via initial block download

`ChainstateManager` manages a single Chainstate object, for which
`m_snapshot_blockhash` is null. This chainstate is (maybe obviously)
considered active. This is the "traditional" mode of operation for dashd.

|    |    |
| ---------- | ----------- |
| number of chainstates | 1 |
| active chainstate | ibd |

### User loads a UTXO snapshot via `loadtxoutset` RPC

`ChainstateManager` initializes a new chainstate (see `ActivateSnapshot()`) to load the
snapshot contents into. During snapshot load and validation (see
`PopulateAndValidateSnapshot()`), the new chainstate is not considered active and the
original chainstate remains in use as active.

|    |    |
| ---------- | ----------- |
| number of chainstates | 2 |
| active chainstate | ibd |

Once the snapshot chainstate is loaded and validated, it is promoted to active
chainstate and a sync to tip begins. A new chainstate directory is created in the
datadir for the snapshot chainstate called `chainstate_snapshot`.

When this directory is present in the datadir, the snapshot chainstate will be detected
and loaded as active on node startup (via `DetectSnapshotChainstate()`).

A special file is created within that directory, `base_blockhash`, which contains the
serialized `uint256` of the base block of the snapshot. This is used to reinitialize
the snapshot chainstate on subsequent inits. Otherwise, the directory is a normal
leveldb database.

|    |    |
| ---------- | ----------- |
| number of chainstates | 2 |
| active chainstate | snapshot |

The snapshot begins to sync to tip from its base block, technically in parallel with
the original chainstate, but it is given priority during block download and is
allocated most of the cache (see `MaybeRebalanceCaches()` and usages) as our chief
goal is getting to network tip.

**Failure consideration:** if shutdown happens at any point during this phase, both
chainstates will be detected during the next init and the process will resume.

### Snapshot chainstate hits network tip

Once the snapshot chainstate leaves IBD, caches are rebalanced
(via `MaybeRebalanceCaches()` in `ActivateBestChain()`) and more cache is given
to the background chainstate, which is responsible for doing full validation of the
assumed-valid parts of the chain.

**Note:** at this point, ValidationInterface callbacks will be coming in from both
chainstates. Considerations here must be made for indexing, which may no longer be happening
sequentially.

### Background chainstate hits snapshot base block

Once the tip of the background chainstate hits the base block of the snapshot
chainstate, we stop use of the background chainstate by setting `m_disabled`, in
`MaybeCompleteSnapshotValidation()`, which is checked in `ActivateBestChain()`. We hash the
background chainstate's UTXO set contents and ensure it matches the compiled value in
`CMainParams::m_assumeutxo_data`. In Dash, completion additionally compares the
deterministic masternode-list hash the background chainstate derived at the base block
against the hash recorded at snapshot activation, and the EvoDB best-block markers
against both chainstates' coins tips; any divergence fails completion with
`EVO_STATE_MISMATCH` and quarantines the snapshot exactly like a UTXO hash mismatch.

|    |    |
| ---------- | ----------- |
| number of chainstates | 2 (ibd has `m_disabled=true`) |
| active chainstate | snapshot |

The background chainstate data lingers on disk until the program is restarted.

### Dashd restarts sometime after snapshot validation has completed

After a shutdown and subsequent restart, `LoadChainstate()` cleans up the background
chainstate with `ValidatedSnapshotCleanup()`, which renames the `chainstate_snapshot`
datadir as `chainstate` and removes the now unnecessary background chainstate data.

|    |    |
| ---------- | ----------- |
| number of chainstates | 1 |
| active chainstate | ibd (was snapshot, but is now fully validated) |

What began as the snapshot chainstate is now indistinguishable from a chainstate that
has been built from the traditional IBD process, and will be initialized as such.

A file will be left in `chainstate/base_blockhash`, which indicates that the
chainstate, even though now fully validated, was originally started from a snapshot
with the corresponding base blockhash.
