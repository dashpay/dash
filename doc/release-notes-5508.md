Updated RPCs
--------

- `protx register_hpmn` is renamed to `protx register_evo`.
- `protx register_fund_hpmn` is renamed to `protx register_fund_evo`.
- `protx register_prepare_hpmn` is renamed to `protx register_prepare_evo`.
- `protx update_service_hpmn` is renamed to `protx update_service_evo`.

Please note that both versions will be accepted for now. Deprecation will be done in the future for backward compatibility.

- `masternodelist` filter `hpmn` is renamed to `evo`.
- `masternode count` return total number of EvoNodes under field `evo` instead of `hpmn`.
-  Description type string `HighPerformance` is renamed to `Evo`: affects most RPCs return MNs details.