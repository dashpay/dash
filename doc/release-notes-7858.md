InstantSend changes
-------------------

- Masternodes no longer InstantSend-lock provider transactions (ProRegTx, ProUpServTx, ProUpRegTx, ProUpRevTx and the
  other provider special transactions). They are mined once they have been in the mempool for 10 minutes, and
  transactions that spend their outputs can be locked once the block that includes the provider transaction is
  ChainLocked. Software that waits for an islock on a provider transaction should wait for a ChainLock or confirmations
  instead. (#7858)
