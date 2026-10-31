P2P and network changes
-----------------------

* Startup now fails if any configured `-bind`, `-whitebind` or the implicit Tor
  onion-service bind (`127.0.0.1:9996` by default) cannot be set up. Dash Core
  v23 and earlier started as long as at least one bind succeeded.

* Nodes configured with `-bind` but no specific `-bind=<addr:port>=onion` no
  longer create an automatic Tor onion service. The service would have to
  forward to a shared bind, which cannot tell Tor-forwarded connections from
  direct ones, so onion peers would be treated as clearnet peers (and could be
  told the node's clearnet address or get IP-based whitelist permissions).
  These nodes still start, and still use Tor for outbound onion connections if
  its control port is reachable; the debug log notes that no onion service was
  created. Nodes without `-bind`, including those using only `-whitebind`,
  continue to get the default onion target. Add a specific
  `-bind=<addr:port>=onion` to accept incoming Tor connections. A wildcard
  `=onion` bind is never used as the onion-service target. Only an explicit
  `-listenonion=1` without a usable onion bind makes startup fail.
