#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test decentralized masternode shares (shared collateral, reward split, dissolution)."""

import base64
from decimal import Decimal

from test_framework.messages import COIN, CTxOut, tx_from_hex
from test_framework.script import CScript
from test_framework.test_framework import DashTestFramework, p2p_port
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_raises_rpc_error,
    softfork_active,
)

V24_ACTIVATION_THRESHOLD = 100
COLLATERAL = 1000 * COIN
SHARED_COLLATERAL_SCRIPT = "04445348437551"
EARLY_PERIOD_BLOCKS = 20
EARLY_PENALTY = 50 * COIN
DISSOLVE_FEE = 100000


class MasternodeSharesTest(DashTestFramework):
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

    def build_funding_tx(self, node):
        """Returns hex of a transaction with enough inputs to fund the collateral plus fee
        and a change output, but without the collateral output itself (register_shared_prepare
        appends it)."""
        dummy = node.getnewaddress()
        raw = node.createrawtransaction([], {dummy: 1000})
        funded = node.fundrawtransaction(raw, {"feeRate": 0.00010000})["hex"]
        tx = tx_from_hex(funded)
        vout = [out for out in tx.vout if out.nValue != COLLATERAL]
        assert_equal(len(vout), len(tx.vout) - 1)
        tx.vout = vout
        return tx.serialize().hex()

    def register_shared(self, node, shares, port_offset, early_period_blocks=EARLY_PERIOD_BLOCKS,
                        early_penalty=EARLY_PENALTY):
        operator_key = node.bls("generate")["public"]
        voting_address = node.getnewaddress()
        funding_hex = self.build_funding_tx(node)
        prepared = node.protx(
            "register_shared_prepare", funding_hex, shares, f"127.0.0.1:{p2p_port(port_offset)}",
            operator_key, voting_address, 0, early_period_blocks, early_penalty)
        assert_equal(len(prepared["consentHash"]), 64)

        sigs = node.protx("shared_sign", prepared["tx"])
        assert_equal(sorted(s["shareIndex"] for s in sigs), list(range(len(shares))))
        combined = node.protx("shared_combine", prepared["tx"], sigs)

        signed = node.signrawtransactionwithwallet(combined)
        assert_equal(signed["complete"], True)
        protx_hash = node.sendrawtransaction(signed["hex"])
        self.bump_mocktime(10 * 60 + 1)
        self.generate(node, 1, sync_fun=self.no_op)
        return protx_hash, prepared["collateralIndex"]

    def owner_gbt_payees(self, node):
        return [p for p in node.getblocktemplate()["masternode"] if p["script"] != "6a"]

    def run_test(self):
        node = self.nodes[0]
        self.activate_v24()

        self.log.info("Register a two-participant shared masternode")
        refund1, refund2 = node.getnewaddress(), node.getnewaddress()
        owner1, owner2 = node.getnewaddress(), node.getnewaddress()
        shares = [
            {"amount": 600 * COIN, "refundAddress": refund1, "ownerAddress": owner1},
            {"amount": 400 * COIN, "refundAddress": refund2, "ownerAddress": owner2},
        ]
        protx_hash, collateral_index = self.register_shared(node, shares, port_offset=1)

        raw = node.getrawtransaction(protx_hash, 1)
        assert_equal(raw["proRegTx"]["version"], 3)
        assert_equal([s["refundAddress"] for s in raw["proRegTx"]["shares"]], [refund1, refund2])
        assert_equal([s["amount"] for s in raw["proRegTx"]["shares"]], [600 * COIN, 400 * COIN])
        # empty reward script falls back to the refund script
        assert_equal([s["rewardAddress"] for s in raw["proRegTx"]["shares"]], [refund1, refund2])
        assert_equal(raw["proRegTx"]["earlyPeriodBlocks"], EARLY_PERIOD_BLOCKS)
        assert_equal(raw["proRegTx"]["earlyPenalty"], EARLY_PENALTY)
        assert_equal(raw["vout"][collateral_index]["scriptPubKey"]["hex"], SHARED_COLLATERAL_SCRIPT)

        info = node.protx("info", protx_hash)
        assert_equal(info["state"]["version"], 3)
        assert_equal([s["ownerAddress"] for s in info["state"]["shares"]], [owner1, owner2])
        assert "payoutAddress" not in info["state"]
        assert "payouts" not in info["state"]

        self.log.info("A registration with a tampered or misplaced consent signature is rejected by consensus")
        refund_a, refund_b = node.getnewaddress(), node.getnewaddress()
        owner_a, owner_b = node.getnewaddress(), node.getnewaddress()
        shares_sigtest = [
            {"amount": 600 * COIN, "refundAddress": refund_a, "ownerAddress": owner_a},
            {"amount": 400 * COIN, "refundAddress": refund_b, "ownerAddress": owner_b},
        ]
        sigtest_funding = self.build_funding_tx(node)
        sigtest_prepared = node.protx(
            "register_shared_prepare", sigtest_funding, shares_sigtest, f"127.0.0.1:{p2p_port(4)}",
            node.bls("generate")["public"], node.getnewaddress(), 0, EARLY_PERIOD_BLOCKS, EARLY_PENALTY)
        sigtest_sigs = node.protx("shared_sign", sigtest_prepared["tx"])
        assert_equal(sorted(s["shareIndex"] for s in sigtest_sigs), [0, 1])

        def submit_with_sigs(sigs):
            combined = node.protx("shared_combine", sigtest_prepared["tx"], sigs)
            signed = node.signrawtransactionwithwallet(combined)
            assert_equal(signed["complete"], True)
            return node.testmempoolaccept([signed["hex"]])[0], signed["hex"]

        # A single flipped bit in one consent signature must invalidate the registration
        tampered = [dict(s) for s in sigtest_sigs]
        raw_sig = bytearray(base64.b64decode(tampered[0]["signature"]))
        raw_sig[10] ^= 0x01
        tampered[0]["signature"] = base64.b64encode(bytes(raw_sig)).decode()
        res, _ = submit_with_sigs(tampered)
        assert_equal(res["allowed"], False)
        assert_equal(res["reject-reason"], "bad-protx-shares-sig")

        # Two valid consent signatures in each other's slots must also be rejected: each
        # signature is verified against its own share's owner key, in share order
        swapped = [
            {"shareIndex": 0, "signature": sigtest_sigs[1]["signature"]},
            {"shareIndex": 1, "signature": sigtest_sigs[0]["signature"]},
        ]
        res, _ = submit_with_sigs(swapped)
        assert_equal(res["allowed"], False)
        assert_equal(res["reject-reason"], "bad-protx-shares-sig")

        # The untampered signatures in the right slots are accepted, proving the two
        # rejections above were caused by the signatures alone
        res, good_hex = submit_with_sigs(sigtest_sigs)
        assert_equal(res["allowed"], True)

        self.log.info("A template output that is not the declared collateral does not relay")
        # The standardness carve-out only excuses the template script at the payload's declared
        # collateral index; a second template output would become permanently unspendable, so
        # policy must refuse to relay it even inside an otherwise valid shared registration
        extra_out_tx = tx_from_hex(good_hex)
        extra_out_tx.vout.append(CTxOut(1000, CScript(bytes.fromhex(SHARED_COLLATERAL_SCRIPT))))
        res = node.testmempoolaccept([extra_out_tx.serialize().hex()])[0]
        assert_equal(res["allowed"], False)
        assert_equal(res["reject-reason"], "scriptpubkey")

        self.log.info("Owner reward is split by share amounts, remainder to the last entry")
        payees = self.owner_gbt_payees(node)
        assert_equal([p["payee"] for p in payees], [refund1, refund2])
        owner_total = sum(p["amount"] for p in payees)
        assert_equal(payees[0]["amount"], owner_total * (600 * COIN) // COLLATERAL)
        assert_equal(payees[1]["amount"], owner_total - payees[0]["amount"])
        self.generate(node, 1, sync_fun=self.no_op)

        miner_addr = node.getnewaddress()

        self.log.info("A normal transaction cannot spend the shared collateral (consensus, not just policy)")
        # The template input is anyone-can-spend at the script layer, so an empty scriptSig is a
        # complete spend. Drive it through block connection (generateblock runs consensus, not
        # policy) to prove the covenant rule rejects it, and specifically for that reason.
        steal_raw = node.createrawtransaction(
            [{"txid": protx_hash, "vout": collateral_index}], {node.getnewaddress(): 999.99})
        assert_raises_rpc_error(-25, "bad-shared-collateral-spend", self.generateblock, node, miner_addr,
                                [steal_raw], sync_fun=self.no_op)
        # It is also nonstandard for relay
        assert_equal(node.testmempoolaccept([steal_raw])[0]["allowed"], False)

        self.log.info("A normal transaction cannot create a template output (consensus, not just policy)")
        create_raw = node.createrawtransaction([], {node.getnewaddress(): 1})
        create_funded = node.fundrawtransaction(create_raw)["hex"]
        create_tx = tx_from_hex(create_funded)
        create_tx.vout[0].scriptPubKey = CScript(bytes.fromhex(SHARED_COLLATERAL_SCRIPT))
        # Sign the funding input so the only thing left to reject is the template output itself
        create_signed = node.signrawtransactionwithwallet(create_tx.serialize().hex())
        assert_equal(create_signed["complete"], True)
        assert_raises_rpc_error(-25, "bad-shared-collateral-create", self.generateblock, node, miner_addr,
                                [create_signed["hex"]], sync_fun=self.no_op)

        self.log.info("A share owner can update their reward script")
        # fee source must hold mature wallet funds (masternode rewards are still immature coinbase)
        fee_addr = node.getnewaddress()
        node.sendtoaddress(fee_addr, 1)
        self.generate(node, 1, sync_fun=self.no_op)
        reward1 = node.getnewaddress()
        node.protx("update_share", protx_hash, 0, reward1, fee_addr)
        self.bump_mocktime(10 * 60 + 1)
        self.generate(node, 1, sync_fun=self.no_op)
        info = node.protx("info", protx_hash)
        assert_equal(info["state"]["shares"][0]["rewardAddress"], reward1)
        payees = self.owner_gbt_payees(node)
        assert_equal([p["payee"] for p in payees], [reward1, refund2])

        self.log.info("The coinbase actually pays the share reward script while the MN is active")
        # reward1 only ever receives coinbase owner rewards (dissolution refunds go to refund1/2),
        # so a mined coinbase paying it proves shared rewards are really paid, not just templated.
        reward1_script = node.getaddressinfo(reward1)["scriptPubKey"]
        paid_reward1 = False
        for _ in range(6):
            self.bump_mocktime(10 * 60 + 1)
            block_hash = self.generate(node, 1, sync_fun=self.no_op)[0]
            coinbase = node.getblock(block_hash, 2)["tx"][0]
            if any(o["scriptPubKey"]["hex"] == reward1_script for o in coinbase["vout"]):
                paid_reward1 = True
                break
        assert paid_reward1, "shared masternode reward was never paid to the share reward script"

        self.log.info("A plain ProUpRegTx cannot update a shared masternode")
        assert_raises_rpc_error(-8, "masternode is shared", node.protx,
                                "update_registrar", protx_hash, "", "", fee_addr)

        self.log.info("A unanimous registrar update changes the voting key")
        node.sendtoaddress(fee_addr, 1)
        self.generate(node, 1, sync_fun=self.no_op)
        new_voting = node.getnewaddress()
        prepared = node.protx("update_shared_registrar_prepare", protx_hash, "", new_voting, fee_addr)
        sigs = node.protx("shared_sign", prepared["tx"])
        assert_equal(len(sigs), 2)
        node.protx("shared_combine", prepared["tx"], sigs, True)
        self.bump_mocktime(10 * 60 + 1)
        self.generate(node, 1, sync_fun=self.no_op)
        info = node.protx("info", protx_hash)
        assert_equal(info["state"]["votingAddress"], new_voting)
        # the share table is untouched
        assert_equal([s["ownerAddress"] for s in info["state"]["shares"]], [owner1, owner2])

        self.log.info("A registrar update with swapped share signatures is rejected by consensus")
        node.sendtoaddress(fee_addr, 1)
        self.generate(node, 1, sync_fun=self.no_op)
        prepared = node.protx("update_shared_registrar_prepare", protx_hash, "", node.getnewaddress(), fee_addr)
        sigs = node.protx("shared_sign", prepared["tx"])
        assert_equal(sorted(s["shareIndex"] for s in sigs), [0, 1])
        swapped = [
            {"shareIndex": 0, "signature": sigs[1]["signature"]},
            {"shareIndex": 1, "signature": sigs[0]["signature"]},
        ]
        assert_raises_rpc_error(None, "bad-proupsharedreg-sig", node.protx,
                                "shared_combine", prepared["tx"], swapped, True)

        self.log.info("A registrar update cannot move the voting key onto a share's payee script")
        node.sendtoaddress(fee_addr, 1)
        self.generate(node, 1, sync_fun=self.no_op)
        prepared = node.protx("update_shared_registrar_prepare", protx_hash, "", reward1, fee_addr)
        sigs = node.protx("shared_sign", prepared["tx"])
        assert_raises_rpc_error(None, "bad-proupsharedreg-payee-reuse", node.protx,
                                "shared_combine", prepared["tx"], sigs, True)

        self.log.info("A pending registrar update keeps a conflicting share update out of the mempool")
        node.sendtoaddress(fee_addr, 1)
        self.generate(node, 1, sync_fun=self.no_op)
        pair_addr = node.getnewaddress()
        prepared = node.protx("update_shared_registrar_prepare", protx_hash, "", pair_addr, fee_addr)
        sigs = node.protx("shared_sign", prepared["tx"])
        node.protx("shared_combine", prepared["tx"], sigs, True)
        # both pass tip-level checks individually, so only the mempool pair guard keeps an honest
        # miner from assembling a block that consensus would then reject
        assert_raises_rpc_error(None, "protx-dup", node.protx, "update_share", protx_hash, 0, pair_addr, fee_addr)
        self.bump_mocktime(10 * 60 + 1)
        self.generate(node, 1, sync_fun=self.no_op)
        assert_equal(node.protx("info", protx_hash)["state"]["votingAddress"], pair_addr)

        # the reverse direction: a pending share update blocks a registrar update onto its script
        node.sendtoaddress(fee_addr, 1)
        self.generate(node, 1, sync_fun=self.no_op)
        rev_addr = node.getnewaddress()
        node.protx("update_share", protx_hash, 0, rev_addr, fee_addr)
        prepared = node.protx("update_shared_registrar_prepare", protx_hash, "", rev_addr, fee_addr)
        sigs = node.protx("shared_sign", prepared["tx"])
        assert_raises_rpc_error(None, "protx-dup", node.protx,
                                "shared_combine", prepared["tx"], sigs, True)
        self.bump_mocktime(10 * 60 + 1)
        self.generate(node, 1, sync_fun=self.no_op)
        assert_equal(node.protx("info", protx_hash)["state"]["shares"][0]["rewardAddress"], rev_addr)
        # restore share 0's reward script for the sections below
        node.sendtoaddress(fee_addr, 1)
        self.generate(node, 1, sync_fun=self.no_op)
        node.protx("update_share", protx_hash, 0, reward1, fee_addr)
        self.bump_mocktime(10 * 60 + 1)
        self.generate(node, 1, sync_fun=self.no_op)
        assert_equal(node.protx("info", protx_hash)["state"]["shares"][0]["rewardAddress"], reward1)
        self.log.info("A zero-penalty unilateral dissolution is invalid during the early period")
        assert_raises_rpc_error(-8, "must pay the penalty", node.protx,
                                "dissolve", protx_hash, 1, DISSOLVE_FEE, True, False)
        standby_hex = node.protx("dissolve", protx_hash, 1, DISSOLVE_FEE, False, False)
        standby_res = node.testmempoolaccept([standby_hex])[0]
        assert_equal(standby_res["allowed"], False)
        assert_equal(standby_res["reject-reason"], "bad-prodis-penalty-floor")

        self.log.info("A penalty-paying unilateral dissolution succeeds during the early period")
        registered_height = node.protx("info", protx_hash)["state"]["registeredHeight"]
        assert_greater_than(registered_height + EARLY_PERIOD_BLOCKS, node.getblockcount() + 1)
        dissolve_txid = node.protx("dissolve", protx_hash, 1, DISSOLVE_FEE)
        dissolve_tx = node.getrawtransaction(dissolve_txid, 1)
        assert_equal(dissolve_tx["proDisTx"]["proTxHash"], protx_hash)
        assert_equal(dissolve_tx["proDisTx"]["actorIndex"], 1)
        assert_equal(dissolve_tx["proDisTx"]["sigCount"], 1)
        # non-actor share is refunded principal plus the entire penalty, the actor absorbs it
        assert_equal(dissolve_tx["vout"][0]["scriptPubKey"]["address"], refund1)
        assert_equal(int(dissolve_tx["vout"][0]["value"] * COIN), 600 * COIN + EARLY_PENALTY)
        assert_equal(dissolve_tx["vout"][1]["scriptPubKey"]["address"], refund2)
        assert_equal(int(dissolve_tx["vout"][1]["value"] * COIN), 400 * COIN - EARLY_PENALTY - DISSOLVE_FEE)
        self.bump_mocktime(10 * 60 + 1)
        self.generate(node, 1, sync_fun=self.no_op)
        assert_raises_rpc_error(None, None, node.protx, "info", protx_hash)
        assert_equal(node.masternodelist(), {})

        self.log.info("A standby dissolution signed inside the early period is valid after it ends")
        refund3, refund4 = node.getnewaddress(), node.getnewaddress()
        owner3, owner4 = node.getnewaddress(), node.getnewaddress()
        shares2 = [
            {"amount": 500 * COIN, "refundAddress": refund3, "ownerAddress": owner3},
            {"amount": 500 * COIN, "refundAddress": refund4, "rewardAddress": node.getnewaddress(),
             "ownerAddress": owner4},
        ]
        protx_hash2, _ = self.register_shared(node, shares2, port_offset=2)
        standby_hex = node.protx("dissolve", protx_hash2, 0, DISSOLVE_FEE, False, False)
        # a signed unilateral dissolution must not be re-signed via the multi-party flow:
        # shared_sign signs the unanimous digest, which can never verify on a one-signature
        # transaction, so it fails fast instead of producing unusable signatures
        assert_raises_rpc_error(-8, "needs no shared_sign step", node.protx, "shared_sign", standby_hex)
        assert_equal(node.testmempoolaccept([standby_hex])[0]["allowed"], False)
        # mine past the early-period boundary; the same signed hex becomes and stays valid
        self.bump_mocktime(10 * 60 + 1)
        self.generate(node, EARLY_PERIOD_BLOCKS + 1, sync_fun=self.no_op)
        assert_equal(node.testmempoolaccept([standby_hex])[0]["allowed"], True)

        self.log.info("A unanimous dissolution is penalty-free")
        prepared = node.protx("dissolve_prepare", protx_hash2, 0, DISSOLVE_FEE)
        sigs = node.protx("shared_sign", prepared["tx"])
        assert_equal(len(sigs), 2)
        dissolve_txid2 = node.protx("shared_combine", prepared["tx"], sigs, True)
        dissolve_tx2 = node.getrawtransaction(dissolve_txid2, 1)
        assert_equal(dissolve_tx2["proDisTx"]["sigCount"], 2)
        assert_equal(int(dissolve_tx2["vout"][0]["value"] * COIN), 500 * COIN)
        self.bump_mocktime(10 * 60 + 1)
        self.generate(node, 1, sync_fun=self.no_op)
        assert_equal(node.masternodelist(), {})

        self.log.info("A pending dissolution and a same-masternode update can be mined together")
        # A ProDisTx removes the MN in the collateral-spend phase, after all other provider txs in
        # the block are applied, so a same-MN update and the dissolution can share a block in
        # either order. Regression guard: with the removal done mid-provider-loop instead, a block
        # ordering the ProDisTx before the update would fail BuildNewListFromBlock and abort mining.
        refund5, refund6 = node.getnewaddress(), node.getnewaddress()
        owner5, owner6 = node.getnewaddress(), node.getnewaddress()
        shares3 = [
            {"amount": 700 * COIN, "refundAddress": refund5, "ownerAddress": owner5},
            {"amount": 300 * COIN, "refundAddress": refund6, "ownerAddress": owner6},
        ]
        protx_hash3, _ = self.register_shared(node, shares3, port_offset=3)
        fee_addr2 = node.getnewaddress()
        node.sendtoaddress(fee_addr2, 1)
        self.generate(node, 1, sync_fun=self.no_op)
        # Both transactions coexist in the mempool (no false provider conflict). The dissolution
        # pays a high feerate so the assembler tends to order it first — the ordering that used to
        # abort BuildNewListFromBlock.
        dissolve_txid = node.protx("dissolve", protx_hash3, 0, 500000)
        update_txid = node.protx("update_share", protx_hash3, 1, node.getnewaddress(), fee_addr2)
        mempool = node.getrawmempool()
        assert dissolve_txid in mempool
        assert update_txid in mempool
        # Mining must not abort and must eventually confirm the dissolution (whether the pair lands
        # in one block or the update mines first and the dissolution follows).
        self.bump_mocktime(10 * 60 + 1)
        self.generate(node, 2, sync_fun=self.no_op)
        assert_equal(node.getrawmempool(), [])
        assert_raises_rpc_error(None, None, node.protx, "info", protx_hash3)

        self.log.info("Every participant's principal was refunded by the dissolutions")
        # refund1/refund2 were refunded by the first MN's unilateral dissolution, refund3/refund4
        # by the second MN's unanimous dissolution (reward receipts are checked separately above)
        for addr in (refund1, refund2, refund3, refund4, refund5, refund6):
            assert_greater_than(node.getreceivedbyaddress(addr, 1), Decimal(0))


if __name__ == '__main__':
    MasternodeSharesTest().main()
