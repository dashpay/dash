P2P and network
---------------

- Bound in-memory masternode list caches so unauthenticated historical
  `GETMNLISTDIFF` requests can no longer grow memory without limit between
  blocks. Recent lists are capped by height-aware eviction; stale historical
  mini-snapshots are kept in a small LRU cache so repeated requests do not
  re-read large on-disk snapshots on every call. (#7485)
