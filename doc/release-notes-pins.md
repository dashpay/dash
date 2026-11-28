Updated RPCs
------------

- `loadtxoutset`, `getchainstates` and `dumptxoutset` are no longer hidden, and
  `loadtxoutset` and `getchainstates` now work on mainnet and testnet as well as
  regtest. `loadtxoutset` loads an AssumeUTXO snapshot so that the node is usable at
  the network tip within minutes, while it validates the chain up to the snapshot in
  the background. Mainnet accepts the snapshot at height 2579482 and testnet the one
  at height 1599687. Pruned nodes, masternodes and nodes with `-addressindex`,
  `-spentindex` or `-timestampindex` refuse to load a snapshot. See
  `doc/design/assumeutxo.md`. (#pins)
