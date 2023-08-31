Coinbase Changes
------------------------

Once v20 activates, miners will produce blocks containing a newer version (=3) of Coinbase transaction.

Version 3 of Coinbase will include the following two fields:
- bestCLHeightDiff (uint32)
- bestCLSignature (BLSSignature)

bestCLSignature will hold the best Chainlock signature known at the moment of mining.
bestCLHeightDiff is equal to the diff in heights from the mined block height to the actual Chainlocked block height.

Although miners are forced to include version 3 Coinbase, actual real-time best Chainlock signature isn't required: as long as blocks will contain at least equal (or newer) bestCLSignature than the previous block.

Note: Until the first bestCLSignature included in Coinbase once v20 activates, null bestCLSignature and bestCLHeightDiff are perfectly valid.