Wallet
------

- Creating an HD wallet with a mnemonic passphrase longer than 248 bytes (not characters) through `upgradetohd`,
  `createwallet` or `-mnemonicpassphrase` is now rejected, and one longer than 256 bytes is always rejected.
  Existing wallets are not affected. (#7842)

  Seed derivation uses the BIP39 salt `"mnemonic"` followed by the passphrase. Dash Core 19.2.0 started cutting that
  salt at 256 bytes to stop a crash, so every passphrase byte after the 248th has been ignored since then. Two
  passphrases that differ only after that point derive the same seed. Before 23.0.0 the salt was also cut at the
  first NUL byte.

  The derivation is left as it is. Using the full passphrase would bring back the pre-19.2.0 derivation, and a wallet
  created with a long passphrase on 19.2.0 or later could then no longer be recovered from its mnemonic. Instead, new
  wallets can't be given a passphrase that the derivation would truncate.

  To recover a wallet that was created on 19.2.0 or later with a passphrase of 249 to 256 bytes, start the node with
  the new `-allow-deprecated-long-mnemonics` option, which accepts passphrases of up to 256 bytes and derives the same
  (truncated) seed as before. A wallet created before 19.2.0 with a passphrase longer than 248 bytes can't be
  recovered from its mnemonic on current versions, with or without the option. Restore it from a wallet backup
  instead.
