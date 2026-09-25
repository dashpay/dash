Masternode configuration
------------------------

A masternode (`-masternodeblsprivkey`) now refuses to start unless:

* `-blocksonly=0`
* `-peerblockfilters=1`
* `-maxuploadtarget=0`
* `-minrelaytxfee`, `-incrementalrelayfee`, `-dustrelayfee` and `-bytespersigop` are not above
  their defaults
* `-datacarrier=1`, `-datacarriersize` is not below its default and `-permitbaremultisig=1`
* `-limitancestorcount`, `-limitancestorsize`, `-limitdescendantcount`, `-limitdescendantsize`
  and `-maxmempool` are not below their defaults

The relay policy checks do not apply on regtest.
