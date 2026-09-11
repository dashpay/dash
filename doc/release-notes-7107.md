New RPCs
--------

- Add `getquorumproofchain` and `verifyquorumproofchain` for compact mining-transaction
  proofs from an independently trusted snapshot, plus `getchainlockbyheight` for
  historical coinbase-carried certificates. The RPCs read required blocks on
  demand, without an additional index or startup scan. Nodes must retain the
  required historical blocks to generate proofs. (#7107)
