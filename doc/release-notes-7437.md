# Decentralized Masternode Shares

This release implements the Decentralized Masternode Shares DIP, activating
together with DIP-0026 multi-party payouts as part of the v24 hard fork
(`DEPLOYMENT_V24`). Before activation there is no behavior change.

## Consensus changes (active with v24)

- A version 3 (extended addresses) ProRegTx may carry a collateral share table: 2 to 8 participants
  fund the masternode collateral atomically in one registration, each recording
  an immutable amount, refund script and share owner key, plus an updatable
  reward script. Every participant consents by signing a digest that binds the
  exact funding inputs, all outputs, the share table, the penalty terms and the
  registrar configuration. The early-period penalty must be below the smallest
  share and must be zero when no early period is configured.
- The shared collateral is paid to the 7-byte template script
  `04445348437551` (`0x04 "DSHC" OP_DROP OP_TRUE`). From activation, an output
  paying this exact script is valid only as the collateral of a valid shared
  registration, and spending such an output is valid only via a ProDisTx.
  Template outputs mined before activation become permanently unspendable.
- Three new special transaction types:
  - **ProDisTx (type 10)** dissolves a shared masternode, refunding every
    participant's principal to its immutable refund script. Exactly one
    signature (unilateral, penalized during the configured early period) or one
    per share (unanimous, penalty-free). Validity is monotone: a ProDisTx that
    is valid at some height is valid at every later height, which makes offline
    "standby dissolutions" safe. The transaction fee is capped at 1000000 duffs
    and a unilateral dissolution may not pay bonuses beyond the configured
    early penalty, bounding what a stolen share owner key can drain from its
    own share.
  - **ProUpShareTx (type 11)** lets one share owner update their reward script.
  - **ProUpSharedRegTx (type 12)** updates the operator key and/or voting key
    with a signature from every share owner. A plain ProUpRegTx is invalid for
    shared masternodes.
- The owner reward of a shared masternode is split across the share table
  proportionally to the recorded contributions (sequential floor, remainder to
  the last entry), paying each share's reward script (or its refund script when
  none is set). Operator rewards are unchanged.
- Withdrawal (asset unlock) transactions may not pay the template script.

## Relay policy changes

- The template output relays only as the declared collateral output of a shared
  registration, and a template prevout is accepted only inside a ProDisTx; both
  remain nonstandard everywhere else.

## New RPCs

- `protx shared_register_prepare` builds an unsigned shared registration from a
  caller-supplied funding transaction. The result echoes the decoded terms and
  carries a `warning` when `earlyPenalty` is zero, since any participant can
  then force an early exit at no cost beyond the transaction fee.
- `protx shared_sign` signs a shared registration, dissolution or shared
  registrar update with every share owner key the wallet holds. It returns the
  decoded terms being consented to (for a dissolution, including the outputs)
  alongside the signatures, and repeats the zero-penalty warning for a
  registration. It refuses a registration or dissolution carrying an
  unsatisfied lock time or a relative input lock unless `allowTimeLocks` is
  set.
- `protx shared_combine` combines collected signatures and optionally submits.
  A dissolution combined here requires a signature from every share; unilateral
  dissolutions come fully signed from `protx shared_dissolve`.
- `protx shared_dissolve` creates, signs and submits a unilateral ProDisTx (or, with
  `submit=false`, returns hex suitable for offline standby storage).
- `protx shared_dissolve_prepare` builds an unsigned unanimous ProDisTx.
- `protx shared_update_share` updates one share's reward address. To restore rewards to the immutable refund address, pass that address explicitly; empty reward scripts are not valid in update transactions.
- `protx shared_update_registrar_prepare` builds an unsigned ProUpSharedRegTx.

Updated RPCs
------------

- `masternodelist` and `masternode list` report comma-separated share owner
  addresses in `owneraddress` for shared masternodes. The `json` and `recent`
  modes can be filtered by any share owner address. Shared registrations and
  masternode state omit the singular `ownerAddress` field in `protx` and decoded
  transaction output; each participant's owner address is in `shares`. (#7437)

GUI changes
-----------

- The owned-masternode filter includes shared masternodes when the wallet holds
  a participant's refund destination, including when rewards go to a different
  wallet. (#7437)
- The Masternodes tab gains a "Shared Masternode…" wizard for registering a
  masternode funded by several people. The coordinator enters the participants,
  masternode settings and exit terms, reserves coins for their own share and
  copies one invitation; every participant pastes it, reserves their coins and
  copies back their details. Two more rounds complete the registration: locked
  terms out and approvals back, then a signing request out and signed
  contributions back. Any message can be pasted from the landing page, replies
  are accepted in any order, and every message shows a short code so
  participants can confirm they hold the same one. Sessions can be saved and
  reopened by either role. Reserved coins are released after broadcast.
- Shared masternodes appear in the list with the type "Shared (you hold k of
  n)", a details view listing every share, and context-menu actions to change
  the wallet's reward address, rotate the operator or voting key (with every
  share owner's approval), dissolve now, dissolve together, or create a
  standby dissolution file holding both the penalty-free and the immediate
  variant. Pasting a maintenance request or a standby dissolution into the
  list opens the matching dialog.
