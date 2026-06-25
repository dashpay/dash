#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test v4 masternode owner payout shares."""

from test_framework.blocktools import create_block, create_coinbase
from test_framework.messages import CTxOut, tx_from_hex
from test_framework.script import CScript
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

    def build_block(self, node, tamper=None):
        """Build a block extending the tip whose coinbase pays exactly the template's
        masternode payouts, optionally mutated by ``tamper(coinbase)`` before solving."""
        bt = node.getblocktemplate()
        tip_hash = bt["previousblockhash"]

        coinbase = create_coinbase(bt["height"])
        coinbase.vout = []
        mn_total = 0
        for payee in bt["masternode"]:
            coinbase.vout.append(CTxOut(payee["amount"], CScript(bytes.fromhex(payee["script"]))))
            mn_total += payee["amount"]
        miner_addr = node.get_deterministic_priv_key().address
        miner_script = CScript(bytes.fromhex(node.getaddressinfo(miner_addr)["scriptPubKey"]))
        coinbase.vout.append(CTxOut(bt["coinbasevalue"] - mn_total, miner_script))

        # The CbTx payload's Merkle roots don't depend on the coinbase outputs (the payee
        # selection is unchanged by our tampering), so reuse the template's payload verbatim.
        coinbase.nVersion = 3
        coinbase.nType = 5  # CbTx
        coinbase.vExtraPayload = bytes.fromhex(bt["coinbase_payload"])

        if tamper is not None:
            tamper(coinbase)
        # create_coinbase() already cached a txid; rehash so it reflects our rewritten outputs.
        coinbase.rehash()

        block = create_block(int(tip_hash, 16), coinbase, ntime=bt["curtime"], version=bt["version"])
        # Quorum commitments from the template are mandatory in the block.
        for tx_obj in bt["transactions"]:
            tx = tx_from_hex(tx_obj["data"])
            if tx.nType == 6:
                block.vtx.append(tx)
        block.hashMerkleRoot = block.calc_merkle_root()
        block.solve()
        return block

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
        oversized_payouts = [{"address": node.getnewaddress(), "reward": 1000} for _ in range(9)]

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

        mn.register(
            node,
            submit=True,
            collateral_txid=mn.collateral_txid,
            collateral_vout=mn.collateral_vout,
            addrs_core_p2p=[f"127.0.0.1:{mn.nodePort}"],
            operator_reward=0,
            payouts=oversized_payouts,
            fundsAddr=mn.fundsAddr,
            expected_assert_code=-8,
            expected_assert_msg="payouts must not contain more than 8 entries",
        )

        protx_hash = mn.register(
            node,
            submit=True,
            collateral_txid=mn.collateral_txid,
            collateral_vout=mn.collateral_vout,
            addrs_core_p2p=[f"127.0.0.1:{mn.nodePort}"],
            operator_reward=25,
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
        assert_equal(node.masternodelist("payee")[f"{mn.collateral_txid}-{mn.collateral_vout}"], f"{payout1}, {payout2}")

        mn.update_registrar(
            node,
            submit=True,
            payouts=[],
            fundsAddr=mn.fundsAddr,
            expected_assert_code=-8,
            expected_assert_msg="payouts must contain at least one entry",
        )

        mn.update_registrar(
            node,
            submit=True,
            payouts=oversized_payouts,
            fundsAddr=mn.fundsAddr,
            expected_assert_code=-8,
            expected_assert_msg="payouts must not contain more than 8 entries",
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

        # DIP-0026 cases 13 & 14: coinbase validation must reject a block that omits an
        # expected owner payout output or pays one the wrong amount. A faithful coinbase
        # is accepted, proving the rejections below are caused by the tampering itself.
        payout1_script = CScript(bytes.fromhex(node.getaddressinfo(payout1)["scriptPubKey"]))
        payout2_script = CScript(bytes.fromhex(node.getaddressinfo(payout2)["scriptPubKey"]))

        def drop_payout1(coinbase):
            coinbase.vout = [out for out in coinbase.vout if out.scriptPubKey != payout1_script]

        def shortpay_payout2(coinbase):
            for out in coinbase.vout:
                if out.scriptPubKey == payout2_script:
                    out.nValue -= 1

        assert_equal(node.submitblock(self.build_block(node, drop_payout1).serialize().hex()), "bad-cb-payee")
        assert_equal(node.submitblock(self.build_block(node, shortpay_payout2).serialize().hex()), "bad-cb-payee")
        assert_equal(node.submitblock(self.build_block(node).serialize().hex()), None)

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

        operator_payout = node.getnewaddress()
        node.sendtoaddress(mn.fundsAddr, 1)
        self.bump_mocktime(10 * 60 + 1)
        self.generate(node, 1, sync_fun=self.no_op)
        service_hash = mn.update_service(
            node,
            submit=True,
            addrs_core_p2p=[f"127.0.0.2:{mn.nodePort}"],
            address_operator=operator_payout,
            fundsAddr=mn.fundsAddr,
        )
        assert service_hash is not None
        self.bump_mocktime(10 * 60 + 1)
        self.generate(node, 1, sync_fun=self.no_op)

        info = node.protx("info", protx_hash)
        assert_equal(info["state"]["version"], 4)
        assert_equal(payout_address_rewards(info["state"]["payouts"]), updated_payouts)

        gbt_payees = [p for p in node.getblocktemplate()["masternode"] if p["script"] != "6a"]
        operator_payees = [p for p in gbt_payees if p["payee"] == operator_payout]
        assert_equal(len(operator_payees), 1)
        owner_payees = [p for p in gbt_payees if p["payee"] != operator_payout]
        assert_equal([p["payee"] for p in owner_payees], [p["address"] for p in updated_payouts])
        masternode_total = sum(p["amount"] for p in gbt_payees)
        operator_amount = masternode_total * 2500 // 10000
        assert_equal(operator_payees[0]["amount"], operator_amount)
        owner_total = masternode_total - operator_amount
        paid_owner_total = 0
        for i, payee in enumerate(owner_payees):
            expected_amount = owner_total - paid_owner_total if i == len(owner_payees) - 1 else owner_total * 1250 // 10000
            assert_equal(payee["amount"], expected_amount)
            paid_owner_total += payee["amount"]

        self.generate(node, 1, sync_fun=self.no_op)
        payments = node.masternode("payments")[0]["masternodes"][0]["payees"]
        payment_payees = [p for p in payments if p["script"] != "6a"]
        rpc_operator_payees = [p for p in payment_payees if p["address"] == operator_payout]
        assert_equal(len(rpc_operator_payees), 1)
        rpc_owner_payees = [p for p in payment_payees if p["address"] != operator_payout]
        assert_equal([p["address"] for p in rpc_owner_payees], [p["address"] for p in updated_payouts])
        assert_equal(rpc_operator_payees[0]["amount"], operator_amount)
        paid_owner_total = 0
        for i, payee in enumerate(rpc_owner_payees):
            expected_amount = owner_total - paid_owner_total if i == len(rpc_owner_payees) - 1 else owner_total * 1250 // 10000
            assert_equal(payee["amount"], expected_amount)
            paid_owner_total += payee["amount"]

        node.sendtoaddress(mn.fundsAddr, 1)
        self.bump_mocktime(10 * 60 + 1)
        self.generate(node, 1, sync_fun=self.no_op)
        revoke_hash = mn.revoke(node, submit=True, reason=1, fundsAddr=mn.fundsAddr)
        assert revoke_hash is not None
        self.bump_mocktime(10 * 60 + 1)
        self.generate(node, 1, sync_fun=self.no_op)

        info = node.protx("info", protx_hash)
        assert_equal(info["state"]["version"], 4)
        assert_equal(payout_address_rewards(info["state"]["payouts"]), updated_payouts)


if __name__ == '__main__':
    MasternodePayoutSharesTest().main()
