Pruning
-------

- Pruned nodes on mainnet, testnet and devnets now keep at least four days of
  blocks (2304) below the tip instead of 288. Validation reads older blocks
  from disk: the credit pool is rebuilt from its newest daily snapshot after a
  restart, and rotated quorum membership reads the work blocks of earlier DKG
  cycles. `pruneblockchain` clamps its height accordingly, and on testnet and
  devnets pruning starts once the chain is longer than 3000 blocks
  (previously 1000). Blocks that an earlier version already pruned are not
  recovered; a node that fails to validate a block after upgrading needs
  `-reindex`.
