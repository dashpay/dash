Wallet
------

- New wallet RPC `bls operatorkey` issues masternode operator keys derived from
  the wallet mnemonic at `m/9'/coin'/3'/3'/index`, the keys the DashPay mobile
  wallets derive from the same recovery phrase, so that phrase is the only
  backup an operator needs. Each call issues the next key; `bls operatorkey
  <index>` returns a key issued earlier. A wallet restored from the phrase
  skips indexes whose key is in the masternode list at the chain tip. Requires
  an unlocked descriptor wallet with a mnemonic. (#7594)
