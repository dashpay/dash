Updated RPCs
------------

- Information on soft fork status has been moved from `getblockchaininfo`
  to the new `getdeploymentinfo` RPC which allows querying soft fork status at any
  block, rather than just at the chain tip. Inclusion of soft fork
  status in `getblockchaininfo` can currently be restored using the
  configuration `-deprecatedrpc=softforks`, but this will be removed in
  a future release. Note that in either case, the `status` field
  now reflects the status of the current block rather than the next
  block.

New REST endpoint
-----------------

- A new `/rest/deploymentinfo` endpoint has been added for fetching various
  state info regarding deployments of consensus changes.
