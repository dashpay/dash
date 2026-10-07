#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test the evo_shares deployment.

v24 follows evo_shares without signing it, so on a real network it stays inactive until a later
release signs it. Until then an EvoNode keeps a single owner payout. The test first runs the
deployment as shipped (masternode-activated), then lets miners activate it (-vbparams with EHF
disabled) to check the restriction lifts.
"""

from test_framework.test_framework import DashTestFramework, MasternodeInfo, p2p_port
from test_framework.util import assert_equal, get_bip9_details, softfork_active

EVO_SHARES_BIT = 14
TESTDUMMY_BIT = 28
V24_MIN_ACTIVATION_HEIGHT = 200


def payout_address_rewards(payouts):
    return [{"address": p["address"], "reward": p["reward"]} for p in payouts]


class EvoSharesActivationTest(DashTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser)

    def set_test_params(self):
        extra_args = [[
            f"-vbparams=v24:{self.mocktime}:999999999999:{V24_MIN_ACTIVATION_HEIGHT}:10:8:6:5:0",
            # A deployment masternodes do sign, to show that the evo_shares check below is not vacuous
            "-vbparams=testdummy:0:999999999999:0:4:4:4:5:1",
            "-persistmempool=0",
        ] for _ in range(2)]
        # One masternode is enough: an MNHF signal only needs a recovered signature of any quorum
        self.set_dash_test_params(2, 1, extra_args=extra_args)
        self.set_dash_llmq_test_params(1, 1)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def mnhf_signals_in_mempool(self, node, version_bit):
        return [txid for txid in node.getrawmempool()
                if node.getrawtransaction(txid, True).get("mnhfTx", {}).get("signal", {}).get("versionBit") == version_bit]

    def test_not_signed(self):
        self.log.info("Masternodes sign other deployments but not evo_shares")
        node = self.nodes[0]
        mn_node = self.mninfo[0].get_node(self)
        self.mine_quorum_single_member()
        with mn_node.assert_debug_log(expected_msgs=[f"bit={TESTDUMMY_BIT} at height"],
                                      unexpected_msgs=[f"bit={EVO_SHARES_BIT} at height"]):
            self.generate(node, 1, sync_fun=self.sync_blocks)
            self.wait_until(lambda: len(self.mnhf_signals_in_mempool(mn_node, TESTDUMMY_BIT)) == 1)
        assert_equal(self.mnhf_signals_in_mempool(mn_node, EVO_SHARES_BIT), [])
        self.generate(mn_node, 1, sync_fun=self.sync_blocks)
        assert_equal(get_bip9_details(node, "evo_shares")["status"], "defined")

    @staticmethod
    def evonode_platform_args(evo):
        return {
            "platform_node_id": "%040x" % evo.nodePort,
            "addrs_platform_p2p": [f"127.0.0.1:{evo.nodePort + 1000}"],
            "addrs_platform_https": [f"127.0.0.1:{evo.nodePort + 2000}"],
        }

    def fund_evonode(self, node, port):
        evo = MasternodeInfo(evo=True, legacy=False)
        evo.generate_addresses(node)
        evo.nodePort = port
        collateral_txid = node.sendmany("", {evo.collateral_address: evo.get_collateral_value(), evo.fundsAddr: 1})
        self.bury_tx(node, collateral_txid)
        evo.collateral_txid = collateral_txid
        evo.collateral_vout = evo.get_collateral_vout(node, collateral_txid)
        return evo

    @staticmethod
    def two_payouts(node):
        return [{"address": node.getnewaddress(), "reward": 6000}, {"address": node.getnewaddress(), "reward": 4000}]

    def test_single_payout(self):
        self.log.info("An EvoNode keeps a single owner payout until evo_shares is active")
        node = self.nodes[0]
        while not softfork_active(node, "v24"):
            self.generate(node, 10, sync_fun=self.sync_blocks)
        assert not softfork_active(node, "evo_shares")
        evo = self.fund_evonode(node, p2p_port(10))
        platform = self.evonode_platform_args(evo)
        evo.register(node, submit=True, payouts=self.two_payouts(node), **platform,
                     expected_assert_code=-1, expected_assert_msg="bad-protx-payouts-evo")
        protx_hash = evo.register(node, submit=True, **platform)
        self.bury_tx(node, protx_hash)
        evo.set_params(proTxHash=protx_hash)

        node.sendtoaddress(evo.fundsAddr, 1)
        evo.update_registrar(node, submit=True, payouts=self.two_payouts(node), fundsAddr=evo.fundsAddr,
                             expected_assert_code=-1, expected_assert_msg="bad-protx-payouts-evo")
        # Rotating the operator key does not open a way around it
        evo.update_registrar(node, submit=True, pubKeyOperator=node.bls("generate")["public"],
                             payouts=self.two_payouts(node), fundsAddr=evo.fundsAddr,
                             expected_assert_code=-1, expected_assert_msg="bad-protx-payouts-evo")
        assert_equal(len(node.protx("info", protx_hash)["state"]["payouts"]), 1)
        return evo

    def activate_evo_shares_by_miners(self):
        self.log.info("Let miners activate evo_shares")
        for i in range(self.num_nodes):
            self.extra_args[i].append("-vbparams=evo_shares:0:999999999999:0:10:8:6:5:0")
        self.restart_node(0)
        mn = self.mninfo[0]
        self.stop_node(mn.nodeIdx)
        self.start_masternode(mn)
        self.connect_nodes(mn.nodeIdx, 0)
        node = self.nodes[0]
        while not softfork_active(node, "evo_shares"):
            self.generate(node, 10, sync_fun=self.sync_blocks)

    def test_multiple_payouts(self, evo):
        self.log.info("EvoNodes can have multiple owner payouts once evo_shares is active")
        node = self.nodes[0]
        payouts = self.two_payouts(node)
        update_hash = evo.update_registrar(node, submit=True, payouts=payouts, fundsAddr=evo.fundsAddr)
        self.bury_tx(node, update_hash)
        assert_equal(payout_address_rewards(node.protx("info", evo.proTxHash)["state"]["payouts"]), payouts)

        evo2 = self.fund_evonode(node, p2p_port(11))
        payouts = self.two_payouts(node)
        protx_hash = evo2.register(node, submit=True, payouts=payouts, **self.evonode_platform_args(evo2))
        self.bury_tx(node, protx_hash)
        assert_equal(payout_address_rewards(node.protx("info", protx_hash)["state"]["payouts"]), payouts)

    def run_test(self):
        self.nodes[0].sporkupdate("SPORK_17_QUORUM_DKG_ENABLED", 0)
        self.wait_for_sporks_same()
        self.test_not_signed()
        evo = self.test_single_payout()
        self.activate_evo_shares_by_miners()
        self.test_multiple_payouts(evo)


if __name__ == '__main__':
    EvoSharesActivationTest().main()
