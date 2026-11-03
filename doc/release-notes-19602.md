Wallet
======

Migrating Legacy Wallets to Descriptor Wallets
---------------------------------------------

An experimental RPC `migratewallet` has been added to migrate Legacy (non-descriptor) wallets to
Descriptor wallets. It is disabled by default: start the node with `-walletmigration` to use it, which
also shows the `File` -> `Migrate Wallet` menu item in the GUI. More information about the migration process is available in the
[documentation](https://github.com/dashpay/dash/blob/master/doc/managing-wallets.md#migrating-legacy-wallets-to-descriptor-wallets).
