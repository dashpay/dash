Consensus (v24, not yet activated on mainnet or testnet)
---------------------------------------------------------

- EvoNodes cannot register with shared collateral or have more than one owner payout until
  the new `evo_shares` deployment activates. An EvoNode ProRegTx with a share table is
  rejected with `bad-protx-shares-evo`, and an EvoNode ProRegTx or ProUpRegTx with more than
  one owner payout is rejected with `bad-protx-payouts-evo`. Regular masternodes are not
  affected.
- `evo_shares` (version bit 14) is a masternode-activated deployment with the same parameters
  as `v24`. This release follows it but its masternodes do not sign it, so it stays inactive
  until a later release signs it once Platform supports EvoNode shares and payouts.
- The consent digest that share owners sign for a shared EvoNode registration also covers the
  EvoNode's `platformNodeID`. The digest of a regular shared registration is unchanged.

New RPCs
--------

- `protx shared_register_prepare_evo` creates an unsigned shared EvoNode registration. It
  takes the arguments of `protx shared_register_prepare` followed by `platformNodeID`,
  `platformP2PAddrs` and `platformHTTPSAddrs`, and fails until `evo_shares` is active.
