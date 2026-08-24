Wallet changes
--------------

- Unlocking an output with `lockunspent` (or through the Dash-Qt coin control
  dialog) now also opts that output out of the automatic masternode-collateral
  and dust-protection locks. Previously those automatic locks were reapplied on
  every wallet load, which silently undid the unlock and left the output
  unspendable with no indication why. Locking the output again hands it back to
  the automatic protection, as long as that lock is persistent: `lockunspent`
  writes a lock to the wallet file only when asked to, and a memory-only lock
  leaves the decision standing. Registering the output as a masternode
  collateral also ends the opt-out and locks the output again, because the
  decision was made about an ordinary coin and does not carry over to live
  collateral. The opt-out is stored in the wallet file; an older Dash Core
  release reading the same wallet ignores the record and reapplies the automatic
  locks as it did before. (#7635)
