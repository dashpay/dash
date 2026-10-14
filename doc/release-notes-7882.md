Pruning
-------

- Pruned nodes now keep the blocks that block validation reads back from disk
  for the credit pool and for quorum members. From V20 on that is at least the
  last 2074 blocks on mainnet, testnet and devnet, instead of 288. Before, a
  node pruned below that could reject a valid block and stop following the
  chain. At current mainnet block sizes this is far below the minimum `-prune`
  target, so it mostly affects manual pruning: `pruneblockchain` now returns a
  lower height when the requested height is within that range. A pruned node
  now refuses to start, and asks to restart with `-reindex`, if blocks that it
  needs to validate the next blocks were already pruned, for example by an
  earlier release. A node that has already rejected a valid block this way also
  needs `-reindex`. (#7882)
