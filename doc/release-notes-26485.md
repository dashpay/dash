JSON-RPC
---

For RPC methods which accept `options` parameters ((`importmulti`, `listunspent`, `fundrawtransaction`, `send`, `sendall`, `walletcreatefundedpsbt`, `simulaterawtransaction`), it is now possible to pass the options as named parameters without the need for a nested object. (#8010)

This means it is possible make calls like:

```sh
src/dash-cli -named listunspent coinType=1
```

instead of

```sh
src/dash-cli -named listunspent query_options='{"coinType": 1}'
```
