Consensus changes
-----------------

- A rotating LLMQ (DIP0024) cycle that can't fill a quarter, because too few eligible masternodes are left, now
  keeps the members it actually selected: the quarter stays short or empty when it is rebuilt from the cycle's
  quorum snapshot. Previously, rebuilding a stored short quarter padded it by wrapping around the candidate list,
  adding members that were never selected and possibly repeating members of the quorum's other quarters. With too
  few enabled masternodes, or when the selection walk made no progress, the builder instead returned empty
  quarters without storing a snapshot, and later cycles stopped loading their older quarters at the missing
  snapshot. Given the same earlier quarters, cycles whose quarters all fill are selected, stored and rebuilt
  exactly as before. Quorum sizes, minimum sizes and thresholds are unchanged: the existing `minSize`, DKG and
  commitment rules still decide whether a quorum with short quarters forms.
- There is no activation: the new rules apply to every cycle. A variant of this build replayed mainnet through
  h=2554746 and testnet through h=1579827 without rejecting a block; that doesn't establish identical quorum
  members for every historical cycle. The testnet cycles whose selection differs (base blocks at h=838368,
  h=901152-903168 and h=1366560-1367136) keep the old rules. Short or empty quarters only occur when very few
  masternodes are eligible. Older and upgraded nodes can then derive different quorum members, either in that
  cycle or when its quarters are reused in later cycles, and reject each other's commitments, so nodes have to
  upgrade before such a cycle occurs.
- Devnets and regtest chains that already had a cycle with a short or empty quarter are not supported across
  this upgrade and have to be started over. Their stored snapshots, or missing snapshots, can give upgraded nodes
  different members than nodes that reindex or sync from scratch, and those can reject historical commitments
  whose members change, so reindexing alone is not a guaranteed recovery.

Notes for SPV and other clients that rebuild rotating quorums from `qrinfo`
-------------------------------------------------------------------------------

The serialized `CQuorumSnapshot` and `qrinfo` formats don't change, but `mnSkipListMode` gains a value,
`MODE_SHORT_QUARTERS` (4). Snapshots in `MODE_NO_SKIPPING` (0) and `MODE_SKIPPING_ENTRIES` (1) keep their
meaning and are decoded as before. Nodes store `MODE_SHORT_QUARTERS` only for cycles with a short or empty
quarter, so clients must decode it before such a cycle occurs:

- Candidate list: as before, `activeQuorumMembers` refers to the work block's valid masternodes with a
  confirmed registration (non-null `confirmedHash`), sorted by score. The candidates are the unmarked ones
  followed by the marked ones, each part in that order.
- Walk: a step counter starts at 0 and counts every candidate visited in the cycle, across all quorum indices
  in order. The candidate at step `t` is `list[t % list.size()]`. For each quorum index, a visited candidate is
  skipped if `t` is a skip step and added otherwise. An index's quarter ends when it has `quarterSize` members
  or after `list.size()` consecutive skips; the next index continues at the next step. An empty candidate list
  gives empty quarters and an empty skip list.
- Skip steps: `mnSkipList` holds the steps at which a candidate was skipped. The first entry is the absolute
  step of the first skip; each later entry is the positive difference from the previous skip step. A snapshot
  is malformed if an entry is negative (or a later entry is zero), if a skip step is never reached by the walk,
  or if a quarter would contain the same position twice.

For cycles they compute successfully, outside predictions, nodes now store a snapshot even when no masternodes
are eligible. The exempt testnet cycles keep the old storage behaviour.
