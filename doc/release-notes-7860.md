Governance
----------

- Pruned nodes can now run governance. Proposal collateral is now looked up
  in a new governance collateral index instead of `-txindex`. The index
  stores only the transactions that could be a proposal's collateral, works
  with `-prune`, and always runs; there is no option for it. Pruned nodes, and
  nodes started with `-txindex=0`, now sync proposals and votes, check
  superblock payments in full, and can create and submit proposals.

- Every node builds the index once, on the first start after upgrading. It
  reads every block for that, and the node's governance sync waits until it
  has finished.

- A node that pruned blocks before it had this index can't build it, because
  the index has to start from the genesis block. It now starts with governance
  turned off and a warning instead of failing; restarting it once with
  `-reindex` downloads the blockchain again and enables governance, and the
  node stays pruned. This replaces the warning such nodes showed before, that
  governance is disabled because the node is pruned.

- Dash-Qt used to save `disablegovernance=1` in `settings.json` when pruning
  was turned on in the options. That setting stays until it is removed, so
  governance stays off on those nodes.

- `gobject prepare` and `gobject submit` now fail straight away on a node that
  can't check or relay proposals. Before, `gobject prepare` burned the
  proposal fee and only `gobject submit` failed afterwards, with the
  misleading error `Invalid proposal collateral`. Collateral errors now also
  say what is wrong with the collateral instead of only
  `Invalid proposal collateral`.

- Pruned nodes now check superblock payments against the governance
  triggers they know, like full nodes do, instead of only checking that a
  superblock stays within the budget.

- `getindexinfo` reports the new index as `govcollateralindex`.

- Starting with `-prune` and `-disablegovernance=0` is no longer an error.
  Governance validation no longer needs `-txindex`.
