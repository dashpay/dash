Pruning
-------

- Pruned nodes now keep the block history needed to rebuild the credit pool after a restart:
  back to one withdrawal window (576 blocks on mainnet and testnet) before the newest
  credit-pool snapshot on disk that is at or below the deepest block a reorg could return to.
  This is typically around 1,400 blocks instead of 288, still far below the 550 MiB minimum
  prune target. The kept range moves forward as the tip advances and new snapshots are written
  to disk, so it can trail the tip by up to about a day of blocks.
- A node that upgrades from a version before v24.0.0-rc.1 may have no recent credit-pool
  snapshots on disk. Pruning then pauses until the next snapshot is written, which takes one to
  two days.
- If credit-pool history that the node needs has already been pruned, the node now stops with
  an error asking for `-reindex`. Previously it marked a valid block as invalid and stopped
  following the chain.
