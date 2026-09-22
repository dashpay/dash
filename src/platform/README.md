# Dash Platform client library (GUI-only)

This directory contains a Qt-free C++ client for Dash Platform (Evolution),
used exclusively by the dash-qt GUI when configured with
`--enable-platform-gui`. It provides:

- per-network parameters and the well-known system data contract IDs
  (`params.*`);
- the `PlatformClient` interface the GUI drives (`client.h`) and its
  production implementation (`client.cpp`): a worker thread over the Dash
  Platform SDK's C++ bindings (`dash-platform-cxx`, namespace `platform_ffi`,
  built from dashpay/platform through depends). The SDK owns query
  construction, DAPI transport (TLS to the evonodes), retries, proof
  verification (GroveDB replay plus the Tenderdash quorum signature against
  the quorum keys this node pushes from its LLMQ store), protocol-version
  tracking and the chain-id / ChainLock freshness checks. This node supplies
  the evonode endpoints from its deterministic masternode list, the Platform
  quorum keys, its best ChainLock height and wallet signatures;
- thin adapters (`dpp/`) over the same bindings for DPP object decoding and
  state-transition construction, signed through a digest callback so private
  keys never leave the wallet;
- the wallet record formats the GUI persists (`walletrecords.*`).

## Isolation rules

- Nothing in this directory may be linked into `dashd`, `dash-cli`,
  `dash-tx`, `dash-wallet` or any consensus/wallet library. It is linked into
  `dash-qt` and `test_dash` only, and only under `--enable-platform-gui`.
- Consensus, wallet and node code must not include headers from here. The GUI
  (`src/qt/platform/`) is the only consumer.
- Code here may depend on `src/crypto`, `src/util` and the standard library.
  It must not depend on Qt.

Upstream references are pinned in code comments (dashpay/platform). The
protocol version transitions are built under is the one the SDK has seen the
network run, ratcheted upward from the per-network floor in `params.cpp`.
