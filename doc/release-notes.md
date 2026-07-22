# Dash Core version v23.1.8

This is a patch release with additional security hardening, networking and
CoinJoin reliability fixes, and compatibility updates for newer build and CI
toolchains.
This release is **recommended** for all nodes, and especially for masternodes.

Please report bugs using the issue tracker at GitHub:

  <https://github.com/dashpay/dash/issues>

# Upgrading and downgrading

## How to Upgrade

If you are running an older version, shut it down. Wait until it has completely
shut down (which might take a few minutes for older versions), then run the
installer (on Windows) or just copy over /Applications/Dash-Qt (on Mac) or
dashd/dash-qt (on Linux).

## Downgrade warning

### Downgrade to a version < v23.0.0

Downgrading to a version older than v23.0.0 is not supported, and will
require a reindex.

# Release Notes

## Security and P2P hardening

This release adds bounds and request authorization to several peer-to-peer
message paths. These changes do not alter consensus or put funds at risk; they
reduce the ability of remote peers to consume excessive memory or CPU, retain
unbounded work, or send unsolicited governance data.

- Added shared bounded-vector deserialization and applied it to SPORK signatures,
  bloom filters, filteradd payloads, quorum data, LLMQ signing messages, and
  CoinJoin entry and final-signature messages. Oversized wire counts are rejected
  before allocating or decoding their full contents.
- Bounded pending recovered-signature and signature-share queues, the number and
  size of unverified signature-share batches, and aggregate batched-signature
  intake.
- Bounded the ChainLock seen cache.
- Governance vote-sync requests are throttled per object. Governance object and
  vote responses are accepted only when the sending peer announced the item or
  was asked for it through the existing per-peer request tracker.
- Compact-block relay was hardened against mutated blocks, malformed or empty
  headers, reconstruction failures, and parallel-download state corruption.
- Block and block-transaction disk reads no longer hold `cs_main`, reducing lock
  contention while serving peers.

## CoinJoin and wallet

- Fixed CoinJoin client lifetime handling by executing client callbacks while the
  wallet-manager map remains locked, avoiding dangling client pointers during
  wallet removal.
- CoinJoin initialization now follows wallet addition, the configured CoinJoin
  preference remains consistent across UI and command-line paths, and locked
  wallets are no longer automatically started for mixing.
- Moved mixing state onto the wallet to avoid a wallet-manager lock-order
  deadlock, with regression coverage for new-keypool callbacks and the
  wallet-manager lock-order cycle.
- CoinJoin wire and semantic limits are separated so protocol payloads are
  rejected at the appropriate boundary without disrupting other session
  participants.

## GUI and RPC

- The masternode PoSe score remains visible when banned masternodes are hidden.
- The CoinJoin toggle now reflects externally started mixing and remains on the
  Start action when a local start attempt fails.
- Internal platform-address migration no longer produces spurious `protx
  listdiff` changes. When deprecated platform port fields are present alongside
  migrated address data, `platformP2PPort` and `platformHTTPPort` report the
  corresponding non-zero values instead of stale zeroes.

## Build, CI, and developer tooling

- Fixed the depends Freetype build with CMake 4 compatibility requirements.
- Updated GitHub Actions used by repository workflows to Node 24-compatible
  releases while preserving the release branch's existing Docker action pins.
- Updated the circular-dependency checker for Python 3.15, including platforms
  where process forking is unavailable.
- Retained the GCC 15/16 compatibility and LevelDB fixes already shipped in
  v23.1.7.

## Tests

New and expanded unit, fuzz, and functional coverage exercises bounded vector
serialization, LLMQ queues and message intake, governance request authorization,
ChainLock cache eviction, CoinJoin boundaries and wallet callbacks, SPORK and
bloom-filter limits, compact-block reconstruction, and mutated blocks.

# v23.1.8 Change log

See detailed [set of changes][set-of-changes].

# Credits

Thanks to everyone who directly contributed to this release:

- Claude Code
- Konstantin Akimov
- MarcoFalke
- pasta
- PastaClaw
- UdjinM6

As well as everyone that submitted issues, reviewed pull requests and helped
debug the release candidates.

# Older releases

These releases are considered obsolete. Old release notes can be found here:

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

[set-of-changes]: https://github.com/dashpay/dash/compare/v23.1.7...dashpay:v23.1.8
