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
  `-bind=<addr:port>=onion` to accept incoming Tor connections. Only an
  explicit `-listenonion=1` without an onion bind makes startup fail.

* A wildcard onion bind such as `-bind=0.0.0.0:<port>=onion` is a startup error
  unless `-listenonion=0` is set, also with the default `-listenonion`. The node
  can't tell which connections to a wildcard listener came through Tor, so Tor
  peers would be treated as clearnet peers. Dash Core v23 accepted it. Use a
  specific address such as `-bind=127.0.0.1:<port>=onion` instead.
