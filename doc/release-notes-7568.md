Wallet
------

- CoinJoin coordinators charge one uncooperative participant when a session times
  out without enough entries or signatures, including when every participant
  fails to cooperate. Valid signature retransmissions are accepted, and clients
  told to abandon a session are not charged for subsequently withholding their
  signatures. An invalid signature can charge only the collateral of the entry
  that submitted it. (#7568)
