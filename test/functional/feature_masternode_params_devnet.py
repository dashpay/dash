#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that a masternode refuses a stricter-than-default relay policy outside regtest.

Regtest exempts masternodes from this check (see feature_llmq_is_retroactive.py), so it is
exercised on devnet here.
"""

from test_framework.test_framework import BitcoinTestFramework


class MasternodeParamsDevnetTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.chain = "devnet"
        self.num_nodes = 1

    def run_test(self):
        bls_key = self.nodes[0].bls('generate')['secret']
        self.stop_node(0)
        mn_args = [f"-masternodeblsprivkey={bls_key}", "-listen=1"]
        relay_policy_msg = ("Error: Masternode must not use a stricter transaction relay policy than the default, "
                            "which would make it skip InstantSend signing. Remove or restore: ")
        for extra_args, flags in [
            (["-datacarrier=0"], "-datacarrier/-datacarriersize"),
            (["-minrelaytxfee=0.001"], "-minrelaytxfee"),
            (["-incrementalrelayfee=0.001"], "-incrementalrelayfee"),
            (["-dustrelayfee=0.001", "-permitbaremultisig=0"], "-dustrelayfee, -permitbaremultisig"),
            (["-limitancestorcount=5", "-limitdescendantsize=50"], "-limitancestorcount, -limitdescendantsize"),
            (["-maxmempool=299"], "-maxmempool"),
            (["-bytespersigop=21"], "-bytespersigop"),
        ]:
            self.log.info(f"Test that a masternode refuses {' '.join(extra_args)}")
            self.nodes[0].assert_start_raises_init_error(extra_args=mn_args + extra_args,
                                                         expected_msg=relay_policy_msg + flags)

        self.log.info("Test that a looser relay policy is still allowed")
        self.start_node(0, extra_args=mn_args + ["-minrelaytxfee=0.000001", "-limitancestorcount=50", "-datacarriersize=160",
                                                 "-maxmempool=301", "-bytespersigop=19"])


if __name__ == '__main__':
    MasternodeParamsDevnetTest().main()
