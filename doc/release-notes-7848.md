Updated RPCs and policy
-----------------------

- `OP_CHECKDATASIG` and `OP_CHECKDATASIGVERIFY` now count as one signature
  operation each for relay policy. A P2SH redeem script whose combined
  `CHECKSIG`, `CHECKMULTISIG` and data-signature operations exceed 15 is
  non-standard. Data-signature operations also count toward the
  transaction signature-operation limit and the sigop-adjusted virtual
  size used for fees. This takes effect on upgrade. Counting them toward
  the block signature-operation limit is a consensus change behind the
  new `signature_work` deployment, which is not active on any network.
  (#7848)
