Mempool policy
--------------

- While an operator key change or revocation for a masternode is unconfirmed, the mempool no
  longer holds a `ProUpServTx` for that masternode signed with the key being replaced. One
  arriving afterwards is rejected with `protx-dup`, and one already pending is evicted when the
  key change is accepted, unless the key change descends from it. Such a service update could
  otherwise confirm in the same block after the change and restore the previous operator's
  service and payout details. Once the change confirms, the new operator signs service updates
  with the new key. An operator whose wallet sent an evicted service update should abandon it
  with `abandontransaction` to release its inputs. (#7582)
