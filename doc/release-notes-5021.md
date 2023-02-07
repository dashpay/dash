Added RPCs
--------

- In order to be backward compatible, `protx register`, `protx register_fund`, and `protx register_prepare` RPCs will decode BLS operator public keys using the legacy BLS scheme. `protx register_basicbls`, `protx register_fund_basicbls`, and `protx register_prepare_basicbls` were added to support decoding BLS operator public keys using the new basic scheme.

Other changes
--------

`qfcommit`
--------

Once the v19 hard fork is activated, `quorumPublicKey` will be serialised using the basic BLS scheme.
To support syncing of older blocks containing the transactions using the legacy BLS scheme, the `version` field indicates which scheme to use for serialisation of `quorumPublicKey`.

| Version | Version Description                                    | Includes `quorumIndex` field |
|---------|--------------------------------------------------------|------------------------------|
| 1       | Non-rotated qfcommit serialised using legacy BLS scheme | No                           |
| 2       | Rotated qfcommit serialised using legacy BLS scheme     | Yes                          |
| 3       | Non-rotated qfcommit serialised using basic BLS scheme  | No                           |
| 4       | Rotated qfcommit serialised using basic BLS scheme      | Yes                          |

`MNLISTDIFF` P2P message
--------

Starting with protocol version 70225, the following field is added to the `MNLISTDIFF` message between `cbTx` and `deletedQuorumsCount`.

| Field               | Type | Size | Description                       |
|---------------------| ---- | ---- |-----------------------------------|
| version             | uint16_t | 2 | Version of the `MNLISTDIFF` reply |

The `version` field indicates which BLS scheme is used to serialise the `pubKeyOperator` field for all SML entries of `mnList`.

| Version | Version Description                                       |
|---------|-----------------------------------------------------------|
| 1       | Serialisation of `pubKeyOperator` using legacy BLS scheme |
| 2       | Serialisation of `pubKeyOperator` using basic BLS scheme  |

`ProTx` txs family
--------

`proregtx` and `proupregtx` will support a new `version` value:

| Version | Version Description                                       |
|---------|-----------------------------------------------------------|
| 1       | Serialisation of `pubKeyOperator` using legacy BLS scheme |
| 2       | Serialisation of `pubKeyOperator` using basic BLS scheme  |

`proupservtx` and `prouprevtx` will support a new `version` value:

| Version | Version Description                            |
|---------|------------------------------------------------|
| 1       | Serialisation of `sig` using legacy BLS scheme |
| 2       | Serialisation of `sig` using basic BLS scheme  |

`MNHFTx`
--------

`MNHFTx` will support a new `version` value:

| Version | Version Description                            |
|---------|------------------------------------------------|
| 1       | Serialisation of `sig` using legacy BLS scheme |
| 2       | Serialisation of `sig` using basic BLS scheme  |

BLS enforced scheme
--------
Once the v19 hard fork is activated, all remaining messages containing BLS public keys or signatures will serialise them using the new basic BLS scheme.
The motivation behind this change is the need to be aligned with IETF standards.

List of affected messages:
`dsq`, `dstx`, `mnauth`, `govobj`, `govobjvote`, `qrinfo`, `qsigshare`, `qsigrec`, `isdlock`, `clsig`, and all DKG messages (`qfcommit`, `qcontrib`, `qcomplaint`, `qjustify`, `qpcommit`).
