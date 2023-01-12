Updated RPCs
--------

- `quorum info`: New field `previouslyFailedConsecutiveDKGs` will be returned for rotated LLMQs. This field will hold the number of previously consecutive failed DGKs before the currently active one. Note: If no previously commitments were found then 0 will be returned for `previouslyFailedConsecutiveDKGs`.
