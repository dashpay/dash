# Dash Platform client library (GUI-only)

This directory contains a Qt-free C++ client for Dash Platform (Evolution),
used exclusively by the dash-qt GUI when configured with
`--enable-platform-gui`. It provides:

- per-network parameters and the well-known system data contract IDs
  (`params.*`);
- a protobuf wire-format subset for the DAPI gRPC messages;
- thin adapters (`dpp/`, `drive/queries.*`) over the Platform-owned CXX
  bindings (namespace `platform_ffi`) for proved-response
  verification, DPP object decoding and state-transition construction —
  backed by the real dashpay/platform crates. Verification hands the exact
  protobuf (request, response) pair to drive-proof-verifier's `FromProof`,
  which replays the GroveDB proof and checks the Tenderdash quorum
  threshold signature against locally synced LLMQ keys;
- a DAPI client speaking gRPC-Web over HTTP/1.1 + TLS (mbedtls) to evonodes.

## Isolation rules

- Nothing in this directory may be linked into `dashd`, `dash-cli`,
  `dash-tx`, `dash-wallet` or any consensus/wallet library. It is linked into
  `dash-qt` and `test_dash` only, and only under `--enable-platform-gui`.
- Consensus, wallet and node code must not include headers from here. The GUI
  (`src/qt/platform/`) is the only consumer.
- Code here may depend on `src/crypto`, `src/util`, `src/bls` (dashbls),
  `src/secp256k1` and the standard library. It must not depend on Qt.

Upstream references are pinned in code comments (dashpay/platform, grovedb).
All protocol logic is pinned to a single Platform protocol version and must be
re-validated against Rust-generated test vectors when Platform upgrades.
