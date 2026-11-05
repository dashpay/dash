RPC Wallet
----------

- The `descriptorprocesspsbt` call returns the more specific
  RPC_INVALID_PARAMETER error instead of RPC_MISC_ERROR if its sighashtype
  argument is malformed, as `walletprocesspsbt` does. (dash#7710)
