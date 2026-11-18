Wallet
------

- Automatic wallet backups now keep older restore points instead of only
  the most recent ones. The newest `-createwalletbackups` backups are
  still always kept (default: 10; the maximum is raised from 10 to 20).
  Beyond those, at most one backup is kept from each exponentially
  widening age range (1-2 hours old, 2-4 hours, 4-8 hours, and so on), up
  to a total of `-maxwalletbackups` files (default: 30). By default this keeps up
  to 30 files per wallet instead of 10; set `-maxwalletbackups=10` to
  keep the previous footprint. A backup's age is read from the timestamp
  in its filename, so a renamed backup is never pruned. See section 1.6
  of `doc/managing-wallets.md` for details. (#7005)

Updated settings
----------------

- New option `-maxwalletbackups=<n>` sets the maximum total number of
  automatic wallet backups to keep (default: 30). `0` disables automatic
  backups, as does `-createwalletbackups=0`; existing backups are left
  untouched while automatic backups are disabled. (#7005)
