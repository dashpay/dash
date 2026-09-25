Proof of Service
----------------

When quorum PoSe (`SPORK_23_QUORUM_POSE`) is active for a quorum type, DKG members now also vote a
fellow member bad if every masternode connection they have to it advertises that it does not
provide service:

* it does not relay transactions (`relay=0` in its `version` message, as a masternode running
  `-blocksonly` does), so it never signs InstantSend locks or relays recovered signatures;
* it does not serve compact block filters (no `NODE_COMPACT_FILTERS`, as with
  `-peerblockfilters=0`).

These votes are handled like the existing ones for masternodes with an outdated protocol version:
enough of them exclude the member from the quorum and PoSe-punish it. Masternodes running this
release already refuse to start with these settings, and masternodes older than the minimum
masternode protocol version are already voted bad, so in practice this covers modified software.
Only masternode connections are considered, because block-relay-only connections always advertise
`relay=0`. With `SPORK_23_QUORUM_POSE` set to 1, the ChainLocks and Platform quorum types
(`llmq_400_60`, `llmq_400_85`, `llmq_100_67`) are exempt, as for the other connection checks.
