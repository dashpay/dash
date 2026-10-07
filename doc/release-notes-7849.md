Consensus changes (not active)
------------------------------

- A new `distinct_required_payments` deployment requires the required
  masternode payments and the payments of a triggered superblock to be
  matched by distinct coinbase outputs, so one output can no longer
  satisfy a masternode payment and an identical superblock payment at
  once. The deployment is not active on any network, and its activation
  is left for a future release. (#7849)
