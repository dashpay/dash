Updated RPCs
------------

* Dash RPCs will no longer permit submitting strings to boolean input fields in line with validation
  enforced on upstream RPCs, where this is already the case. Requests must now use unquoted `true`
  or `false`.

  * The following RPCs are affected, `bls generate`, `bls fromsecret`, `coinjoinsalt generate`,
    `coinjoinsalt set`, `masternode connect`, `protx diff`, `protx list`, `protx register`,
    `protx register_legacy`, `protx register_evo`, `protx register_fund`, `protx register_fund_legacy`,
    `protx register_fund_evo`, `protx revoke`, `protx update_registrar`, `protx update_registrar_legacy`,
    `protx update_service`, `protx update_service_evo`, `quorum info`,  `quorum platformsign`,
    `quorum rotationinfo`, `quorum sign`.

  * This restriction can be relaxed by setting `-deprecatedrpc=permissive_bool` at runtime
    but is liable to be removed in future versions of Dash Core.
