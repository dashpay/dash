Proof of Service
----------------

Masternode operators can now see whether their masternode is heading towards being voted bad for not signing
final commitments, before it happens (#8207).

- `masternode status` reports a `signingHistory` object: how many commitments of the last 4032 blocks (about a
  week) the masternode was a valid member of and how many of those it signed, counted since its registration or
  last revival, `recentUnsigned` (unsigned ones since the last signed one), `oneMissAway`, `expiringSoon`, `atRisk`
  and `votedBad`. The object is absent while the masternode is PoSe-banned or not in the masternode list.
- Members in `quorum info` report `signed`, and entries in `quorum memberof` report `isSigner`.
- A masternode logs a `WARNING` during the DKG sessions it is a member of while its history is at risk or already
  gets it voted bad.

Alert on `signingHistory.atRisk` or `signingHistory.votedBad`. To find the cause, look for `quorum memberof` entries
with `isValidMember: true` and `isSigner: false`, and check `quorum dkgstatus` during the next session. Once fixed,
new quorums show `isSigner: true` and `recentUnsigned` goes back to 0. `atRisk` cannot warn about several unsigned
commitments mined within the same hour.

A masternode that is already voted bad does not have to wait for the PoSe penalties to ban it: a `ProUpRegTx` that
changes the operator key bans it at once, and a `ProUpServTx` with the new key revives it and restarts its signing
history. As with any revival, it moves to the back of the payment queue.
