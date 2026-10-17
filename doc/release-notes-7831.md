P2P and network changes
-----------------------

- Governance object signatures larger than 96 bytes are now rejected, both from
  the network and from the on-disk governance cache (`governance.dat`). The
  cache format version was bumped for this: on the first start after
  upgrading, the existing cache is discarded and governance objects and votes
  are synced again from the network. (#7831)
