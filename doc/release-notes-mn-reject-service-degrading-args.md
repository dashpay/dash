Masternode configuration
------------------------

A masternode now refuses to start with settings that let it stay paid while silently skipping
part of its work. Masternodes started with any of the following will exit with an error at
startup and must remove or restore the setting:

* `-blocksonly=1`: the node rejects all transactions, so it never signs InstantSend locks and
  never relays recovered signatures.
* `-peerblockfilters=0`: the node stops serving BIP157 compact block filters.
* A non-zero `-maxuploadtarget`: once the daily budget minus the block relay reserve is used up,
  the node refuses filtered blocks, historical blocks and BIP35 `mempool` requests while still
  advertising bloom filter support. A cap that never triggers is the same as no cap.
* A transaction relay policy stricter than the default (a raised `-minrelaytxfee`,
  `-incrementalrelayfee` or `-dustrelayfee`, `-datacarrier=0` or a lowered `-datacarriersize`,
  `-permitbaremultisig=0`, or lowered `-limitancestorcount`, `-limitancestorsize`,
  `-limitdescendantcount`, `-limitdescendantsize`). A masternode only signs InstantSend locks
  for transactions that enter its own mempool, so a stricter policy makes it skip signing, for
  example for asset lock transactions when data carrier outputs are disabled. This check does
  not apply on regtest.
