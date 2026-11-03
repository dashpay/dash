Consensus changes
-----------------

- On mainnet, the first entry of each address list in a version 3 (extended addresses) ProRegTx or
  ProUpServTx must use that list's standard port: 9999 for `coreP2PAddrs`, 26656 for `platformP2PAddrs` and
  443 for `platformHTTPSAddrs`, as legacy ProTx versions already require on mainnet. Additional entries
  may use any otherwise valid port. Violations are rejected with `bad-protx-netinfo-port`.
