Added RPCs
--------

The following RPCs were added: `protx register_hpmn`, `protx register_fund_hpmn`, `protx register_prepare_hpmn` and `protx update_service_hpmn`.
These HPMN RPCs correspond to the standard masternode RPCs but have the following additional mandatory arguments: `platformNodeID`, `platformP2PPort` and `platformHTTPPort`.
- `platformNodeID`: Platform P2P node ID, derived from P2P public key.
- `platformP2PPort`: TCP port of Dash Platform peer-to-peer communication between nodes (network byte order).
- `platformHTTPPort`: TCP port of Platform HTTP/API interface (network byte order).
Notes:
- Since the Platform Node ID cannot be known at this time, `platformNodeID` can be null.
- `platformP2PPort`, `platformHTTPPort` and the Core port must be distinct.