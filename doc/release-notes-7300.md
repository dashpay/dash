P2P and network changes
-----------------------

* Startup now fails if any configured `-bind`, `-whitebind` or the implicit Tor
  onion-service bind (`127.0.0.1:9996` by default) cannot be set up. Dash Core
  v23 and earlier started as long as at least one bind succeeded.

* Nodes configured with `-bind` but no specific `-bind=<addr:port>=onion` now
  refuse to start when `-listenonion` is enabled. This includes nodes without
  Tor configured, since `-listenonion` is enabled by default when listening.
  Shared binds cannot distinguish Tor-forwarded connections from direct
  connections, which can grant Tor peers unintended IP-based whitelist
  permissions. Nodes without `-bind`, including those using only `-whitebind`,
  continue to get the default onion target. Users should add a specific
  `-bind=<addr:port>=onion` to accept incoming Tor connections, or set
  `-listenonion=0` to disable automatic onion service creation.
