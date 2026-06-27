# Daily Sync + Reindex CI

The `Daily Sync + Reindex` workflow
(`.github/workflows/sync-reindex.yml`) runs a full-node regression check
once per day. It rebuilds `dashd` and `dash-cli` from the latest `develop`
of `dashpay/dash`, resumes a persistent on-disk chain, advances to tip,
restarts the node with `-reindex`, and verifies the node
converges back to the same or a higher tip.

The validation logic lives in
[`contrib/devtools/sync_reindex_check.sh`](../contrib/devtools/sync_reindex_check.sh)
and can be run by hand on any host with a built tree.

## Why a stateful self-hosted runner

Syncing testnet or mainnet from genesis takes hours-to-days. Hosted
GitHub-Actions runners are wiped between jobs, so they would either
re-sync from scratch on every run (uselessly slow and prone to
timing-based false negatives) or be skipped entirely. The workflow is
intentionally targeted at a **stateful self-hosted Linux runner** whose
datadir survives across runs.

Each daily invocation therefore measures the day's incremental delta plus
a fresh `-reindex` over the accumulated chain — exactly the regression
we care about.

## Runner setup

### 1. Register a self-hosted runner

Register a runner on the repository with a label that uniquely identifies
it as the sync/reindex host. The workflow defaults to the label
`self-hosted-sync-reindex`; you can override this through the
`RUNNER_SYNC_REINDEX` repository variable or the workflow's `runner`
dispatch input.

The host should provide:

* Linux x86_64 or aarch64.
* A C++ toolchain capable of building Dash Core (see
  [`doc/build-unix.md`](build-unix.md)). The workflow uses the in-tree
  `depends` system to avoid relying on distro package versions.
* At least 200 GB of free disk for mainnet, 50 GB for testnet.
* Enough RAM/CPU to keep up with the network at idle (4 cores / 8 GB is
  the practical floor for testnet).
* Outbound network connectivity for peering and for fetching depends
  sources during the build.

### 2. Provision a persistent datadir

Create a directory the runner user can read and write that is **not**
inside the runner work directory (the work directory is cleaned between
jobs). The default path expected by the workflow is:

```
/var/lib/dashcore-ci/sync-reindex
```

The workflow stores each configured run under a stable subdirectory of the
base path (for example `test/` for `network=test`); Dash Core still creates
its own network subdirectory such as `testnet3/` inside that datadir.

Override the base path with the `SYNC_REINDEX_DATADIR` repository
variable when the runner stores the chain elsewhere (e.g. on a mounted
data disk).

The directory must exist and be writable before the workflow runs; the
sanity-check step fails fast otherwise.

### 3. Configure an external tip oracle

The workflow requires `SYNC_REINDEX_EXTERNAL_TIP_URL`, or the matching
`external_tip_url` dispatch input. It should point at an HTTP endpoint
that returns either a bare integer block height or JSON with a `.height`
field.

The standalone script can run without an oracle. In that fallback mode it
considers the node "synced" once
`getblockchaininfo` reports `blocks == headers`, `initialblockdownload`
is `false`, all enabled indexes from `getindexinfo` are synced to at least
the chain height, and the height is above a conservative network-specific
floor (one million for testnet, two million for mainnet). Tune the floor
with `--min-headers` if needed. The workflow does not allow that fallback
because a stateful runner could otherwise pass at yesterday's local tip.
When an oracle is set, the script requires the local tip to be within
`--tip-tolerance` blocks of it (default: 50) before a phase is considered
converged, and any fetch or parse failure is fatal.

### 4. (Optional) Tune build parallelism and extra args

`SYNC_REINDEX_MAKEJOBS` (e.g. `-j8`) overrides the default `-j4` passed
to `make`. Keep at least one core free for the running node if the host
is also serving other workloads.

`SYNC_REINDEX_EXTRA_DASHD_ARGS` is appended to every `dashd` invocation.
This is mainly for uncommon networks. For example, Dash requires explicit
`-port=<n>` and `-rpcport=<n>` when running `-devnet`, and named devnets
should include `-devnet=<name>`. The script mirrors `-devnet=<name>` and
`-rpcport=<n>` into `dash-cli` so RPC calls target the same node.

## What the workflow runs

1. Checks out `dashpay/dash@develop` (or the ref supplied via dispatch).
2. Builds `depends` (no Qt, no wallet UI) and configures the source
   tree.
3. Builds only `src/dashd` and `src/dash-cli`.
4. Invokes `contrib/devtools/sync_reindex_check.sh` against the
   persistent datadir.

The script:

* Starts `dashd` and waits for the RPC to come up.
* Polls `getblockchaininfo` / `getconnectioncount` until the node
  converges (`blocks == headers`, `ibd == false`) or a per-phase timeout
  expires, and requires every enabled `getindexinfo` index to be synced to
  at least that chain height. When an external tip oracle is configured,
  local convergence at an older persisted tip is not enough; the script keeps
  polling until the local tip is also within tolerance of the oracle.
* Calls `dash-cli stop`, then restarts the node with the reindex flag
  (`-reindex` by default).
* Waits for the reindexed node to converge again.
* Verifies the post-reindex tip has not regressed below the post-sync
  tip and that the post-sync block hash is still present at the same height
  after reindex. This baseline check is strict; the external-tip tolerance
  applies only to comparison with the oracle.

### Why `-reindex` by default

Dash enables `-txindex` by default, and startup rejects
`-reindex-chainstate` while `-txindex` is active because chainstate-only
replay can corrupt indexes. The daily workflow therefore uses full
`-reindex`: it replays existing block files end-to-end, rebuilds indexes,
and still avoids re-downloading blocks from peers.

Maintainers can dispatch with `reindex_flag=-reindex-chainstate` only for
a datadir that has compatible index settings from the initial sync onward,
for example with `extra_args="-txindex=0"`.

## Running locally

```
DATADIR=/var/lib/dashcore-ci/sync-reindex/test \
EXTERNAL_TIP_URL=https://example.test-explorer.invalid/status \
./contrib/devtools/sync_reindex_check.sh \
    --dashd ./src/dashd \
    --dash-cli ./src/dash-cli \
    --datadir "$DATADIR" \
    --network test \
    --mode both
```

All flags are also configurable via environment variables; run with
`--help` for the full list.

## Interpreting failures

Each failure prints a phase label, the most recent
`getblockchaininfo` snapshot, and a tail of the dashd log. Exit codes
are stable and meaningful:

| Exit | Meaning |
| ---- | ------- |
| 0    | Sync and reindex both succeeded and tip checks pass. |
| 1    | Misconfiguration or dashd failed to start (RPC never came up). |
| 2    | Sync phase failed — timeout or stagnation. |
| 3    | Reindex phase failed — timeout, stagnation, or shutdown hang. |
| 4    | A phase finished but its tip is below the expected height. |

When triaging a red run:

1. **Download the `sync-reindex-logs-*` artifact** from the workflow
   run. It contains generated tail files for the script log and any
   `debug.log` files under the network datadir, plus `depends-build.log`,
   `configure.log`, and `build.log`.
2. **Confirm whether the build itself broke** (exit 1 with no
   `getblockchaininfo` output, or a non-empty configure/build log
   tail). If so, the failure is a develop-branch build regression, not
   a sync regression.
3. **Check the last poll line for `peers=`.** A node that converged at
   a low height with `peers=0` is almost always a runner-network
   problem, not a Dash Core bug — verify the runner can still reach
   seeds.
4. **For exit 3 (reindex), inspect `debug.log` around the restart
   timestamp.** Reindex regressions usually surface as either an
   assertion in evodb/quorum code or as a stall while replaying a
   specific block height.
5. **Compare the post-sync and post-reindex tips** in the script log.
   A reindex that lands below the synced tip, or reports a different hash at
   the post-sync baseline height, indicates corruption or divergence during
   the chainstate rebuild.

If the chain on disk has become corrupt in a way that is not the bug
under investigation, ssh to the runner and either remove the
network-specific subdirectory of `SYNC_REINDEX_DATADIR` or rename it for
later forensic work, then re-dispatch the workflow. The next run will
re-sync from genesis, which is slow but self-healing.

## Limitations

* This workflow does not run functional tests or unit tests; that is
  intentional — those are covered by the existing `CI` and `Guix
  Build` workflows.
* `mode=sync` alone is the right choice if the runner's chain is
  already known-good and the only thing being validated is incremental
  follow-up; `mode=reindex` alone skips the daily catch-up and only
  exercises the reindex code path. The default `both` is what gives
  the regression signal.
* The conservative `MIN_HEADERS` floor is only a standalone-script
  fallback. The scheduled workflow requires an external tip URL so it
  cannot pass at a stale local height.
