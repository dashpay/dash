Files
-----

* On startup, the list of banned hosts and networks (via `setban` RPC) in
  `banlist.dat` is ignored and only `banlist.json` is considered. Dash Core
  version 22.x is the only version that can read `banlist.dat` and also write
  it to `banlist.json`. If `banlist.json` already exists, version 22.x will not
  try to translate the `banlist.dat` into json. After an upgrade, `listbanned`
  can be used to double check the parsed entries. (#5574)
