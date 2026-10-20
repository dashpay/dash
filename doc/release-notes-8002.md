Wallet
------

- New wallet RPC `bls operatorkey` issues masternode operator keys derived from
  the wallet mnemonic at `m/9'/coin'/3'/3'/index`, the keys the DashPay mobile
  wallets derive from the same recovery phrase, so that phrase is the only
  backup an operator needs. Each call issues the next key; `bls operatorkey
  <index>` returns a key issued earlier. A wallet restored from the phrase
  skips indexes whose key is in the masternode list at the chain tip, and a
  rescan moves past every key it sees assigned by a ProRegTx, ProUpRegTx or
  ProUpSharedRegTx, so keys rotated away earlier are not issued again. A
  wallet starts following its operator keys when a recovery phrase is restored
  into it or at its first `bls operatorkey` call; from then on its rescans
  inspect full blocks instead of block filters. A pruned node cannot rescan
  the blocks it has discarded, so issuing on a pruned node returns a warning.
  No key is issued while a rescan is running. Requires an unlocked descriptor
  wallet with a mnemonic. (#8002)
