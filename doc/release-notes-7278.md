Removed Sporks
--------------

* `SPORK_3_INSTANTSEND_BLOCK_FILTERING` and `SPORK_9_SUPERBLOCKS_ENABLED` have
  been removed. These sporks were already effectively always enabled on mainnet
  but continued to gate behavior on test networks (testnet, regtest, and devnet).
  The associated functionality (InstantSend conflicting-block rejection and
  superblock payments) is now permanently enabled across all networks, and the
  sporks will no longer appear in the `spork` RPC output.

Updated RPCs
------------

* `getblocktemplate` now always reports `superblocks_enabled` as `true`. The
  field is retained for backwards compatibility.

* `getblocktemplate` now requires a fully synced node at superblock heights.
  This check is skipped on test networks. Previously it was gated by
  `SPORK_9_SUPERBLOCKS_ENABLED` which was already always active on mainnet.
