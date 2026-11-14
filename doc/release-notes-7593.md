Updated RPCs
------------

- On a pruned node, `quorum info`, `quorum memberof` and `quorum selectquorum`
  fail with `RPC_INTERNAL_ERROR` for a quorum whose members this node cannot
  compute because the block that fixes them was pruned. They used to work with
  members computed from a substitute value, which could be wrong. (#7593)

Other changes
-------------

- A pruned node no longer substitutes the block hash for the quorum score
  modifier of a pruned work block. It keeps such a quorum's public key, from
  its mined commitment, without members, so it still verifies signatures
  by that quorum (for example on masternode hard-fork signals) and never
  stalls on them. (#7593)
