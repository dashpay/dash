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
  registrar configuration.
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
    "standby dissolutions" safe.
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

- `protx register_shared_prepare` builds an unsigned shared registration from a
  caller-supplied funding transaction.
- `protx shared_sign` signs a shared registration, dissolution or shared
  registrar update with every share owner key the wallet holds.
- `protx shared_combine` combines collected signatures and optionally submits.
- `protx dissolve` creates, signs and submits a unilateral ProDisTx (or, with
  `submit=false`, returns hex suitable for offline standby storage).
- `protx dissolve_prepare` builds an unsigned unanimous ProDisTx.
- `protx update_share` updates one share's reward address.
- `protx update_shared_registrar_prepare` builds an unsigned ProUpSharedRegTx.
