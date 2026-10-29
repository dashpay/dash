# Dash Core version v24.0.0

This is a new major version release, bringing new features, various bugfixes and other improvements.

This release introduces the `v24` network upgrade (hard fork) and is **mandatory** for all nodes.
Masternodes must upgrade before the hard fork start date; all other nodes must upgrade before the
hard fork activates. See [Network upgrade](#network-upgrade) below.

Please report bugs using the issue tracker at GitHub:

  <https://github.com/dashpay/dash/issues>

# Upgrading and downgrading

## How to Upgrade

If you are running an older version, shut it down. Wait until it has completely
shut down (which might take a few minutes for older versions), then run the
installer (on Windows) or just copy over /Applications/Dash-Qt (on Mac) or
dashd/dash-qt (on Linux).

On the first start after upgrading, a node that has `-addressindex`,
`-spentindex` or `-timestampindex` enabled migrates that index to its own
database (see [Asynchronous address, spent and timestamp indexes](#asynchronous-address-spent-and-timestamp-indexes)).
This can take 20-40 minutes or longer depending on hardware and index size. The
governance cache (`governance.dat`) and the spork cache (`sporks.dat`) changed
format and are discarded and re-synced from the network on the first start.

## Downgrade warning

### Downgrade to a version < v24.0.0

Once the `v24` hard fork activates, versions older than v24.0.0 can no longer
follow the network and must not be used.

Before activation, downgrading to v23.x is possible with the following caveats:

- The address, spent and timestamp indexes are moved out of the block index
  database into separate databases on the first start of v24.0.0. A node that
  had any of these indexes enabled must be restarted with `-reindex` after
  downgrading.
- The governance and spork caches written by v24.0.0 are not readable by older
  versions; they are discarded and re-synced from the network.
- v24.0.0 does not upgrade the wallet file format. Wallet records added by
  v24.0.0 that older versions do not know about (such as the new CoinJoin
  pending-observation record) are ignored by them.

### Downgrade to a version < v23.0.0

Downgrading to a version older than v23.0.0 is not supported, and will
require a reindex.

## Compatibility

- Dash Core binaries now require macOS 14 (Sonoma) or later. Dash Core v23.1.x
  binaries supported macOS 11 (Big Sur) or later. Windows 10 or later continues
  to be required on Windows.
- On Linux, kernel 3.17 or later is now required. The minimum glibc version
  (2.31) is unchanged.
- `PROTOCOL_VERSION` has been bumped from `70240` to `70242` (see
  [P2P and network changes](#p2p-and-network-changes)).
  `MIN_PEER_PROTO_VERSION` is unchanged at `70221`.
- `MIN_MASTERNODE_PROTO_VERSION` has been bumped from `70240` to `70242`. During
  DKG, quorum members connected over an older protocol version are considered
  to have a bad connection and are voted bad, which leads to PoSe penalties.
  All masternodes must therefore upgrade to v24.0.0.

# Network upgrade

This release introduces two new deployments that use the masternode-activated
[Enhanced Hard Fork](https://github.com/dashpay/dips/blob/master/dip-0023.md)
(EHF, DIP-0023) mechanism:

| Deployment | Name | Version bit |
| --- | --- | --- |
| V24 | `v24` | 13 |
| EvoNode shares | `evo_shares` | 14 |

On mainnet both deployments use identical parameters:

- start time: `1794182400` (Monday, November 9, 2026 00:00:00 UTC);
- timeout: `1857340800` (Thursday, November 9, 2028 00:00:00 UTC);
- signaling window: 4032 blocks (about 7 days);
- threshold: 80% (3226 of 4032 blocks) in the first signaling window, decreasing
  with each window to a floor of 60% (2420 of 4032 blocks) after 10 windows.

Activation proceeds as follows:

1. Once the median time past passes the start time, masternodes running v24
   sign an EHF signal with the `LLMQ_400_85` quorum. This requires 85% of the
   quorum's members to be running v24.
2. After the signal is mined, miner signaling starts at the next 4032-block
   window boundary.
3. If the threshold is reached within a window, the hard fork locks in, and it
   activates one window later.

The earliest possible activation is therefore roughly two to three weeks after
November 9, 2026.

Earliest possible activation: TBD-EARLIEST-DATE

All masternodes must upgrade before the start date. All other nodes must
upgrade before activation, or they will follow an invalid chain.

v24.0.0 does **not** sign the `evo_shares` signal. Until `evo_shares` activates,
EvoNodes cannot use shared collateral or more than one owner payout (see
[EvoNode restrictions](#evonode-restrictions)). A later release (v24.1) is
expected to sign it once Dash Platform supports EvoNode shares and payouts.
Both deployments carry identical parameters so that the later release can
activate `evo_shares` without another mainnet parameter change.

On testnet, both deployments start at `1790208000` (Thursday, September 24, 2026
00:00:00 UTC).

# Notable changes

## Decentralized masternode shares and multi-party payouts

This release implements the Decentralized Masternode Shares DIP, activating
together with DIP-0026 multi-party payouts as part of the `v24` hard fork.
Before activation there is no behavior change. (#7437, #7340)

### Multi-party payouts

- The `payoutAddress` argument of `protx register`, `protx register_legacy`,
  `protx register_evo`, `protx register_fund`, `protx register_fund_legacy`,
  `protx register_fund_evo` and `protx update_registrar` accepts, in addition to
  a single address, an array of `{"address": ..., "reward": ...}` objects that
  splits the owner reward between several payees, with `reward` given in basis
  points. Multi-party payouts are only valid once the `v24` hard fork activates.
- For ExtAddr (version 3) masternode states and ProTx payloads the payout list
  is reported as a `payouts` array in place of the single `payoutAddress`
  field, which legacy versions keep. Shared masternodes report `shares`
  instead. The fields appear in `protx info`, `protx list detailed`,
  `protx listdiff`, `masternode status` (`dmnState`) and in decoded
  `ProRegTx`/`ProUpRegTx` payloads of the transaction-decoding RPCs;
  `protx diff` includes them only with `extended=true`. (#7340)

### Consensus changes (active with `v24`)

- A version 3 (extended addresses) ProRegTx may carry a collateral share table:
  2 to 8 participants fund the masternode collateral atomically in one
  registration, each recording an immutable amount, refund script and share
  owner key, plus an updatable reward script. Every share owner consents by
  signing a digest that binds the exact funding inputs, all outputs, the share
  table, the penalty terms and the registrar configuration. Consent is tied to
  the shares in the table, not to the funding inputs: signing an input into a
  registration neither requires nor creates a share for its owner. The
  early-period penalty must be below the smallest share and must be zero when
  no early period is configured.
- The shared collateral is paid to the 7-byte template script
  `04445348437551` (`0x04 "DSHC" OP_DROP OP_TRUE`). From activation, an output
  paying this exact script is valid only as the collateral of a valid shared
  registration, and spending such an output is valid only via a ProDisTx.
  Template outputs mined before activation become permanently unspendable.
- Three new special transaction types:
  - **ProDisTx (type 10)** dissolves a shared masternode, refunding every
    participant's principal to its immutable refund script. Exactly one
    signature (unilateral, penalized during the configured early period) or one
    per share (unanimous, penalty-free). Validity is monotone: a ProDisTx that
    is valid at some height is valid at every later height, which makes offline
    "standby dissolutions" safe. The transaction fee is capped at 1000000 duffs
    and a unilateral dissolution may not pay bonuses beyond the configured
    early penalty, bounding what a stolen share owner key can drain from its
    own share.
  - **ProUpShareTx (type 11)** lets one share owner update their reward script.
  - **ProUpSharedRegTx (type 12)** updates the operator key and/or voting key
    with a signature from every share owner. A plain ProUpRegTx is invalid for
    shared masternodes.
- The owner reward of a shared masternode is split across the share table
  proportionally to the recorded contributions (sequential floor, remainder to
  the last entry), paying each share's reward script (or its refund script when
  none is set). Operator rewards are unchanged.
- Withdrawal (asset unlock) transactions may not pay the template script.

### EvoNode restrictions

- EvoNodes cannot register with shared collateral or have more than one owner
  payout until the `evo_shares` deployment activates. An EvoNode ProRegTx with a
  share table is rejected with `bad-protx-shares-evo`, and an EvoNode ProRegTx
  or ProUpRegTx with more than one owner payout is rejected with
  `bad-protx-payouts-evo`. Regular masternodes are not affected.
- The consent digest that share owners sign for a shared EvoNode registration
  also covers the EvoNode's `platformNodeID`. The digest of a regular shared
  registration is unchanged. (#7807)

### Relay policy changes

- The template output relays only as the declared collateral output of a shared
  registration, and a template prevout is accepted only inside a ProDisTx; both
  remain nonstandard everywhere else.

### New RPCs

- `protx shared_register_prepare` builds an unsigned shared registration from a
  caller-supplied funding transaction. The result echoes the decoded terms and
  carries a `warning` when `earlyPenalty` is zero, since any participant can
  then force an early exit at no cost beyond the transaction fee. Before signing
  its funding inputs with `signrawtransactionwithwallet`, each participant should
  confirm that the share table (`proRegTx.shares` in `decoderawtransaction`)
  holds its share with its own amount and addresses.
- `protx shared_register_prepare_evo` creates an unsigned shared EvoNode
  registration. It takes the arguments of `protx shared_register_prepare`
  followed by `platformNodeID`, `platformP2PAddrs` and `platformHTTPSAddrs`, and
  fails until `evo_shares` is active. (#7807)
- `protx shared_sign` signs a shared registration, dissolution or shared
  registrar update with every share owner key the wallet holds. It returns the
  decoded terms being consented to (for a dissolution, including the outputs)
  alongside the signatures, and repeats the zero-penalty warning for a
  registration. It refuses a registration or dissolution carrying an
  unsatisfied lock time or a relative input lock unless `allowTimeLocks` is
  set.
- `protx shared_combine` combines collected signatures and optionally submits.
  A dissolution combined here requires a signature from every share; unilateral
  dissolutions come fully signed from `protx shared_dissolve`.
- `protx shared_dissolve` creates, signs and submits a unilateral ProDisTx (or,
  with `submit=false`, returns hex suitable for offline standby storage).
- `protx shared_dissolve_prepare` builds an unsigned unanimous ProDisTx.
- `protx shared_update_share` updates one share's reward address. To restore
  rewards to the immutable refund address, pass that address explicitly; empty
  reward scripts are not valid in update transactions.
- `protx shared_update_registrar_prepare` builds an unsigned ProUpSharedRegTx.

### Updated RPCs

- `masternodelist` and `masternode list` report comma-separated share owner
  addresses in `owneraddress` for shared masternodes. The `json` and `recent`
  modes can be filtered by any share owner address. Shared registrations and
  masternode state omit the singular `ownerAddress` field in `protx` and decoded
  transaction output; each participant's owner address is in `shares`. (#7437)

### GUI changes

- The Masternodes tab gains a "Shared Masternode…" wizard for registering a
  masternode funded by several people. The coordinator enters the participants,
  masternode settings and exit terms, reserves coins for their own share and
  copies one invitation; every participant pastes it, reserves their coins and
  copies back their details. Two more rounds complete the registration: locked
  terms out and approvals back, then a signing request out and signed
  contributions back. Any message can be pasted from the landing page, replies
  are accepted in any order, and every message shows a short code so
  participants can confirm they hold the same one. Sessions can be saved and
  reopened by either role. Reserved coins are released after broadcast. (#7701)
- Shared masternodes appear in the list with the type "Shared (you hold k of
  n)", a details view listing every share, and context-menu actions to change
  the wallet's reward address, rotate the operator or voting key (with every
  share owner's approval), dissolve now, dissolve together, or create a
  standby dissolution file holding both the penalty-free and the immediate
  variant. Pasting a maintenance request or a standby dissolution into the
  list opens the matching dialog. (#7701)
- The owned-masternode filter includes shared masternodes when the wallet holds
  a participant's refund destination, including when rewards go to a different
  wallet. (#7437)

## Version 2 Asset Unlock transactions

Once the `v24` hard fork activates, Platform withdrawals are issued as version 2
Asset Unlock transactions. Their transaction hash is computed with the quorum
signing fields (`requestedHeight`, `quorumHash`, `quorumSig`) zeroed, so every
instance Platform re-signs after an expiry is the same transaction with one
stable txid. Spends of an unmined withdrawal's outputs therefore stay valid
across re-signs. (#7639)

A version 2 Asset Unlock is InstantSend-locked as soon as it can be mined in
the next block: it carries a valid signature from a recent quorum, is inside
its height window, no other instance of its withdrawal index is in the
mempool, and the withdrawals pending in the mempool fit the credit pool's
current limit. The lock pins the withdrawal index (as the outpoint
`{DIP-27 request id, 0}`) to the txid. Spends of a locked withdrawal are
ordinary InstantSend transactions and the wallet treats the withdrawal's
outputs as trusted, so Platform-to-Core transfers become rapidly respendable.

Version 2 unlocks are relayed by instance hash (new inventory type
`MSG_ASSET_UNLOCK`, protocol version 70242), kept in the mempool while expired
awaiting a re-signed replacement, and committed to by the coinbase transaction
(CbTx version 4, `merkleRootAssetUnlocks`). An expired instance is also
accepted into the mempool (from peers and when reloading `mempool.dat`) if its
signature was valid at a height within its window, so its spends propagate; it
is neither mined nor InstantSend-locked until Platform re-signs it. Relaying an
Asset Unlock outside the receiving node's height window or quorum range is no
longer penalized. (#7639, #7744)

- `getmempoolinfo` reports `pendingassetunlocks`, the sum of the withdrawal
  amounts of the Asset Unlock transactions in the mempool.
- `getassetunlockstatuses` reports `instantlock` for mempooled withdrawals.
- `getrawtransaction` and `decoderawtransaction` report `instanceHash` for
  version 2 Asset Unlock transactions.

## Relative asset unlock limit

- The asset unlock limit that applies once the `v24` hard fork activates is now
  a relative net rule on the credit pool balance instead of a flat 4000 DASH cap
  on gross unlocks per window (576 blocks on mainnet and testnet). Asset unlocks
  may not leave the credit pool below its balance one window earlier minus an
  allowed drop of 20% of that balance, never less than 2000 DASH and with no
  upper bound.
- Deposits and the per-block Platform reward inside the window raise the
  balance and are withdrawable again, so a deposit followed by its withdrawal
  does not consume the allowance of other users.
- The new `getcreditpoolinfo [height]` RPC reports the credit pool balance after
  a block, the balance one window earlier, the window length, the amount
  unlocked inside the window and the asset unlock limit that applies to the
  next block.
- Devnets that have had `v24` active must be reset with fresh datadirs: the
  credit pool limit is part of the EvoDB credit pool snapshot, and a datadir
  written by an earlier version would keep serving the old value at snapshot
  heights. (#7712)

## Version 2 Asset Locks and Platform addresses

- Asset Lock special transactions gain a payload version 2, enabled by the
  `v24` hard fork. Before activation only version 1 is valid; after activation
  both version 1 and version 2 are. A version 2 payload signals that Platform
  assigns each credit output to the matching Identity automatically, and it
  extends the allowed `creditOutputs[].scriptPubKey` forms from P2PKH only to
  P2PKH and P2SH. All other Asset Lock rules are unchanged. See DIP-27.
- `validateaddress` and `getaddressinfo` now accept DIP-18 Dash Platform
  addresses (`dash1k…`/`dash1s…` on mainnet, `tdash1k…`/`tdash1s…` on the test
  chains) and report them with a new `isplatform` field. `getaddressinfo`
  describes such an address against the credit output script an Asset Lock would
  carry for it, so `ismine` and `solvable` answer whether this wallet holds the
  key or script behind the address.
- `sendtoaddress` and `sendmany` accept Platform addresses as recipients. Doing
  so turns the transaction into an Asset Lock with a version 2 payload whose
  credit outputs are the Platform recipients, plus a single empty `OP_RETURN`
  output carrying their total. `subtractfeefromamount` is not supported for
  Platform recipients, because the locked amount has to match the `OP_RETURN`
  value exactly. Paying a Platform address is refused before the `v24` hard
  fork activates.
- `getrawtransaction`, `decoderawtransaction` and the other
  transaction-decoding RPCs report `assetLockTx.creditOutputs[].address` for
  version 2 payloads, the DIP-18 Platform address the credit output pays.
  Version 1 payloads carry no such address and the field is omitted.

This release carries the consensus side of version 2 Asset Locks: after the
`v24` hard fork they are valid in a block. They remain non-standard for now, so
under default policy they are not accepted into the mempool or relayed, and
sending to a Platform address fails with
`Transaction is non-standard (assetlocktx-version-2) …`. A future release will
make them standard once Dash Platform can process them; that is a policy change
only and needs no further consensus change. On testnet, devnets and regtest the
node can be started with `-acceptnonstdtxn=1` to relay them already; mainnet
does not accept that option. (#7294, #7285, #7577)

## Masternode operator keys and BLS schemes

- Dash Core will no longer permit the registration of new legacy BLS scheme
  masternodes after the `v24` hard fork activates. Existing basic scheme
  masternodes will also be prohibited from downgrading to the legacy scheme
  after activation. (#6729)
- Masternode operator key uniqueness is now enforced per key rather than per
  BLS encoding. After the `v24` hard fork activates, registering or updating to
  an operator public key already used by another masternode is rejected
  regardless of which BLS scheme (legacy or basic) either side is encoded under,
  closing a gap where the same key could be claimed twice under different
  encodings.
- After the `v24` hard fork activates, a legacy (LegacyBLS) masternode migrates
  to the basic BLS scheme *in place* when its state version is raised, keeping
  the same operator key: the stored operator public key is re-encoded from the
  legacy to the basic scheme, with no key rotation required. `protx
  update_service` and `protx update_registrar` build the corresponding
  basic-scheme migration transaction for a legacy masternode, and the
  masternode is no longer PoSe-banned by the change. The `..._legacy` RPC
  variants keep the masternode on the legacy scheme. Previously a legacy
  masternode could not move to the basic scheme without effectively rotating
  its operator key, and `update_registrar` clamped the transaction version
  silently. (#7473)

## CoinJoin promotion and demotion

- CoinJoin can now promote and demote between adjacent standard denominations
  within a mixing session after the `v24` hard fork activates. Promotion
  combines 10 inputs of one denomination into 1 output of the next larger
  denomination, while demotion splits 1 input into 10 outputs of the next
  smaller denomination. Pre-v24 behavior remains unchanged.
- Conversions only spend fully-mixed coins, and their outputs start mixing over
  from zero rounds. The 10:1 shape of a conversion publicly clusters one
  participant's coins even inside a mixing transaction, so a converted coin is
  not treated as mixed: it re-enters mixing at its new denomination and
  disperses normally, while the histories of the fully-mixed coins that fed the
  conversion remain protected.
- A mixing session only completes once each side of its denomination is
  occupied by nobody or by at least two participants, since coins are only
  concealed by other coins of the same size on the same side. A session that
  attracts a lone promotion or demotion participant and no counterpart
  therefore waits, and expires in the queue stage without charging anyone's
  collateral, rather than publishing a transaction that would identify that
  participant's coins. Because admission relies on the declared directions, a
  participant whose entry deviates from what it declared has its collateral
  consumed. (#7052)

See [P2P and network changes](#p2p-and-network-changes) for the related
protocol changes.

## Compact quorum proofs

New RPCs `getquorumproofchain` and `verifyquorumproofchain` produce and verify
compact mining-transaction proofs from an independently trusted snapshot, and
`getchainlockbyheight` returns historical coinbase-carried ChainLock
certificates. The RPCs read required blocks on demand, without an additional
index or startup scan, and keep what they read in bounded in-memory caches of up
to about 50 MiB. Nodes must retain the required historical blocks to generate
proofs. (#7107, #7727)

## Deprecated RPC fields now hidden by default

- The deprecated keys `service`, `platformP2PPort` and `platformHTTPPort` now
  require the runtime flag `-deprecatedrpc=service`. (#7181)
  - The following RPCs are affected: `decoderawtransaction`, `decodepsbt`,
    `getblock`, `getrawtransaction`, `gettransaction`, `masternode status`
    (only the `dmnState` key), `protx diff`, `protx listdiff`.
  - This runtime gate also extends to the functionally identical key, `address`
    in `masternode list` (and its alias, `masternodelist`).
  - The same gate applies to `getcoinjoininfo`, `protx info`,
    `protx list detailed` and `quorum info`.
  - `dash-tx -json` output omits these keys as well and offers no way to restore
    them, since `dash-tx` does not accept `-deprecatedrpc`.
- Information on soft fork status has been moved from `getblockchaininfo` to the
  new `getdeploymentinfo` RPC, which allows querying soft fork status at any
  block, rather than just at the chain tip. Inclusion of soft fork status in
  `getblockchaininfo` can currently be restored using the configuration
  `-deprecatedrpc=softforks`, but this will be removed in a future release. Note
  that in either case, the `status` field now reflects the status of the current
  block rather than the next block. A new `/rest/deploymentinfo` REST endpoint
  provides the same information. (#6888)

### Note for masternode operators running Dash Platform

Current versions of Dash Platform read the `service` field and the
`getblockchaininfo` `softforks` field from Dash Core. Operators running an
EvoNode with Dash Platform (including setups managed with dashmate) must start
Dash Core with both `-deprecatedrpc=service` and `-deprecatedrpc=softforks`
until the Platform version they run no longer requires these fields. Removing
either option before then can prevent Platform from operating correctly.

## Descriptor wallets by default

Descriptor wallets are now the default wallet type. Newly created wallets will
use descriptors unless `descriptors=false` is set during `createwallet`, or the
`Descriptor wallet` checkbox is unchecked in the GUI.

Note that wallet RPC commands like `importmulti` and `dumpprivkey` cannot be
used with descriptor wallets, so if your client code relies on these commands
without specifying `descriptors=false` during wallet creation, you will need to
update your code. (#7204)

An experimental RPC `migratewallet` has been added to migrate legacy
(non-descriptor) wallets to descriptor wallets. More information about the
migration process is available in the
[documentation](https://github.com/dashpay/dash/blob/master/doc/managing-wallets.md#migrating-legacy-wallets-to-descriptor-wallets).
(#7275)

## Asynchronous address, spent and timestamp indexes

`TimestampIndex`, `SpentIndex`, and `AddressIndex` have been migrated from
synchronous to asynchronous operation, following the same pattern as `TxIndex`
and other indexes using the `BaseIndex` framework.

When enabling an index for the first time, the node will build the index in the
background while remaining fully operational. Progress can be monitored via the
`getindexinfo` RPC.

Existing nodes with indexes enabled will automatically migrate data from the old
location (block index database) to new separate databases on first startup.
This migration may take 20-40 minutes or longer depending on hardware
specifications and index sizes. The node will log progress during migration.

Breaking changes:

- `SpentIndex` and `AddressIndex` are incompatible with pruned nodes as they
  require access to undo data now.
- `TimestampIndex` can no longer be enabled on a datadir where pruning has
  already occurred. It still works when started together with `-prune` from a
  fresh sync or reindex (the index is built forward as blocks arrive), but
  enabling it on an existing pre-pruned datadir now fails at startup. The old
  synchronous implementation built forward from the current tip and accepted
  that case. (#7101)

## Masternode configuration requirements

A masternode (`-masternodeblsprivkey`) now refuses to start unless:

- `-blocksonly=0`
- `-peerblockfilters=1`
- `-maxuploadtarget=0`
- `-minrelaytxfee`, `-incrementalrelayfee`, `-dustrelayfee` and
  `-bytespersigop` are not above their defaults
- `-datacarrier=1`, `-datacarriersize` is not below its default and
  `-permitbaremultisig=1`
- `-limitancestorcount`, `-limitancestorsize`, `-limitdescendantcount`,
  `-limitdescendantsize`, `-maxmempool` and `-mempoolexpiry` are not below
  their defaults

The relay policy checks do not apply on regtest. (#7754)

In addition, DKG members now also vote a member bad if none of their masternode
connections to it advertise:

- transaction relay (`relay=1` in the `version` message);
- compact block filters (`NODE_COMPACT_FILTERS`). (#7755)

## InstantSend and provider transactions

Masternodes no longer InstantSend-lock provider transactions (ProRegTx,
ProUpServTx, ProUpRegTx, ProUpRevTx and the other provider special
transactions). They are mined once they have been in the mempool for 10
minutes, and transactions that spend their outputs can be locked once the block
that includes the provider transaction is ChainLocked. Software that waits for
an islock on a provider transaction should wait for a ChainLock or
confirmations instead. (#7858)

## Bitcoin Core backports

This release includes a large number of backports from Bitcoin Core, covering
most of the changes from Bitcoin Core v25 and a substantial part of Bitcoin
Core v26, along with selected fixes from later releases. User-visible changes
from these backports are listed in the sections below. Notable additions
include the `sendall` RPC, the experimental `migratewallet` RPC,
`minconf`/`maxconf` options for funding RPCs, faster descriptor wallet rescans
using compact block filters, and stricter validation of configuration options
and RPC parameters.

## Other notable changes

- Synced nodes now recover and relay ChainLocks embedded in newly connected
  blocks, even when the original ChainLock message was missed. This applies
  after v20 activation while ChainLocks are enabled. (#6236)
- `SPORK_3_INSTANTSEND_BLOCK_FILTERING` and `SPORK_9_SUPERBLOCKS_ENABLED` have
  been removed. These sporks were already effectively always enabled on mainnet
  but continued to gate behavior on test networks (testnet, regtest, and
  devnet). The associated functionality (InstantSend conflicting-block rejection
  and superblock payments) is now permanently enabled across all networks, and
  the sporks will no longer appear in the `spork` RPC output. (#7278)
- Multi-key ("M-of-N") spork signing, which was never deployed on any network,
  has been removed. The `sporks.dat` cache format changed; caches written by
  previous versions are discarded on startup and sporks are re-synced from
  peers. (#7505)
- Governance object signatures larger than 96 bytes are now rejected, both from
  the network and from the on-disk governance cache (`governance.dat`). The
  cache format version was bumped for this: on the first start after upgrading,
  the existing cache is discarded and governance objects and votes are synced
  again from the network. (#7831)
- This release also contains many robustness and denial-of-service hardening
  fixes in the P2P, LLMQ, governance and CoinJoin message handlers.

## P2P and network changes

- `PROTOCOL_VERSION` has been bumped to `70242`. Protocol version `70241`
  introduces CoinJoin promotion/demotion signaling and protocol version `70242`
  introduces relay of version 2 Asset Unlock transactions by instance hash.
  (#7052, #7639)
- The `dsa` message gained a version-gated flags field (protocol version 70241)
  declaring which mixing direction a participant intends. A session commits to
  carrying promotion/demotion entries only once a participant is admitted that
  actually declared one, and it becomes closed to pre-70241 clients only from
  that point on; conversely, a session that has already admitted a pre-70241
  client refuses later promotion/demotion participants. Either way the refusal
  happens at acceptance time, before any collateral is committed, so a client
  doing ordinary 1:1 mixing is never turned away from a session simply because
  of who opened it. Unbalanced (promotion/demotion) DSTXes are only announced as
  `dstx` to peers at protocol 70241 or newer, since older peers would reject
  them as structurally invalid and penalize the relayer; those peers are sent a
  plain `tx` announcement instead, so they still receive the transaction without
  the mixing metadata they cannot parse. (#7052)
- Version 2 Asset Unlock transactions are relayed by instance hash using the new
  inventory type `MSG_ASSET_UNLOCK` (protocol version 70242). (#7639)
- Startup now fails if any configured `-bind`, `-whitebind` or the implicit Tor
  onion-service bind (`127.0.0.1:9996` by default) cannot be set up. Dash Core
  v23 and earlier started as long as at least one bind succeeded.
- Nodes configured with `-bind` but no specific `-bind=<addr:port>=onion` now
  refuse to start when `-listenonion` is enabled. This includes nodes without
  Tor configured, since `-listenonion` is enabled by default when listening.
  Shared binds cannot distinguish Tor-forwarded connections from direct
  connections, which can grant Tor peers unintended IP-based whitelist
  permissions. Nodes without `-bind`, including those using only `-whitebind`,
  continue to get the default onion target. Users should add a specific
  `-bind=<addr:port>=onion` to accept incoming Tor connections, or set
  `-listenonion=0` to disable automatic onion service creation. (#7300, #7786)
- `getqrinfo` requests now carry at most 4096 `baseBlockHashes`. A request with
  more is rejected and the sending peer is penalized as misbehaving. No known
  client sends more than a handful. The `quorum rotationinfo` RPC applies the
  same limit and returns an error when it is exceeded. (#7629)

## Mining and mempool policy

- Transactions of size 65 bytes and above are now allowed by mempool and relay
  policy (previously 83 bytes). This better reflects the actual afforded
  protections against CVE-2017-12842 and opens up additional use-cases of
  smaller transaction sizes. (#7666)
- Block template creation now checks credit pool limits across complete
  transaction packages. Because Asset Unlock limits are cumulative, a package
  may exceed the block's limit even though its transactions were accepted
  individually. Such packages are now skipped so miners can continue building a
  template instead of template creation failing. (#7570)
- When a block template includes an operator key change or revocation for a
  masternode, it now places the masternode's pending `ProUpServTx` signed with
  the key being replaced before it. Such a service update could otherwise
  confirm after the change in the same block and restore the previous
  operator's service and payout details. A service update that cannot be placed
  before the change, or that does not fit in the block with it, is left out of
  that block. (#7857)
- The mempool now rejects with `protx-dup` a `ProUpServTx` that spends, directly
  or through other pending transactions, an output of a pending operator key
  change or revocation, which it can only confirm after, or of a pending asset
  lock, asset unlock or MNHF signal, which could keep it out of a block. A
  service update funded from such outputs can be sent once they confirm, signed
  with the operator key that is current then. (#7857)
- `getblocktemplate` now always reports `superblocks_enabled` as `true`. The
  field is retained for backwards compatibility. `getblocktemplate` now requires
  a fully synced node at superblock heights. This check is skipped on test
  networks. (#7278)

## Updated RPCs

- Dash RPCs will no longer permit submitting strings to boolean input fields in
  line with validation enforced on upstream RPCs, where this is already the
  case. Requests must now use unquoted `true` or `false`.
  - The following RPCs are affected: `bls generate`, `bls fromsecret`,
    `coinjoinsalt generate`, `coinjoinsalt set`, `masternode connect`,
    `protx diff`, `protx list`, `protx register`, `protx register_legacy`,
    `protx register_evo`, `protx register_fund`, `protx register_fund_legacy`,
    `protx register_fund_evo`, `protx revoke`, `protx update_registrar`,
    `protx update_registrar_legacy`, `protx update_service`,
    `protx update_service_evo`, `quorum info`, `quorum platformsign`,
    `quorum rotationinfo`, `quorum sign`.
  - This restriction can be relaxed by setting `-deprecatedrpc=permissive_bool`
    at runtime but is liable to be removed in future versions of Dash Core.
    (#7099)
- Numeric arguments of the Dash-specific RPCs (`quorum`, `protx`, `gobject`,
  `masternode` and their sub-commands) must now be passed as JSON numbers.
  Strings containing a number, which earlier versions accepted, are rejected
  with a type error (code -3). The only exception is the `baseBlock` and `block`
  arguments of `protx diff` and `protx listdiff`, which take a block hash or a
  height and therefore still accept a height as a string. There is no runtime
  option to restore the old behaviour; `-deprecatedrpc=permissive_bool` only
  applies to booleans. (#7099)
- Previously the `setban`, `addpeeraddress` and `walletcreatefundedpsbt` methods
  allowed non-boolean and non-null values to be passed as boolean parameters.
  Any string, number, array, or object value that was passed would be treated
  as false. Passing any value except `true`, `false`, or `null` now triggers a
  JSON value is not of expected type error. (#7666)
- `protx listdiff` reports the complete `addresses` object, `platform_p2p` and
  `platform_https` included, when the core P2P address of an EvoNode on legacy
  (pre-extended) addresses changes, so its Platform ports can be followed
  without `-deprecatedrpc=service`. (#7779)
- Normal and Evo `protx` registration and maintenance commands now share a typed
  provider-transaction implementation with other wallet frontends. RPC names and
  successful result formats are unchanged. When a wallet cannot completely sign
  the inputs it selected (e.g. `protx register_submit` run in a different
  wallet than the one that prepared the registration), the command now fails
  with a clear wallet error naming the problem instead of reporting success with
  a partially signed transaction or attempting a broadcast that failed mempool
  acceptance with a bare `-26` error. The external-signing workflow
  (`protx register_prepare` followed by `protx register_submit`) is unchanged.
  `protx update_service` on a masternode whose state does not yield a usable
  default fee source now returns an explicit "specify feeSourceAddress"
  parameter error instead of an internal error. (#7600)
- `quorum dkginfo` accepts an optional `proTxHash` argument and reports, in a
  new `upcoming_dkgs` array, the DKG sessions that masternode will take part in
  whose work block is already mined. Without the argument the local active
  masternode is used, if any. (#7428)
- `getcoinjoininfo` gained a `pending_inputs` field, reporting how many
  successfully mixed inputs are currently kept locked while waiting for the
  finalized mixing transaction spending them to be observed. (#7480)
- `gettxoutsetinfo` no longer returns `block_info.unspendables.bip30`. Dash
  never had transactions overridden by duplicates, so the value was always
  zero. (#7335)
- The `-deprecatedrpc=fees` configuration option has been removed. The top-level
  fee fields `fee`, `modifiedfee`, `ancestorfees` and `descendantfees` are no
  longer returned by RPCs `getmempoolentry`, `getrawmempool(verbose=true)`,
  `getmempoolancestors(verbose=true)` and `getmempooldescendants(verbose=true)`.
  The same fee fields can be accessed through the `fees` object in the result.
  (#7551)
- The `validateaddress` RPC now returns an `error_locations` array for invalid
  addresses, with the indices of invalid character locations in the address (if
  known). For example, this will attempt to locate up to two Bech32 errors, and
  return their locations if successful. Success and correctness are only
  guaranteed if fewer than two substitution errors have been made. The error
  message returned in the `error` field now also returns more specific errors
  when decoding fails. (#7492)
- The `testmempoolaccept` RPC now returns 2 additional results within the
  "fees" result: "effective-feerate" is the feerate including fees and sizes of
  transactions validated together if package validation was used, and also
  includes any modified fees from prioritisetransaction. The
  "effective-includes" result lists the txids of transactions whose modified
  fees and sizes were used in the effective-feerate. (#7684)
- The `verifychain` RPC will now return `false` if the checks didn't fail, but
  couldn't be completed at the desired depth and level. This could be due to
  missing data while pruning, due to an insufficient dbcache or due to the node
  being shutdown before the call could finish. (#7710)
- The `listdescriptors`, `decodepsbt` and similar RPC methods now show `h`
  rather than apostrophe (`'`) to indicate hardened derivation. This does not
  apply when using the `private` parameter, which matches the marker used when
  descriptor was generated or imported. Newly created wallets use `h`. This
  change makes it easier to handle descriptor strings manually. E.g. the
  `importdescriptors` RPC call is easiest to use `h` as the marker:
  `'["desc": ".../0h/..."]'`. With this change `listdescriptors` will use `h`,
  so you can copy-paste the result, without having to add escape characters or
  switch `'` to 'h' manually. Note that this changes the descriptor checksum.
  For legacy wallets the `hdkeypath` field in `getaddressinfo` is unchanged, nor
  is the serialization format of wallet dumps. (#7237, #7338)

## New RPCs

- `getcreditpoolinfo` (see [Relative asset unlock limit](#relative-asset-unlock-limit)). (#7712)
- `getquorumproofchain`, `verifyquorumproofchain` and `getchainlockbyheight`
  (see [Compact quorum proofs](#compact-quorum-proofs)). (#7107)
- `getdeploymentinfo` and the `/rest/deploymentinfo` REST endpoint (see
  [Deprecated RPC fields now hidden by default](#deprecated-rpc-fields-now-hidden-by-default)). (#6888)
- `protx shared_*` (see [Decentralized masternode shares and multi-party payouts](#decentralized-masternode-shares-and-multi-party-payouts)). (#7437, #7807)
- `migratewallet` (see [Descriptor wallets by default](#descriptor-wallets-by-default)). (#7275)
- The `sendall` RPC spends specific UTXOs to one or more recipients without
  creating change. By default, the `sendall` RPC will spend every UTXO in the
  wallet. `sendall` is useful to empty wallets or to create a changeless payment
  from select UTXOs. When creating a payment from a specific amount for which
  the recipient incurs the transaction fee, continue to use the
  `subtractfeefromamount` option via the `send`, `sendtoaddress`, or `sendmany`
  RPCs. (#7131)

## Updated settings

- If the `-checkblocks` or `-checklevel` options are explicitly provided by the
  user, but the verification checks cannot be completed due to an insufficient
  dbcache, Dash Core will now return an error at startup. (#7710)
- `dashd` and `dash-qt` will now raise an error on startup if a datadir that is
  being used contains a dash.conf file that will be ignored, which can happen
  when a datadir= line is used in a dash.conf file. The error message is just a
  diagnostic intended to prevent accidental misconfiguration, and it can be
  disabled (`-allowignoredconf`) to restore the previous behavior of using the
  datadir while ignoring the dash.conf contained in it. (#7710)
- Passing an invalid `-debug`, `-debugexclude`, or `-loglevel` logging
  configuration option now raises an error, rather than logging an easily missed
  warning. (#7710)
- Setting `-blocksonly` will now reduce the maximum mempool memory to 5MB (users
  may still use `-maxmempool` to override). Previously, the default 300MB would
  be used, leading to unexpected memory usage for users running with
  `-blocksonly` expecting it to eliminate mempool memory usage. As unused
  mempool memory is shared with dbcache, this also reduces the dbcache size for
  users running with `-blocksonly`, potentially impacting performance. (#7684)
- Non-standard transactions are now disabled by default on testnet and devnets
  for relay and mempool acceptance. The previous behaviour can be re-enabled by
  setting `-acceptnonstdtxn=1`. (#7710)

## Command-line options

### New options

- The dust attack protection introduced in v23.1.0 as a GUI option is now
  available to `dashd` as well through `-dustprotectionthreshold=<n>`: UTXOs
  received from external transactions whose value is at or below `<n>` duffs
  are locked automatically by the wallet, so they are not spent together with
  other coins. It is disabled by default (`0`). The GUI option and the
  command-line option control the same setting. Locks persist across restarts
  and are not released when the threshold is lowered or the option is removed;
  use `lockunspent` to unlock such outputs manually. (#7265)
- `-allow-deprecated-long-mnemonics` (see [Wallet](#wallet)). (#7842)

### Changed options

- `-sporkaddr` now configures a single spork address. Previously it could be
  specified multiple times to configure a set of valid spork signers. (#7505)

### Removed options

- `-minsporkkeys` has been removed. Multi-key ("M-of-N") spork signing was never
  deployed on any network — every chain shipped a single spork address with a
  threshold of one — and the implementation has been removed. (#7505)
- The `-rescan` startup parameter has been removed. Wallets which require
  rescanning due to corruption will still be rescanned on startup. Otherwise,
  please use the `rescanblockchain` RPC to trigger a rescan.

## Wallet

- Creating an HD wallet with a mnemonic passphrase longer than 248 bytes (not
  characters) through `upgradetohd`, `createwallet` or `-mnemonicpassphrase` is
  now rejected, and one longer than 256 bytes is always rejected. Existing
  wallets are not affected. (#7842)

  Seed derivation uses the BIP39 salt `"mnemonic"` followed by the passphrase.
  Dash Core 19.2.0 started cutting that salt at 256 bytes to stop a crash, so
  every passphrase byte after the 248th has been ignored since then. Two
  passphrases that differ only after that point derive the same seed. Before
  23.0.0 the salt was also cut at the first NUL byte.

  The derivation is left as it is. Using the full passphrase would bring back
  the pre-19.2.0 derivation, and a wallet created with a long passphrase on
  19.2.0 or later could then no longer be recovered from its mnemonic. Instead,
  new wallets can't be given a passphrase that the derivation would truncate.

  To recover a wallet that was created on 19.2.0 or later with a passphrase of
  249 to 256 bytes, start the node with the new
  `-allow-deprecated-long-mnemonics` option, which accepts passphrases of up to
  256 bytes and derives the same (truncated) seed as before. A wallet created
  before 19.2.0 with a passphrase longer than 248 bytes can't be recovered from
  its mnemonic on current versions, with or without the option. Restore it from
  a wallet backup instead.
- After a mixing session completes successfully, the inputs a client
  contributed to it are no longer released as soon as the `DSCOMPLETE` message
  is processed. They stay locked until the wallet observes a transaction
  spending them, or for at most an hour if that transaction never propagates.
  Previously the completion message routinely overtook the trickle-relayed
  mixing transaction, leaving a window in which another session (with
  `-coinjoinmultisession=1`, where the one-block cooldown does not apply) could
  select and sign the very same inputs and produce a second, conflicting
  CoinJoin transaction.

  These locks are persisted, so they survive a restart. They are tracked in a
  new wallet database record, `cj_pending_obs`, which is written only when a
  mixing session completes successfully. Older clients loading such a wallet
  ignore the record, counting it as an unknown record; the accompanying
  `lockedutxo` entries are understood by them regardless, so the inputs stay
  locked either way. Locks the user set themselves via `lockunspent` are never
  adopted or released by this mechanism, and unlocking a pending input manually
  purges it from the pending set. (#7480)
- CoinJoin denomination counts, average mixing rounds and the normalized
  anonymized balance no longer include outputs of transactions that cannot
  confirm as they stand: conflicted ones, and ones that were abandoned, never
  broadcast, or rejected from the mempool. Previously such outputs inflated
  these figures, which could make the wallet create fewer denominations than
  intended and report mixing progress that did not match the coins it could
  actually use. (#7634)
- Rescans for descriptor wallets are now significantly faster if compact block
  filters (BIP158) are available. Since those are not constructed by default on
  regular nodes, the configuration option `-blockfilterindex=1` has to be
  provided to take advantage of the optimization. This improves the performance
  of the RPC calls `rescanblockchain`, `importdescriptors` and `restorewallet`.
  (#7085)
- Address purpose strings are now restricted to the currently known values of
  "send", "receive", and "refund". Wallets that have unrecognized purpose
  strings will have loading warnings, and the `listlabels` RPC will raise an
  error if an unrecognized purpose is requested. (#7871)

## RPC Wallet

- `fundrawtransaction`, `send`, `sendall` and `walletcreatefundedpsbt` accept a
  `use_cj` option, previously available only on `sendtoaddress` and `sendmany`,
  that restricts coin selection to fully mixed CoinJoin outputs. `send` and
  `sendall` record the resulting transaction as a CoinJoin spend (`DS` in
  `gettransaction`), as `sendtoaddress` and `sendmany` already do. (#7261)
- The `minconf` option, which allows a user to specify the minimum number of
  confirmations a UTXO being spent has, and the `maxconf` option, which allows
  specifying the maximum number of confirmations, have been added to the
  following RPCs: `fundrawtransaction`, `send`, `walletcreatefundedpsbt` and
  `sendall`. (#7709)
- The `listsinceblock`, `listtransactions` and `gettransaction` output now
  contain a new `parent_descs` field for every "receive" entry. A new optional
  `include_change` parameter was added to the `listsinceblock` command. (#7550)
- RPC `listsinceblock` now accepts an optional `label` argument to fetch
  incoming transactions having the specified label. (#7648)
- RPC `getreceivedbylabel` now returns an error, "Label not found in wallet"
  (-4), if the label is not in the address book. (#7550)
- RPC `listunspent` now has a new argument `include_immature_coinbase` to
  include coinbase UTXOs that don't meet the minimum spendability depth
  requirement (which before were silently skipped). (#7646)
- RPC `walletprocesspsbt` return object now includes field `hex` (if the
  transaction is complete) containing the serialized transaction suitable for
  RPC `sendrawtransaction`. (#7354)
- The `signrawtransactionwithkey`, `signrawtransactionwithwallet`,
  `walletprocesspsbt` and `descriptorprocesspsbt` calls now return the more
  specific RPC_INVALID_PARAMETER error instead of RPC_MISC_ERROR if their
  sighashtype argument is malformed. (#7710)
- In the `createwallet`, `loadwallet`, `unloadwallet`, and `restorewallet` RPCs,
  the "warning" string field is deprecated in favor of a "warnings" field that
  returns a JSON array of strings to better handle multiple warning messages and
  for consistency with other wallet RPCs. The "warning" field will be fully
  removed from these RPCs in v25. It can be temporarily re-enabled during the
  deprecation period by launching dashd with the configuration option
  `-deprecatedrpc=walletwarningfield`. (#7711)

## GUI changes

- Dash-Qt can now register and maintain regular masternodes and EvoNodes from
  the Masternodes tab. The registration wizard supports wallet-funded,
  wallet-owned, and externally held collateral, while row actions provide
  Update Service, Update Registrar, and Revoke workflows. Generated operator
  keys are shown and confirmed before registration; wallet-seed-derived
  operator keys are not required. (#7618)
- Shared masternodes can be registered and maintained from the Masternodes tab
  (see [Decentralized masternode shares and multi-party payouts](#decentralized-masternode-shares-and-multi-party-payouts)). (#7701)
- Dash-Qt now labels masternode registration and update transactions in the
  transaction history as **Masternode Registration** and **Masternode Update**
  instead of generic "Payment to yourself" rows. A new **Masternode** filter
  shows only these operations. The amount shown is the transaction's net effect
  on your wallet (for example, the network fee on a self-funded registration).
  (#7595)
- The Governance tab now shows the wallet's current weighted proposal votes and
  voting deadline estimate. The Votes dialog shows each eligible masternode's
  current vote and allows voting with selected masternodes. (#7694)

## Build system

- macOS binaries now target macOS 14 (Sonoma) (see [Compatibility](#compatibility)).
- The `--enable-gprof` configure option has been removed.

## Library

- `libdashconsensus` is deprecated and will be removed in a future release. It
  has existed for nearly 10 years with very little known uptake or impact, and
  has become a maintenance burden. The underlying functionality does not change
  between versions, so any users of the library can continue to use the final
  release indefinitely, with the understanding that it will not receive future
  consensus updates. (#7378)

See detailed [set of changes][set-of-changes].

# Credits

Thanks to everyone who directly contributed to this release:

- Antonio Moratti
- Kittywhiskers Van Gogh
- Konstantin Akimov
- PastaClaw
- PastaPastaPasta
- takeshikurosawaa
- thephez
- UdjinM6
- Vijay
- zxccxccxz

As well as everyone that submitted issues, reviewed pull requests and helped
debug the release candidates.

# Older releases

These releases are considered obsolete. Old release notes can be found here:

- [v23.1.9](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-23.1.9.md) released Oct/29/2026
- [v23.1.8](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-23.1.8.md) released Jul/29/2026
- [v23.1.7](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-23.1.7.md) released Jun/30/2026
- [v23.1.5](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-23.1.5.md) released Jun/19/2026
- [v23.1.4](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-23.1.4.md) released Jun/18/2026
- [v23.1.3](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-23.1.3.md) released May/28/2026
- [v23.1.2](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-23.1.2.md) released Mar/12/2026
- [v23.1.0](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-23.1.0.md) released Feb/15/2026
- [v23.0.2](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-23.0.2.md) released Dec/4/2025
- [v23.0.0](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-23.0.0.md) released Nov/10/2025
- [v22.1.3](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-22.1.3.md) released Jul/15/2025
- [v22.1.2](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-22.1.2.md) released Apr/15/2025
- [v22.1.1](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-22.1.1.md) released Feb/17/2025
- [v22.1.0](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-22.1.0.md) released Feb/10/2025
- [v22.0.0](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-22.0.0.md) released Dec/12/2024
- [v21.1.1](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-21.1.1.md) released Oct/22/2024
- [v21.1.0](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-21.1.0.md) released Aug/8/2024
- [v21.0.2](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-21.0.2.md) released Aug/1/2024
- [v21.0.0](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-21.0.0.md) released Jul/25/2024
- [v20.1.1](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-20.1.1.md) released April/3/2024
- [v20.1.0](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-20.1.0.md) released March/5/2024
- [v20.0.4](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-20.0.4.md) released Jan/13/2024
- [v20.0.3](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-20.0.3.md) released December/26/2023
- [v20.0.2](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-20.0.2.md) released December/06/2023
- [v20.0.1](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-20.0.1.md) released November/18/2023
- [v20.0.0](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-20.0.0.md) released November/15/2023
- [v19.3.0](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-19.3.0.md) released July/31/2023
- [v19.2.0](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-19.2.0.md) released June/19/2023
- [v19.1.0](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-19.1.0.md) released May/22/2023
- [v19.0.0](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-19.0.0.md) released Apr/14/2023
- [v18.2.2](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-18.2.2.md) released Mar/21/2023
- [v18.2.1](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-18.2.1.md) released Jan/17/2023
- [v18.2.0](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-18.2.0.md) released Jan/01/2023
- [v18.1.1](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-18.1.1.md) released January/08/2023
- [v18.1.0](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-18.1.0.md) released October/09/2022
- [v18.0.2](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-18.0.2.md) released October/09/2022
- [v18.0.1](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-18.0.1.md) released August/17/2022
- [v0.17.0.3](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.17.0.3.md) released June/07/2021
- [v0.17.0.2](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.17.0.2.md) released May/19/2021
- [v0.16.1.1](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.16.1.1.md) released November/17/2020
- [v0.16.1.0](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.16.1.0.md) released November/14/2020
- [v0.16.0.1](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.16.0.1.md) released September/30/2020
- [v0.15.0.0](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.15.0.0.md) released Febrary/18/2020
- [v0.14.0.5](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.14.0.5.md) released December/08/2019
- [v0.14.0.4](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.14.0.4.md) released November/22/2019
- [v0.14.0.3](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.14.0.3.md) released August/15/2019
- [v0.14.0.2](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.14.0.2.md) released July/4/2019
- [v0.14.0.1](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.14.0.1.md) released May/31/2019
- [v0.14.0](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.14.0.md) released May/22/2019
- [v0.13.3](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.13.3.md) released Apr/04/2019
- [v0.13.2](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.13.2.md) released Mar/15/2019
- [v0.13.1](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.13.1.md) released Feb/9/2019
- [v0.13.0](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.13.0.md) released Jan/14/2019
- [v0.12.3.4](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.12.3.4.md) released Dec/14/2018
- [v0.12.3.3](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.12.3.3.md) released Sep/19/2018
- [v0.12.3.2](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.12.3.2.md) released Jul/09/2018
- [v0.12.3.1](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.12.3.1.md) released Jul/03/2018
- [v0.12.2.3](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.12.2.3.md) released Jan/12/2018
- [v0.12.2.2](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.12.2.2.md) released Dec/17/2017
- [v0.12.2](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.12.2.md) released Nov/08/2017
- [v0.12.1](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.12.1.md) released Feb/06/2017
- [v0.12.0](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.12.0.md) released Aug/15/2015
- [v0.11.2](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.11.2.md) released Mar/04/2015
- [v0.11.1](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.11.1.md) released Feb/10/2015
- [v0.11.0](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.11.0.md) released Jan/15/2015
- [v0.10.x](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.10.0.md) released Sep/25/2014
- [v0.9.x](https://github.com/dashpay/dash/blob/master/doc/release-notes/dash/release-notes-0.9.0.md) released Mar/13/2014

[set-of-changes]: https://github.com/dashpay/dash/compare/v23.1.9...dashpay:v24.0.0
