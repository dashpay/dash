Removed Sporks
--------------

* `SPORK_3_INSTANTSEND_BLOCK_FILTERING` and `SPORK_9_SUPERBLOCKS_ENABLED` have
  been removed. The behaviours they gated (InstantSend conflicting-block
  rejection and superblock payments) are now always enabled. These sporks will
  no longer appear in the output of the `spork` RPC.

Updated RPCs
------------

* `getblocktemplate` now always reports `superblocks_enabled` as `true`. The
  field is retained for backwards compatibility.
