Notable changes
===============

Updated RPCs
------------


Changes to wallet related RPCs can be found in the Wallet section below.

Wallet
------

- Descriptor wallets are now the default wallet type. Newly created wallets
  will use descriptors unless `descriptors=false` is set during `createwallet`, or
  the `Descriptor wallet` checkbox is unchecked in the GUI. (dash#7204)

  Note that wallet RPC commands like `importmulti` and `dumpprivkey` cannot be
  used with descriptor wallets, so if your client code relies on these commands
  without specifying `descriptors=false` during wallet creation, you will need
  to update your code.

