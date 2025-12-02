Wallet
------

- CoinJoin denomination creation now respects the wallet's "avoid_reuse"
  setting. When the wallet has `avoid_reuse` enabled, change is sent to a
  fresh change address to avoid address/public key reuse. Otherwise, change
  goes back to the source address (legacy behavior). (#6870)


