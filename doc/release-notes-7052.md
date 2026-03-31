Wallet
------

- CoinJoin can now promote and demote between adjacent standard
  denominations within a mixing session after V24 activation.
  Promotion combines 10 inputs of one denomination into 1 output of the
  next larger denomination, while demotion splits 1 input into 10
  outputs of the next smaller denomination. Pre-V24 behavior remains
  unchanged. (#7052)
