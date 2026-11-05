Wallet
------

- Automatic wallet backups now keep older restore points instead of only
  the most recent ones. The newest `-createwalletbackups` backups are
  always kept (default: 10; the maximum is raised from 10 to 20). Beyond
  those, one backup is kept from each exponentially widening age range
  (1-2 hours old, 2-4 hours, 4-8 hours, and so on), up to a total of
  `-maxwalletbackups` files. Previously a wallet that replenished its
  keypool often, for example while mixing, could lose every backup older
  than a few hours. (#7005)

- A backup's age is now read from the timestamp in its filename
  (`wallet.dat.YYYY-MM-DD-HH-MM`) rather than the file modification time.
  Files in the backups directory whose names do not follow this format,
  such as a renamed backup, are no longer deleted and do not count
  towards `-maxwalletbackups`. Renaming a backup is a simple way to keep
  it indefinitely. (#7005)

- By default up to 30 automatic backups are now kept per wallet instead
  of 10, which takes more disk space for large wallets. Set
  `-maxwalletbackups=10` to keep the previous footprint. See section 1.6
  of `doc/managing-wallets.md` for details. (#7005)

Updated settings
----------------

- New option `-maxwalletbackups=<n>` sets the maximum total number of
  automatic wallet backups to keep (default: 30). `0` disables automatic
  backups, as does `-createwalletbackups=0`; existing backups are left
  untouched while automatic backups are disabled. `-createwalletbackups`
  is capped at `-maxwalletbackups`. (#7005)
