P2P and network changes
-----------------------

* Startup now fails if any configured `-bind` (including `-bind=...=onion`) or
  `-whitebind` cannot be set up. Dash Core v23 and earlier started as long as
  at least one bind succeeded. The implicit Tor onion-service bind
  (`127.0.0.1:9996` by default) is still best effort, as in v23, unless
  `-listenonion=1` is set explicitly.

* The automatic Tor onion service (`-listenonion`) only forwards to a
  dedicated, non-wildcard onion listener that the node actually bound, so peers
  arriving through Tor are always recognized as onion peers. Without a
  `-bind=<addr:port>=onion`, Dash Core adds the default onion bind as v23 did,
  also when `-bind` is given; with `-bind` and `-listenonion=0` it no longer
  does. If the default onion bind fails, for example because another instance
  on the same host holds it, the node logs a warning and creates no onion
  service rather than pointing Tor at whatever holds that port. A wildcard
  `=onion` bind is never used as the onion-service target. With the default
  `-listenonion`, a node without a usable onion bind still starts and still
  uses Tor for outbound onion connections if its control port is reachable; the
  debug log notes that no onion service was created. Only an explicit
  `-listenonion=1` without a usable onion bind makes startup fail. To run onion
  services for several instances on one host, give each its own
  `-bind=127.0.0.1:<port>=onion`.
