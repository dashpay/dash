Updated RPCs
------------

- `protx listdiff` reports the complete `addresses` object, `platform_p2p` and `platform_https` included,
  when the core P2P address of an EvoNode on legacy (pre-extended) addresses changes, so its Platform
  ports can be followed without `-deprecatedrpc=service`. (#7779)
