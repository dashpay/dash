Wallet
------

- Since Dash Core 19.2.0, mnemonic passphrase bytes after byte 248 have been silently ignored when deriving the wallet seed. Creating a new mnemonic seed with a passphrase longer than 248 bytes (UTF-8 bytes, not characters) is now rejected by `upgradetohd`, `createwallet` and first-run wallet creation from `-mnemonic`/`-mnemonicpassphrase`. Existing wallets, backups and migration derive exactly the same keys as before.

- To recover an existing wallet whose seed was created with a longer passphrase, use the new `allowlegacymnemonicpassphrase` argument of `upgradetohd`, or the startup option `-allowlegacymnemonicpassphrase` together with `-mnemonic` and `-mnemonicpassphrase`. Either way, you must supply the original valid recovery words. Random mnemonic generation cannot use this option, and it does not add any protection from the ignored bytes. A warning is returned when it takes effect. Legacy wallets keep their historical 256-byte passphrase ceiling.

Updated RPCs
------------

- `upgradetohd` has a new fifth argument, `allowlegacymnemonicpassphrase` (default: `false`). See above.

Updated settings
----------------

- New option `-allowlegacymnemonicpassphrase` (default: `0`). It only takes effect during wallet creation/first start. See above.
