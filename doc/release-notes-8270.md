Updated RPCs
------------

- RPC handlers that let a local Evo-state exception reach the RPC dispatcher
  now return `RPC_INTERNAL_ERROR` (-32603) instead of `RPC_MISC_ERROR` (-1).
  RPCs that handle the failure as a validation result, such as `submitblock`
  and the package RPCs, report it through their own error paths. (#8270)

Other changes
-------------

- Developers: on regtest, every Evo database access must now be bound to a
  chainstate's view (the hidden `-checkevobinding` option, on by default
  there). A code path that reads Evo state without a binding fails with an
  internal error, a dropped task or a stalled chainstate instead of reading
  the default view. Local regtest networks run with it unless started with
  `-checkevobinding=0`. (#8270)
