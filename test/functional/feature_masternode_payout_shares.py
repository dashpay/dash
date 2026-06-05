#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test v4 masternode owner payout shares."""

from test_framework.test_framework import DashTestFramework, MasternodeInfo, p2p_port
from test_framework.util import assert_equal, softfork_active

V24_ACTIVATION_THRESHOLD = 100


def payout_address_rewards(payouts):
    return [{"address": p["address"], "reward": p["reward"]} for p in payouts]


class MasternodePayoutSharesTest(DashTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser)

    def set_test_params(self):
        self.set_dash_test_params(1, 0, extra_args=[[
            f"-vbparams=v24:{self.mocktime}:999999999999:{V24_ACTIVATION_THRESHOLD}:10:8:6:5:0",
        ]])

    def activate_v24(self):
        while not softfork_active(self.nodes[0], "v24"):
            self.bump_mocktime(50)
            self.generate(self.nodes[0], 50, sync_fun=self.no_op)
        assert softfork_active(self.nodes[0], "v24")

    def run_test(self):
        node = self.nodes[0]
        self.activate_v24()

        mn = MasternodeInfo(evo=False, legacy=False)
        mn.generate_addresses(node)
        mn.nodePort = p2p_port(1)

        payout1 = node.getnewaddress()
        payout2 = node.getnewaddress()
        payouts = [
            {"address": payout1, "reward": 7000},
            {"address": payout2, "reward": 3000},
        ]

        collateral_txid = node.sendmany("", {mn.collateral_address: mn.get_collateral_value(), mn.fundsAddr: 1})
        self.bump_mocktime(10 * 60 + 1)
        self.generate(node, 1, sync_fun=self.no_op)
        mn.collateral_txid = collateral_txid
        mn.collateral_vout = mn.get_collateral_vout(node, collateral_txid)

        mn.register(
            node,
            submit=True,
            collateral_txid=mn.collateral_txid,
            collateral_vout=mn.collateral_vout,
            addrs_core_p2p=[f"127.0.0.1:{mn.nodePort}"],
            operator_reward=0,
            payouts=[],
            fundsAddr=mn.fundsAddr,
            expected_assert_code=-8,
            expected_assert_msg="payouts must contain at least one entry",
        )

        protx_hash = mn.register(
            node,
            submit=True,
            collateral_txid=mn.collateral_txid,
            collateral_vout=mn.collateral_vout,
            addrs_core_p2p=[f"127.0.0.1:{mn.nodePort}"],
            operator_reward=0,
            payouts=payouts,
            fundsAddr=mn.fundsAddr,
        )
        assert protx_hash is not None
        self.bump_mocktime(10 * 60 + 1)
        self.generate(node, 1, sync_fun=self.no_op)
        mn.set_params(proTxHash=protx_hash)

        raw = node.getrawtransaction(protx_hash, 1)
        assert_equal(raw["proRegTx"]["version"], 4)
        assert_equal(payout_address_rewards(raw["proRegTx"]["payouts"]), payouts)

        info = node.protx("info", protx_hash)
        assert_equal(info["state"]["version"], 4)
        assert_equal(payout_address_rewards(info["state"]["payouts"]), payouts)
        assert "payoutAddress" not in info["state"]

        mn.update_registrar(
            node,
            submit=True,
            payouts=[],
            fundsAddr=mn.fundsAddr,
            expected_assert_code=-8,
            expected_assert_msg="payouts must contain at least one entry",
        )

        gbt_payees = [p for p in node.getblocktemplate()["masternode"] if p["script"] != "6a"]
        assert_equal(len(gbt_payees), 2)
        assert_equal(gbt_payees[0]["payee"], payout1)
        assert_equal(gbt_payees[1]["payee"], payout2)
        owner_total = sum(p["amount"] for p in gbt_payees)
        assert_equal(gbt_payees[0]["amount"], owner_total * 7000 // 10000)
        assert_equal(gbt_payees[1]["amount"], owner_total - gbt_payees[0]["amount"])

        self.generate(node, 1, sync_fun=self.no_op)
        payments = node.masternode("payments")[0]["masternodes"][0]["payees"]
        payment_payees = [p for p in payments if p["script"] != "6a"]
        assert_equal([p["address"] for p in payment_payees], [payout1, payout2])

        updated_payouts = [{"address": node.getnewaddress(), "reward": 1250} for _ in range(8)]
        node.sendtoaddress(mn.fundsAddr, 1)
        self.bump_mocktime(10 * 60 + 1)
        self.generate(node, 1, sync_fun=self.no_op)
        update_hash = mn.update_registrar(node, submit=True, payouts=updated_payouts, fundsAddr=mn.fundsAddr)
        assert update_hash is not None
        self.bump_mocktime(10 * 60 + 1)
        self.generate(node, 1, sync_fun=self.no_op)

        update_raw = node.getrawtransaction(update_hash, 1)
        assert_equal(update_raw["proUpRegTx"]["version"], 4)
        assert_equal(payout_address_rewards(update_raw["proUpRegTx"]["payouts"]), updated_payouts)
        assert_equal(payout_address_rewards(node.protx("info", protx_hash)["state"]["payouts"]), updated_payouts)


if __name__ == '__main__':
    MasternodePayoutSharesTest().main()
