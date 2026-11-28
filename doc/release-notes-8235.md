Updated RPCs
------------

- `testmempoolaccept` with more than one transaction reports the
  `package-error` `evo-local-unavailable`, and `submitpackage` fails with that
  reason, when this node cannot read the Evo state it needs to check the
  package, for example a stored block it cannot read back while deriving the
  credit pool for an asset unlock. The request was not at fault. (#8235)

Other changes
-------------

- A block this node has stored but cannot read back while deriving the credit
  pool no longer makes the block being validated invalid. Block validation then
  shuts the node down with a "Failed to read block" error instead, as it does
  when it cannot read a block it is connecting. (#8235)
