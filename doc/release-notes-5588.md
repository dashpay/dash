Block Reward Reallocation
-------------------------

Once Masternode Reward Location Reallocation activates:

- Treasury is bumped to 20% of block subsidy.
- Block reward shares are immediately set to 75% for MN and 25% miners. MN reward share should be 75% of block reward in order to represent 60% of the block subsidy. (according to the proposal).

Note: Previous reallocation periods are dropped.

Updated RPCs
--------

- `getgovernanceinfo` RPC returns the field `governancebudget`: the governance budget for the next superblock.
