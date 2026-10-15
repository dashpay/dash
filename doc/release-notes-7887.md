Updated RPCs
------------

- An RPC that fails with an error because this node cannot read the
  masternode, quorum or other Evo state it needs, for example because the
  block data it is derived from was pruned, now returns error code
  `RPC_INTERNAL_ERROR` (-32603) instead of `RPC_MISC_ERROR` (-1). The request
  was not at fault. The package RPCs report such a failure as described
  below. (#7887)

- `testmempoolaccept` with more than one transaction reports such a failure as
  the `package-error` `evo-local-unavailable`, and `submitpackage` fails with
  that reason. (#7887)

Other changes
-------------

- A block this node has stored but cannot read back while deriving the credit
  pool or the EHF signals no longer makes the block being validated invalid.
  The node shuts down with a "Failed to read block" error instead, as it does
  when it cannot read a block it is connecting. (#7887)

- On a node without an assumeutxo snapshot, a warning about Evo state that
  cannot be derived because block data was pruned now says so and suggests
  restarting with `-reindex`. Startup still fails when the EHF signals at the
  tip cannot be rebuilt. (#7887)

- On a pruned node, a block whose masternode, quorum or other Evo state
  depends on block data that was pruned is no longer marked invalid, which
  left the node on a local fork until `reconsiderblock`. The node now stops
  advancing at that block, logs an error and shows a warning that block data
  was pruned; restart with `-reindex` to download it again. (#7887)

- Developers: on regtest, every Evo database access must now be bound to a
  chainstate's view (the hidden `-checkevobinding` option, on by default
  there). A code path that reads Evo state without a binding fails with an
  internal error, a dropped task or a stalled chainstate instead of reading
  the default view. Local regtest networks run with it unless started with
  `-checkevobinding=0`. (#7887)
