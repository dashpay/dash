Other changes
-------------

- While a loaded AssumeUTXO snapshot has not been validated in the
  background, an InstantSend lock that conflicts with a block on the snapshot
  chain no longer invalidates that block. The block is kept and the lock is
  discarded: it is not applied, announced, relayed or served. Once background
  validation completes, such a lock invalidates the conflicting blocks as
  before. (#8078)
