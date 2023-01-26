Added RPCs
--------

The following RPCs were added `protx register_hpmn`, `protx register_fund_hpmn`, `protx register_prepare_hpmn` and `protx update_service_hpmn`.
Those RPCs are the corresponding versions for HPMNs and they have the following mandatory arguments: `platformNodeID`, `platformP2PPort` and `platformNodeID`.
- `platformNodeID`: Platform P2P node ID, derived from P2P public key.
- `platformP2PPort`: TCP port of Dash Platform peer-to-peer communication between nodes (network byte order).
- `platformNodeID`: TCP port of Platform HTTP/API interface (network byte order).
Notes:
- Since the Platform Node ID can not be known at this time, `platformNodeID` can be null.
- `platformP2PPort`, `platformNodeID` and the Core port must be distinct.