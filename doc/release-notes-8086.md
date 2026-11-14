Proof of Service
----------------

DKG members now also vote a member bad if it persistently does not sign final commitments. This applies to a
masternode that was a valid member of at least 6 commitments mined for quorums of the last 4032 blocks (about a
week) and signed at most a quarter of them. Like the other bad votes, it requires `SPORK_23_QUORUM_POSE`.

A commitment in which the masternode was voted bad adds nothing to its signing history. Once over the threshold,
it therefore keeps being voted bad in the quorums it is selected for until it is PoSe-banned. Reviving it with a
`ProUpServTx`, after fixing the cause, clears the history (#8086).
