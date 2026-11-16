P2P and network changes
-----------------------

Support for UPnP was dropped. If you want to open a port automatically, consider using the `-natpmp`
option instead, which now uses a built-in PCP or NAT-PMP implementation depending on router support.

Updated settings
------

- `-upnp` is ignored with a warning. If `-natpmp` is not set, the `-upnp` value is used for `-natpmp`.
  The GUI's former "Map port using UPnP" option is migrated to `natpmp` on startup, whether it is
  stored as an `upnp` entry in `settings.json` or, from releases before v23.1.0, in the GUI's own
  settings.

Build system
------------

- The miniupnpc and libnatpmp dependencies were removed, together with the `WITH_MINIUPNPC` and
  `WITH_NATPMP` CMake options and the `NO_UPNP` and `NO_NATPMP` depends options.
