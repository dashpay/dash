Consensus changes
-----------------

- A new EHF deployment, `quarter_steps` (version bit 15), changes how rotating LLMQs (DIP0024) record and
  rebuild the quarter selected in each DKG cycle. Before this deployment, when a cycle could not fill a
  quarter because too few masternodes were eligible, rebuilding that quarter from its quorum snapshot
  padded it by wrapping around the candidate list, which added members that were never selected and could
  repeat members of other quarters. After activation, a rebuilt quarter is exactly the quarter that was
  selected, including short and empty quarters. Quorum sizes, minimum sizes and thresholds are unchanged:
  the existing `minSize`, DKG and commitment rules still decide whether such a quorum forms.
  Quarters built before activation keep the old rule, so quorums become fully faithful three cycles after
  the first cycle that uses the new rule (the first cycle whose base block is at or after activation).
- The mainnet and testnet parameters of `quarter_steps` are placeholders (`NEVER_ACTIVE`); the start time,
  window and thresholds will be set in the release that deploys it.

Updated RPCs
------------

- `getdeploymentinfo` reports the new `quarter_steps` deployment.
- `quorum dkginfo`: while the `quarter_steps` state at a rotating quorum's cycle base can't be settled from
  its work block yet (the deployment is locked in, or is signalling with a window short enough to reach
  `active` before the cycle base), the upcoming DKG is reported with `known: false` and the reason
  "quarter_steps deployment state at the cycle base is not known yet".

Notes for SPV and other clients that rebuild rotating quorums from `qrinfo`
-------------------------------------------------------------------------------

The serialized `CQuorumSnapshot` and `qrinfo` formats do not change, but the meaning of `mnSkipList` does
for cycles where the deployment is active. Clients must implement the following rule before the deployment
activates.

- Activation: the rule applies to a cycle iff `quarter_steps` is active at the cycle's base block, that is
  the deployment state evaluated from the base block's parent (`DeploymentActiveAt(cycleBase)`, not
  `DeploymentActiveAfter(cycleBase)`). Every rebuild of that cycle's quarter, including as the H-C, H-2C or
  H-3C quarter of a later cycle, uses the state at that cycle's own base block.
- Activation height: `quarter_steps` is an EHF deployment, so its signalling starts only once the mnhf
  special transaction for bit 15 is mined, and its activation height can't be derived from block headers
  alone. Clients can take it from a node's `getdeploymentinfo` (`quarter_steps.bip9`: `ehf_height` is the
  height of the mined mnhf signal, and `since` is the activation height once `status` is `active`), or use
  the activation height published once the deployment has locked in. While the deployment is locked in,
  the reported `activation_height` is the end of the lock-in window and doesn't account for
  `min_activation_height`; activation happens at the later of the two, so don't use `activation_height`
  alone.
- Used masternodes: after activation, membership and exclusion are by proTxHash only. A masternode is used
  by a quorum index if its proTxHash is a member of one of that index's previous quarters (H-C, H-2C, H-3C),
  and used by the cycle if it is used by any index (masternodes removed from or PoSe-banned in the work
  block's list are ignored, as before). The masternodes' other properties, such as their addresses, no
  longer matter: before activation, previous members whose historical states conflicted with each other or
  with a candidate (for example one masternode's former address now used by another) could drop out of the
  used sets or block a candidate.
- Candidate list: the work block's valid masternodes not used by the cycle sorted by score, then the used
  ones sorted by score. `activeQuorumMembers` marks exactly the masternodes used by the cycle, so clients
  rebuild the same list from the snapshot as before.
- Walk: a step counter starts at 0 and counts every candidate visited in the cycle, across all quorum
  indices in order. The candidate at step `t` is `list[t % list.size()]`. For each quorum index, a visited
  candidate is skipped if its proTxHash is already a member of that index (from its previous quarters or
  this quarter), and is added otherwise. An index's quarter ends when it has `quarterSize` members or after
  `list.size()` consecutive skips; the next index continues at the next step. An empty candidate list gives
  empty quarters and an empty skip list.
- Encoding: `mnSkipList` holds the steps at which a skip happened. The first entry is the absolute step
  of the first skip; each later entry is the positive difference from the PREVIOUS skip step (the legacy
  encoding is relative to the first skip position instead). `mnSkipListMode` is `MODE_SKIPPING_ENTRIES`
  when the list is non-empty, otherwise `MODE_NO_SKIPPING`.
- Decoding: replay the same walk, treating step `t` as a skip exactly when it is the next decoded skip
  step and as an addition otherwise. A snapshot is malformed if an entry is negative (or a later entry is
  zero), if a skip step is never reached by the walk, or if a quarter would contain the same position
  twice.
- A snapshot is stored for every cycle after activation, including cycles with no eligible masternodes.
