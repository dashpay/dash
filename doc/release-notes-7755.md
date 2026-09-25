Proof of Service
----------------

When `SPORK_23_QUORUM_POSE` is active for a quorum type, DKG members also vote a member bad if
none of their masternode connections to it advertise:

* transaction relay (`relay=1` in the `version` message);
* compact block filters (`NODE_COMPACT_FILTERS`).
