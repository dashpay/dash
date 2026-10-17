Mining and mempool policy
-------------------------

- When a block template includes an operator key change or revocation for a masternode, it now
  places the masternode's pending `ProUpServTx` signed with the key being replaced before it. Such
  a service update could otherwise confirm after the change in the same block and restore the
  previous operator's service and payout details. A service update that cannot be placed before
  the change, or that does not fit in the block with it, is left out of that block. (#7857)
- The mempool now rejects with `protx-dup` a `ProUpServTx` that spends, directly or through other
  pending transactions, an output of a pending operator key change or revocation, which it can only
  confirm after, or of a pending asset lock, asset unlock or MNHF signal, which could keep it out of
  a block. A service update funded from such outputs can be sent once they confirm, signed with the
  operator key that is current then. (#7857)
