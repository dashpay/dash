Notable changes
===============

Consensus / Masternodes
-----------------------

- DIP-0026 (Multi-Party Payouts) is implemented behind a new version-bits deployment, `v25`
  (EHF, version bit 13), which additionally requires `v24` (extended addresses) to be active.
  Once `v25` activates, a masternode can split its owner-side block reward on-chain among up to
  32 payees instead of a single owner payout, using a new version 4 ProRegTx/ProUpRegTx. All
  pre-activation behavior is unchanged: version 4 transactions are rejected until `v25` is active,
  and the serialized forms for existing (pre-v4) masternodes are byte-for-byte identical. The
  coinbase splits the owner reward across the payees by basis points (floor with a deterministic
  one-duff remainder), summing to the owner reward exactly. (#7353)

RPC
---

- `protx register`, `protx register_fund`, `protx register_prepare` and `protx update_registrar`
  now accept the `payoutAddress` argument either as a single address (unchanged) or, once DIP-0026
  (`v25`) is active, as a JSON object mapping each payout address to its share in basis points
  (1-10000), e.g. `{"XaddrA": 6000, "XaddrB": 4000}`. The shares must be unique and sum to exactly
  10000. Over dash-cli the object is passed as a quoted JSON string. (#7353)

- `protx info`, `protx list` and `masternodelist` now report a `payoutShares` array for version 4
  masternodes; the single `payoutAddress` field is omitted for those entries. Output for pre-v4
  masternodes is unchanged. (#7353)
