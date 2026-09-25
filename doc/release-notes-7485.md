P2P and network changes
-----------------------

- Masternode list caches no longer grow between blocks when peers request
  masternode list diffs or quorum rotation info (`getmnlistd`, `getqrinfo`) for
  many historical blocks. Lists and diffs older than the recent cache window are
  now dropped after each such request instead of after the next block. (#7485)
