Wallet
------

- Mnemonic-backed descriptor wallets can now derive DashSync-compatible
  masternode operator BLS keys from the wallet seed, so the recovery phrase
  also backs up operator keys. Restored wallets avoid keys that are currently
  registered, but may reuse a key that was retired in the past. Other wallet
  types remain unchanged and can continue using `bls generate`. (#7594)
