#!/usr/bin/env python3
# Copyright (c) 2018-2025 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Tests around dash governance."""

from copy import deepcopy
from decimal import Decimal
import json

from test_framework.authproxy import JSONRPCException
from test_framework.messages import CBlock, COIN, from_hex, uint256_to_string
from test_framework.test_framework import DashTestFramework
from test_framework.governance import have_trigger_for_height, prepare_object
from test_framework.util import assert_equal, force_finish_mnsync, satoshi_round

GOVERNANCE_UPDATE_MIN = 60 * 60 # src/governance/object.h

class DashGovernanceTest (DashTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser)

    def set_test_params(self):
        self.set_dash_test_params(6, 5, [[
            "-budgetparams=10:10:10",
        ]] * 6)
        self.delay_v20_and_mn_rr(height=160)

    def check_superblockbudget(self, v20_active):
        v20_state = self.nodes[0].getdeploymentinfo()["deployments"]["v20"]
        assert_equal(v20_state["active"], v20_active)
        assert_equal(self.nodes[0].getsuperblockbudget(120), self.expected_old_budget)
        assert_equal(self.nodes[0].getsuperblockbudget(140), self.expected_old_budget)
        assert_equal(self.nodes[0].getsuperblockbudget(160), self.expected_v20_budget)
        assert_equal(self.nodes[0].getsuperblockbudget(180), self.expected_v20_budget)

    def check_superblock(self):
        # Make sure Superblock has only payments that fit into the budget
        # p0 must always be included because it has most votes
        # p1 and p2 have equal number of votes (but less votes than p0)
        # so only one of them can be included (depends on proposal hashes).

        coinbase_outputs = self.nodes[0].getblock(self.nodes[0].getbestblockhash(), 2)["tx"][0]["vout"]
        payments_found = 0
        for txout in coinbase_outputs:
            if txout["value"] == self.p0_amount and txout["scriptPubKey"]["address"] == self.p0_payout_address:
                payments_found += 1
            if txout["value"] == self.p1_amount and txout["scriptPubKey"]["address"] == self.p1_payout_address:
                if self.p1_hash > self.p2_hash:
                    payments_found += 1
                else:
                    assert False
            if txout["value"] == self.p2_amount and txout["scriptPubKey"]["address"] == self.p2_payout_address:
                if self.p2_hash > self.p1_hash:
                    payments_found += 1
                else:
                    assert False

        assert_equal(payments_found, 2)

    def test_distinct_required_payments(self):
        node = self.nodes[0]
        self.sync_all()
        parent_height = node.getblockcount()
        target_height = node.getgovernanceinfo()["nextsuperblock"]
        assert_equal(target_height - parent_height, 20)
        assert_equal(len(node.protx("list", "valid")), self.mn_count)
        # Twenty blocks are four full payment cycles of the five regular masternodes.
        owner_addresses = {mn.rewards_address for mn in self.mninfo}
        payment, = [p for p in node.masternode("payments", node.getbestblockhash())[0]["masternodes"][0]["payees"] if p["address"] in owner_addresses]
        payment_amount = Decimal(payment["amount"]) / COIN
        assert payment_amount <= node.getsuperblockbudget(target_height)

        node.sporkupdate("SPORK_19_CHAINLOCKS_ENABLED", 4070908800)
        self.wait_for_sporks_same()
        self.bump_mocktime(GOVERNANCE_UPDATE_MIN + 1, update_schedulers=False)
        for proposal_hash in (self.p0_hash, self.p1_hash, self.p2_hash):
            node.gobject("vote-many", proposal_hash, "funding", "no")
            self.wait_until(lambda: all(n.gobject("get", proposal_hash)["FundingResult"]["NoCount"] == self.mn_count for n in self.nodes))

        proposal_time = self.mocktime
        proposal = prepare_object(node, 1, uint256_to_string(0), proposal_time, 1,
                                  "DistinctPayments", payment_amount, payment["address"])
        self.bump_mocktime(6)
        self.generate(node, 6)
        proposal_hash = node.gobject("submit", "0", 1, proposal_time, proposal["hex"], proposal["collateralHash"])
        self.wait_until(lambda: all(proposal_hash in n.gobject("list") for n in self.nodes))
        node.gobject("vote-many", proposal_hash, "funding", "yes")
        self.wait_until(lambda: all(n.gobject("get", proposal_hash)["FundingResult"]["YesCount"] == self.mn_count for n in self.nodes))
        trigger_hash = None
        trigger_voted = False
        while node.getblockcount() < target_height - 1:
            self.bump_mocktime(1)
            # Flush tip callbacks before another payee can create a competing trigger.
            self.generate(node, 1)
            if trigger_hash is not None:
                if not trigger_voted:
                    self.wait_until(lambda: all(n.gobject("get", trigger_hash)["FundingResult"]["YesCount"] == self.mn_count for n in self.nodes))
                    trigger_voted = True
                continue
            triggers = {h for n in self.nodes for h, trigger in n.gobject("list", "valid", "triggers").items()
                        if json.loads(trigger["DataString"])["event_block_height"] == target_height}
            if triggers:
                trigger_hash, = triggers
                # Recipients vote on the next tip; first relay this exact trigger to everyone.
                self.wait_until(lambda: all(trigger_hash in n.gobject("list", "valid", "triggers") for n in self.nodes))
        assert trigger_voted
        self.wait_until(lambda: have_trigger_for_height(self.nodes, target_height))

        def check_validator(active):
            assert_equal(node.mnsync("status")["IsSynced"], True)
            deployments = node.getdeploymentinfo()["deployments"]
            if active:
                assert_equal(deployments["distinct_required_payments"]["active"], True)
            else:
                # NEVER_ACTIVE deployments are omitted from this RPC.
                assert "distinct_required_payments" not in deployments
            assert_equal(have_trigger_for_height([node], target_height), True)
            template = node.getblocktemplate()
            assert_equal(template["height"], target_height)
            superblock_payment, = template["superblock"]
            assert_equal(superblock_payment["script"], payment["script"])
            assert_equal(superblock_payment["amount"], payment["amount"])
            assert any(p["script"] == payment["script"] and p["amount"] == payment["amount"] for p in template["masternode"])
            assert_equal(node.spork("active")["SPORK_19_CHAINLOCKS_ENABLED"], False)
            try:
                assert node.getbestchainlock()["height"] < target_height
            except JSONRPCException as error:
                assert_equal(error.error["code"], -32603)
                assert_equal(error.error["message"], "Unable to find any ChainLock")

        check_validator(False)
        miner_address = node.getnewaddress()
        result = self.generateblock(node, miner_address, [], False, sync_fun=self.no_op)
        valid_block = from_hex(CBlock(), result["hex"])
        shared_block = deepcopy(valid_block)
        coinbase = shared_block.vtx[0]
        required_outputs = [i for i, output in enumerate(coinbase.vout)
                            if output.scriptPubKey.hex() == payment["script"] and output.nValue == payment["amount"]]
        assert_equal(len(required_outputs), 2)
        miner_script = node.getaddressinfo(miner_address)["scriptPubKey"]
        assert miner_script != payment["script"]
        miner_output, = [output for output in coinbase.vout if output.scriptPubKey.hex() == miner_script]
        miner_output.nValue += coinbase.vout.pop(required_outputs[-1]).nValue
        assert_equal(sum(output.nValue for output in coinbase.vout), sum(output.nValue for output in valid_block.vtx[0].vout))
        assert_equal(sum(output.scriptPubKey.hex() == payment["script"] and output.nValue == payment["amount"] for output in coinbase.vout), 1)
        coinbase.rehash()
        shared_block.hashMerkleRoot = shared_block.calc_merkle_root()
        shared_block.solve()

        # Keep this historical acceptance local so other nodes retain the honest parent.
        self.isolate_node(0)
        parent_hash = node.getbestblockhash()
        check_validator(False)
        self.log.info("Dormant distinct_required_payments accepts a shared MN/treasury output")
        assert_equal(node.submitblock(shared_block.serialize().hex()), None)
        assert_equal(node.getbestblockhash(), shared_block.hash)
        dormant_hash = shared_block.hash
        node.invalidateblock(shared_block.hash)
        assert_equal(node.getbestblockhash(), parent_hash)

        self.restart_node(0, extra_args=node.extra_args + ["-vbparams=distinct_required_payments:-1:-1", "-networkactive=0"])
        # Startup reconsiders invalidated blocks before governance sync is complete.
        node.invalidateblock(dormant_hash)
        assert_equal(node.getbestblockhash(), parent_hash)
        self.reconnect_isolated_node(0, 1)
        force_finish_mnsync(node)
        self.wait_until(lambda: have_trigger_for_height([node], target_height))
        self.isolate_node(0)
        check_validator(True)
        self.log.info("Active distinct_required_payments accepts two distinct equal outputs")
        assert_equal(node.submitblock(valid_block.serialize().hex()), None)
        assert_equal(node.getbestblockhash(), result["hash"])
        node.invalidateblock(result["hash"])
        assert_equal(node.getbestblockhash(), parent_hash)
        check_validator(True)
        # A fresh hash prevents duplicate-block handling from reusing the dormant verdict.
        shared_block.nTime += 1
        shared_block.solve()
        assert shared_block.hash != dormant_hash
        self.log.info("Active distinct_required_payments rejects the shared output at ConnectBlock")
        with node.assert_debug_log([f"Masternode and superblock payments share an output at height {target_height}"]):
            assert_equal(node.submitblock(shared_block.serialize().hex()), "bad-cb-payee")
        assert_equal(node.getbestblockhash(), parent_hash)

    def run_test(self):
        self.log.info("Start testing...")
        governance_info = self.nodes[0].getgovernanceinfo()
        assert_equal(governance_info['governanceminquorum'], 1)
        assert_equal(governance_info['proposalfee'], 1)
        assert_equal(governance_info['superblockcycle'], 20)
        assert_equal(governance_info['superblockmaturitywindow'], 10)
        assert_equal(governance_info['lastsuperblock'], 120)
        assert_equal(governance_info['nextsuperblock'], governance_info['lastsuperblock'] + governance_info['superblockcycle'])
        assert_equal(governance_info['governancebudget'], 1000)

        map_vote_outcomes = {
            0: "none",
            1: "yes",
            2: "no",
            3: "abstain"
        }
        map_vote_signals = {
            0: "none",
            1: "funding",
            2: "valid",
            3: "delete",
            4: "endorsed"
        }
        sb_cycle = 20
        sb_maturity_window = 10
        sb_immaturity_window = sb_cycle - sb_maturity_window
        self.expected_old_budget = satoshi_round("1000")
        self.expected_v20_budget = satoshi_round("18.57142860")

        self.nodes[0].sporkupdate("SPORK_2_INSTANTSEND_ENABLED", 4070908800)
        self.wait_for_sporks_same()

        assert_equal(len(self.nodes[0].gobject("list-prepared")), 0)

        self.log.info("Check 1st superblock before v20")
        self.bump_mocktime(3)
        self.generate(self.nodes[0], 2, sync_fun=self.sync_blocks())
        assert_equal(self.nodes[0].getblockcount(), 137)
        assert_equal(self.nodes[0].getdeploymentinfo()["deployments"]["v20"]["active"], False)
        self.check_superblockbudget(False)

        self.log.info("Check 2nd superblock before v20")
        self.bump_mocktime(3)
        self.generate(self.nodes[0], 3, sync_fun=self.sync_blocks())
        assert_equal(self.nodes[0].getblockcount(), 140)
        assert_equal(self.nodes[0].getdeploymentinfo()["deployments"]["v20"]["active"], False)
        self.check_superblockbudget(False)

        self.log.info("Prepare proposals")
        proposal_time = self.mocktime
        self.p0_payout_address = self.nodes[0].getnewaddress()
        self.p1_payout_address = self.nodes[0].getnewaddress()
        self.p2_payout_address = self.nodes[0].getnewaddress()
        self.p0_amount = satoshi_round("1.1")
        self.p1_amount = satoshi_round("3.3")
        self.p2_amount = self.expected_v20_budget - self.p1_amount

        p0_collateral_prepare = prepare_object(self.nodes[0], 1, uint256_to_string(0), proposal_time, 1, "Proposal_0", self.p0_amount, self.p0_payout_address)
        p1_collateral_prepare = prepare_object(self.nodes[0], 1, uint256_to_string(0), proposal_time, 1, "Proposal_1", self.p1_amount, self.p1_payout_address)
        p2_collateral_prepare = prepare_object(self.nodes[0], 1, uint256_to_string(0), proposal_time, 1, "Proposal_2", self.p2_amount, self.p2_payout_address)

        self.bump_mocktime(6)
        self.generate(self.nodes[0], 6, sync_fun=self.sync_blocks())

        assert_equal(len(self.nodes[0].gobject("list-prepared")), 3)
        assert_equal(len(self.nodes[0].gobject("list")), 0)

        self.log.info("Submit objects")
        self.p0_hash = self.nodes[0].gobject("submit", "0", 1, proposal_time, p0_collateral_prepare["hex"], p0_collateral_prepare["collateralHash"])
        self.p1_hash = self.nodes[0].gobject("submit", "0", 1, proposal_time, p1_collateral_prepare["hex"], p1_collateral_prepare["collateralHash"])
        self.p2_hash = self.nodes[0].gobject("submit", "0", 1, proposal_time, p2_collateral_prepare["hex"], p2_collateral_prepare["collateralHash"])

        assert_equal(len(self.nodes[0].gobject("list")), 3)
        self.wait_until(lambda: len(self.nodes[1].gobject("list")) == 3, timeout = 5)

        assert_equal(self.nodes[0].gobject("get", self.p0_hash)["FundingResult"]["YesCount"], 0)
        assert_equal(self.nodes[0].gobject("get", self.p0_hash)["FundingResult"]["NoCount"], 0)

        assert_equal(self.nodes[0].gobject("get", self.p1_hash)["FundingResult"]["YesCount"], 0)
        assert_equal(self.nodes[0].gobject("get", self.p1_hash)["FundingResult"]["NoCount"], 0)

        assert_equal(self.nodes[0].gobject("get", self.p2_hash)["FundingResult"]["YesCount"], 0)
        assert_equal(self.nodes[0].gobject("get", self.p2_hash)["FundingResult"]["NoCount"], 0)

        self.log.info("Cast votes")
        self.nodes[0].gobject("vote-alias", self.p0_hash, map_vote_signals[1], map_vote_outcomes[2], self.mninfo[0].proTxHash)
        self.nodes[0].gobject("vote-many", self.p0_hash, map_vote_signals[1], map_vote_outcomes[1])
        assert_equal(self.nodes[0].gobject("get", self.p0_hash)["FundingResult"]["YesCount"], self.mn_count - 1)
        assert_equal(self.nodes[0].gobject("get", self.p0_hash)["FundingResult"]["NoCount"], 1)
        self.wait_until(lambda: self.nodes[1].gobject("get", self.p0_hash)["FundingResult"]["YesCount"] == self.mn_count - 1, timeout = 5)
        self.wait_until(lambda: self.nodes[1].gobject("get", self.p0_hash)["FundingResult"]["NoCount"] == 1, timeout = 5)

        self.nodes[0].gobject("vote-alias", self.p1_hash, map_vote_signals[1], map_vote_outcomes[2], self.mninfo[0].proTxHash)
        self.nodes[0].gobject("vote-alias", self.p1_hash, map_vote_signals[1], map_vote_outcomes[2], self.mninfo[1].proTxHash)
        self.nodes[0].gobject("vote-many", self.p1_hash, map_vote_signals[1], map_vote_outcomes[1])
        assert_equal(self.nodes[0].gobject("get", self.p1_hash)["FundingResult"]["YesCount"], self.mn_count - 2)
        assert_equal(self.nodes[0].gobject("get", self.p1_hash)["FundingResult"]["NoCount"], 2)
        self.wait_until(lambda: self.nodes[1].gobject("get", self.p1_hash)["FundingResult"]["YesCount"] == self.mn_count - 2, timeout = 5)
        self.wait_until(lambda: self.nodes[1].gobject("get", self.p1_hash)["FundingResult"]["NoCount"] == 2, timeout = 5)

        self.nodes[0].gobject("vote-alias", self.p2_hash, map_vote_signals[1], map_vote_outcomes[2], self.mninfo[0].proTxHash)
        self.nodes[0].gobject("vote-alias", self.p2_hash, map_vote_signals[1], map_vote_outcomes[2], self.mninfo[1].proTxHash)
        self.nodes[0].gobject("vote-many", self.p2_hash, map_vote_signals[1], map_vote_outcomes[1])
        assert_equal(self.nodes[0].gobject("get", self.p2_hash)["FundingResult"]["YesCount"], self.mn_count - 2)
        assert_equal(self.nodes[0].gobject("get", self.p2_hash)["FundingResult"]["NoCount"], 2)
        self.wait_until(lambda: self.nodes[1].gobject("get", self.p2_hash)["FundingResult"]["YesCount"] == self.mn_count - 2, timeout = 5)
        self.wait_until(lambda: self.nodes[1].gobject("get", self.p2_hash)["FundingResult"]["NoCount"] == 2, timeout = 5)

        assert_equal(len(self.nodes[0].gobject("list", "valid", "triggers")), 0)
        # 5 nodes voted on 3 proposals so we expect to see 15 votes total
        assert_equal(self.nodes[0].gobject("count")["votes"], 15)
        assert_equal(self.nodes[1].gobject("count")["votes"], 15)

        block_count = self.nodes[0].getblockcount()

        self.log.info("Move until 1 block before the Superblock maturity window starts")
        n = sb_immaturity_window - block_count % sb_cycle
        self.log.info("v20 is expected to be activate since block 160")
        assert block_count + n < 160
        for _ in range(n - 1):
            self.bump_mocktime(1)
            self.generate(self.nodes[0], 1, sync_fun=self.sync_blocks())
            self.check_superblockbudget(False)

        assert_equal(len(self.nodes[0].gobject("list", "valid", "triggers")), 0)

        self.log.info("Detect payee node")
        mn_list = self.nodes[0].protx("list", "registered", True)
        height_protx_list = []
        for mn in mn_list:
            height_protx_list.append((mn['state']['lastPaidHeight'], mn['proTxHash']))

        height_protx_list = sorted(height_protx_list)
        _, mn_payee_protx = height_protx_list[1]

        payee_idx = None
        for mn in self.mninfo:
            if mn.proTxHash == mn_payee_protx:
                payee_idx = mn.nodeIdx
                break
        assert payee_idx is not None

        self.log.info("Isolate payee node and create a trigger")
        self.isolate_node(payee_idx)
        isolated = self.nodes[payee_idx]

        self.log.info("Move 1 block inside the Superblock maturity window on the isolated node")
        self.bump_mocktime(1)
        self.generate(isolated, 1, sync_fun=self.no_op)
        self.log.info("The isolated 'winner' should submit new trigger and vote for it")
        self.wait_until(lambda: len(isolated.gobject("list", "valid", "triggers")) == 1, timeout=5)
        isolated_trigger_hash = list(isolated.gobject("list", "valid", "triggers").keys())[0]
        self.wait_until(lambda: list(isolated.gobject("list", "valid", "triggers").values())[0]['YesCount'] == 1, timeout=5)
        more_votes = self.wait_until(lambda: list(isolated.gobject("list", "valid", "triggers").values())[0]['YesCount'] > 1, timeout=5, do_assert=False)
        assert_equal(more_votes, False)
        # Isolated node created a trigger and voted YES for it (16 votes total)
        assert_equal(isolated.gobject("count")["votes"], 16)
        # Non-isolated nodes don't see this (still 15 votes total)
        assert_equal(self.nodes[0].gobject("count")["votes"], 15)

        self.log.info("Move 1 block enabling the Superblock maturity window on non-isolated nodes")
        self.bump_mocktime(1)
        self.generate(self.nodes[0], 1, sync_fun=self.no_op)
        assert_equal(self.nodes[0].getblockcount(), 150)
        assert_equal(self.nodes[0].getdeploymentinfo()["deployments"]["v20"]["active"], False)
        self.check_superblockbudget(False)

        self.log.info("The 'winner' should submit new trigger and vote for it, but it's isolated so no triggers should be found")
        has_trigger = self.wait_until(lambda: len(self.nodes[0].gobject("list", "valid", "triggers")) >= 1, timeout=5, do_assert=False)
        assert_equal(has_trigger, False)

        self.log.info("Move 1 block inside the Superblock maturity window on non-isolated nodes")
        self.bump_mocktime(1)
        self.generate(self.nodes[0], 1, sync_fun=self.no_op)

        self.log.info("There is now new 'winner' who should submit new trigger and vote for it")
        self.wait_until(lambda: len(self.nodes[0].gobject("list", "valid", "triggers")) == 1, timeout=5)
        winning_trigger_hash = list(self.nodes[0].gobject("list", "valid", "triggers").keys())[0]
        self.wait_until(lambda: list(self.nodes[0].gobject("list", "valid", "triggers").values())[0]['YesCount'] == 1, timeout=5)
        more_votes = self.wait_until(lambda: list(self.nodes[0].gobject("list", "valid", "triggers").values())[0]['YesCount'] > 1, timeout=5, do_assert=False)
        assert_equal(more_votes, False)
        # Non-isolated node created a trigger and voted YES for it (16 votes total)
        assert_equal(self.nodes[0].gobject("count")["votes"], 16)
        # Isolated node don't see this (still 16 votes total)
        assert_equal(isolated.gobject("count")["votes"], 16)

        self.log.info("Make sure amounts aren't trimmed")
        payment_amounts_expected = [str(satoshi_round(str(self.p0_amount))), str(satoshi_round(str(self.p1_amount))), str(satoshi_round(str(self.p2_amount)))]
        data_string = list(self.nodes[0].gobject("list", "valid", "triggers").values())[0]["DataString"]
        payment_amounts_trigger = json.loads(data_string)["payment_amounts"].split("|")
        for amount_str in payment_amounts_trigger:
            assert amount_str in payment_amounts_expected

        self.log.info("Move another block inside the Superblock maturity window on non-isolated nodes")
        self.bump_mocktime(1)
        self.generate(self.nodes[0], 1, sync_fun=self.no_op)

        self.log.info("Every non-isolated MN should vote for the same trigger now, no new triggers should be created")
        self.wait_until(lambda: list(self.nodes[0].gobject("list", "valid", "triggers").values())[0]['YesCount'] == self.mn_count - 1, timeout=5)
        more_triggers = self.wait_until(lambda: len(self.nodes[0].gobject("list", "valid", "triggers")) > 1, timeout=5, do_assert=False)
        assert_equal(more_triggers, False)
        # All 4 non-isolated nodes voted YES for a trigger created by a non-isolated node earlier (19 votes total)
        assert_equal(self.nodes[0].gobject("count")["votes"], 19)
        # Isolated node don't see this (still 16 votes total)
        assert_equal(isolated.gobject("count")["votes"], 16)

        self.reconnect_isolated_node(payee_idx, 0)
        # self.connect_nodes(0, payee_idx)
        self.sync_blocks()

        # re-sync helper
        def sync_gov(node):
            self.bump_mocktime(1)
            return node.mnsync("status")["IsSynced"]

        self.log.info("make sure isolated node is fully synced at this point")
        self.wait_until(lambda: sync_gov(isolated))
        self.log.info("let all fulfilled requests expire for re-sync to work correctly")
        self.bump_mocktime(5 * 60)

        for node in self.nodes:
            # Force sync
            node.mnsync("reset")
            # fast-forward to governance sync
            node.mnsync("next")
            self.wait_until(lambda: sync_gov(node))

        self.log.info("Should see two triggers now")
        self.wait_until(lambda: len(isolated.gobject("list", "valid", "triggers")) == 2, timeout=5)
        self.wait_until(lambda: len(self.nodes[0].gobject("list", "valid", "triggers")) == 2, timeout=5)
        more_triggers = self.wait_until(lambda: len(self.nodes[0].gobject("list", "valid", "triggers")) > 2, timeout=5, do_assert=False)
        assert_equal(more_triggers, False)

        self.log.info("Move another block inside the Superblock maturity window")
        self.bump_mocktime(1)
        self.generate(self.nodes[0], 1, sync_fun=self.sync_blocks())

        self.log.info("Should see same YES and NO vote count for both triggers on all nodes now")
        for node in self.nodes:
            self.wait_until(lambda: node.gobject("list", "valid", "triggers")[winning_trigger_hash]['YesCount'] == self.mn_count - 1, timeout=5)
            self.wait_until(lambda: node.gobject("list", "valid", "triggers")[winning_trigger_hash]['NoCount'] == 1, timeout=5)
            self.wait_until(lambda: node.gobject("list", "valid", "triggers")[isolated_trigger_hash]['YesCount'] == 1, timeout=5)
            self.wait_until(lambda: node.gobject("list", "valid", "triggers")[isolated_trigger_hash]['NoCount'] == self.mn_count - 1, timeout=5)

        self.log.info("Should have 25 votes on all nodes")
        # All 4 non-isolated nodes voted NO for a trigger created by a now reconnected node.
        # They also see 1 YES vote for this trigger the reconnected node created earlier.
        # The reconnected node received earlier votes from non-isolated ones and
        # voted NO vote for the trigger non-isolated node created.
        # So everyone should be on the same page now with 25 votes total.
        for node in self.nodes:
            assert_equal(node.gobject("count")["votes"], 25)

        self.log.info("Remember vote count")
        before = self.nodes[1].gobject("count")["votes"]

        self.log.info("Bump mocktime to let MNs vote again")
        self.bump_mocktime(GOVERNANCE_UPDATE_MIN + 1, update_schedulers=False)

        self.log.info("Move another block inside the Superblock maturity window")
        with self.nodes[1].assert_debug_log(["VoteGovernanceTriggers --"]):
            self.bump_mocktime(1)
            self.generate(self.nodes[0], 1, sync_fun=self.sync_blocks())

        self.log.info("Vote count should not change even though MNs are allowed to vote again")
        assert_equal(before, self.nodes[1].gobject("count")["votes"])
        self.log.info("Revert mocktime back to avoid issues in tests below")
        self.bump_mocktime(GOVERNANCE_UPDATE_MIN * -1, update_schedulers=False)

        block_count = self.nodes[0].getblockcount()
        n = sb_cycle - block_count % sb_cycle

        self.log.info("Move remaining n blocks until actual Superblock")
        for i in range(n):
            self.bump_mocktime(1)
            self.generate(self.nodes[0], 1, sync_fun=self.sync_blocks())
            # comparing to 159 because bip9 forks are active when the tip is one block behind the activation height
            self.check_superblockbudget(block_count + i + 1 >= 159)

        self.check_superblockbudget(True)
        self.check_superblock()

        self.log.info("Move a few block past the recent superblock height and make sure we have no new votes")
        for _ in range(5):
            with self.nodes[1].assert_debug_log(expected_msgs=[""], unexpected_msgs=[f"Voting NO-FUNDING for trigger:{winning_trigger_hash} success"]):
                self.bump_mocktime(1)
                self.generate(self.nodes[0], 1, sync_fun=self.sync_blocks())
            # Votes on both triggers should NOT change
            assert_equal(self.nodes[0].gobject("list", "valid", "triggers")[winning_trigger_hash]['NoCount'], 1)
            assert_equal(self.nodes[0].gobject("list", "valid", "triggers")[isolated_trigger_hash]['NoCount'], self.mn_count - 1)

        block_count = self.nodes[0].getblockcount()
        n = sb_immaturity_window - block_count % sb_cycle
        assert n > 0

        self.log.info("Move remaining n blocks until the next maturity window")
        self.bump_mocktime(n)
        self.generate(self.nodes[0], n, sync_fun=self.sync_blocks())

        self.log.info("Move inside maturity window until the next Superblock")
        for _ in range(sb_maturity_window - 1):
            self.bump_mocktime(1)
            self.generate(self.nodes[0], 1, sync_fun=self.sync_blocks())
            self.wait_until(lambda: have_trigger_for_height(self.nodes, 180), timeout=1, do_assert=False)
        self.log.info("Wait for new trigger and votes")
        self.wait_until(lambda: have_trigger_for_height(self.nodes, 180))
        self.log.info("Mine superblock")
        self.bump_mocktime(1)
        self.generate(self.nodes[0], 1, sync_fun=self.sync_blocks())
        assert_equal(self.nodes[0].getblockcount(), 180)
        assert_equal(self.nodes[0].getdeploymentinfo()["deployments"]["v20"]["active"], True)

        self.log.info("Mine and check a couple more superblocks")
        for i in range(2):
            sb_block_height = 180 + (i + 1) * sb_cycle
            self.bump_mocktime(sb_immaturity_window)
            self.generate(self.nodes[0], sb_immaturity_window, sync_fun=self.sync_blocks())
            for _ in range(sb_maturity_window - 1):
                self.bump_mocktime(1)
                self.generate(self.nodes[0], 1, sync_fun=self.sync_blocks())
                self.wait_until(lambda: have_trigger_for_height(self.nodes, sb_block_height), timeout=1, do_assert=False)
            # Wait for new trigger and votes
            self.wait_until(lambda: have_trigger_for_height(self.nodes, sb_block_height))
            # Mine superblock
            self.bump_mocktime(1)
            self.generate(self.nodes[0], 1, sync_fun=self.sync_blocks())
            assert_equal(self.nodes[0].getblockcount(), sb_block_height)
            assert_equal(self.nodes[0].getdeploymentinfo()["deployments"]["v20"]["active"], True)
            self.check_superblockbudget(True)
            self.check_superblock()

        # This restarts an isolated validator with the draft gate forced active; keep it last.
        self.test_distinct_required_payments()


if __name__ == '__main__':
    DashGovernanceTest().main()
