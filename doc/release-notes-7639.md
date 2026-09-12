Notable changes
---------------

### Version 2 Asset Unlock transactions (v24)

Once the `v24` hard fork activates, Platform withdrawals are issued as version 2
Asset Unlock transactions. Their transaction hash is computed with the quorum
signing fields (`requestedHeight`, `quorumHash`, `quorumSig`) zeroed, so every
instance Platform re-signs after an expiry is the same transaction with one
stable txid. Spends of an unmined withdrawal's outputs therefore stay valid
across re-signs.

A version 2 Asset Unlock is InstantSend-locked as soon as it can be mined in
the next block: it carries a valid signature from a recent quorum, is inside
its height window, no other instance of its withdrawal index is in the
mempool, and the withdrawals pending in the mempool fit the credit pool's
current limit. The lock pins the withdrawal index (as the outpoint
`{DIP-27 request id, 0}`) to the txid. Spends of a locked withdrawal are
ordinary InstantSend transactions and the wallet treats the withdrawal's
outputs as trusted, so Platform-to-Core transfers become rapidly respendable.

Version 2 unlocks are relayed by instance hash (new inventory type
`MSG_ASSET_UNLOCK`, protocol version 70242), kept in the mempool while expired
awaiting a re-signed replacement, and committed to by the coinbase transaction
(CbTx version 4, `merkleRootAssetUnlocks`).

Updated RPCs
------------

- `getmempoolinfo` reports `pendingassetunlocks`, the sum of the withdrawal
  amounts of the Asset Unlock transactions in the mempool.
- `getassetunlockstatuses` reports `instantlock` for mempooled withdrawals.
- `getrawtransaction` and `decoderawtransaction` report `instanceHash` for
  version 2 Asset Unlock transactions.
