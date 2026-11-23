#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test wallet-dependent Dash RPC paths.

Covers the wallet-mode-divergent surface of Dash-specific RPCs so that
consensus-heavy tests (feature_governance.py, feature_governance_cl.py,
feature_dip3_deterministicmns.py) only need to run in a single wallet mode:

- gobject prepare (wallet-funded collateral), list-prepared, submit
- gobject vote-many and vote-alias (CheckWalletOwnsKey/IsMine and
  CWallet::SignGovernanceVote -> SignMessage SPKM dispatch)
- protx register_fund (wallet-funded collateral)
- protx register (collateral owned by the wallet, payload signed via
  CWallet::SignMessage)
- protx register_prepare + signmessage + register_submit (external
  collateral signing path)
- protx update_service and protx update_registrar

This test runs in both --legacy-wallet and --descriptors modes.
"""

from test_framework.governance import prepare_object
from test_framework.messages import uint256_to_string
from test_framework.test_framework import (
    DashTestFramework,
    MasternodeInfo,
)
from test_framework.util import assert_equal, assert_raises_rpc_error, p2p_port, softfork_active


class WalletDashRPCsTest(DashTestFramework):
    def set_test_params(self):
        self.set_dash_test_params(3, 2)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def add_options(self, parser):
        self.add_wallet_options(parser)

    def prepare_unstarted_mn(self, idx) -> MasternodeInfo:
        mn = MasternodeInfo(evo=False, legacy=(not softfork_active(self.nodes[0], 'v19')))
        mn.generate_addresses(self.nodes[0])
        mn.set_params(nodePort=p2p_port(idx))
        return mn

    def confirm_tx(self, txid):
        assert txid in self.nodes[0].getrawmempool()
        self.bump_mocktime(1)
        block_hash = self.generate(self.nodes[0], 1)[0]
        assert txid in self.nodes[0].getblock(block_hash)["tx"]

    def run_test(self):
        # There are no quorums in this test, so txes can never be InstantSend-locked
        # and would not be mined until they are 10 minutes old. Disable InstantSend
        # to make them mineable right away.
        self.nodes[0].sporkupdate("SPORK_2_INSTANTSEND_ENABLED", 4070908800)
        self.wait_for_sporks_same()

        self.test_gobject_wallet_paths()
        funded_mn = self.test_protx_register_fund()
        self.test_protx_collateral_lock_errors()
        self.test_protx_register_own_collateral()
        self.test_protx_register_external()
        self.test_protx_update_service(funded_mn)
        self.test_protx_update_registrar(funded_mn)

    def test_gobject_wallet_paths(self):
        node = self.nodes[0]
        self.log.info("Test gobject prepare (wallet collateral funding)")
        assert_equal(len(node.gobject("list-prepared")), 0)
        proposal_time = self.mocktime
        payout_address = node.getnewaddress()
        prepared = prepare_object(node, 1, uint256_to_string(0), proposal_time, 1, "wallet_test_proposal", 1, payout_address)
        self.confirm_tx(prepared["collateralHash"])
        # Governance collateral needs GOVERNANCE_FEE_CONFIRMATIONS (6) confirmations
        self.bump_mocktime(5)
        self.generate(node, 5)

        self.log.info("Test gobject list-prepared and submit")
        assert_equal(len(node.gobject("list-prepared")), 1)
        assert_equal(len(node.gobject("list")), 0)
        proposal_hash = node.gobject("submit", "0", 1, proposal_time, prepared["hex"], prepared["collateralHash"])
        assert_equal(len(node.gobject("list")), 1)
        self.wait_until(lambda: len(self.mninfo[0].get_node(self).gobject("list")) == 1, timeout=10)

        self.log.info("Test gobject vote-alias and vote-many (wallet vote signing)")
        alias_result = node.gobject("vote-alias", proposal_hash, "funding", "no", self.mninfo[0].proTxHash)
        assert_equal(alias_result["detail"][self.mninfo[0].proTxHash]["result"], "success")
        # vote-many signs for every masternode whose voting key is in the wallet, but the
        # repeated vote for mninfo[0] is rejected by the GOVERNANCE_UPDATE_MIN rate limit,
        # so only the votes for the remaining masternodes are actually recorded
        many_result = node.gobject("vote-many", proposal_hash, "funding", "yes")
        assert_equal(many_result["detail"][self.mninfo[0].proTxHash]["result"], "failed")
        for mn in self.mninfo[1:]:
            assert_equal(many_result["detail"][mn.proTxHash]["result"], "success")
        assert_equal(node.gobject("get", proposal_hash)["FundingResult"]["YesCount"], self.mn_count - 1)
        assert_equal(node.gobject("get", proposal_hash)["FundingResult"]["NoCount"], 1)
        assert_equal(node.gobject("count")["votes"], self.mn_count)

        self.log.info("Make sure wallet-signed votes are accepted by other nodes")
        mn_node = self.mninfo[0].get_node(self)
        self.wait_until(lambda: mn_node.gobject("get", proposal_hash)["FundingResult"]["YesCount"] == self.mn_count - 1, timeout=10)
        self.wait_until(lambda: mn_node.gobject("get", proposal_hash)["FundingResult"]["NoCount"] == 1, timeout=10)

    def test_protx_register_fund(self) -> MasternodeInfo:
        node = self.nodes[0]
        self.log.info("Test protx register_fund (wallet-funded collateral)")
        mn = self.prepare_unstarted_mn(len(self.nodes) + 1)
        node.sendtoaddress(mn.fundsAddr, mn.get_collateral_value() + 0.001)
        self.bump_mocktime(1)
        self.generate(node, 1)
        txid = mn.register_fund(node, submit=True)
        assert txid is not None
        self.confirm_tx(txid)
        vout = mn.get_collateral_vout(node, txid)
        mn.set_params(proTxHash=txid, collateral_txid=txid, collateral_vout=vout)
        assert txid in node.protx("list", "registered")
        assert_equal(node.protx("info", txid)["collateralAddress"], mn.collateral_address)
        assert "%s-%d" % (txid, vout) in node.masternode("list")
        return mn

    def test_protx_register_own_collateral(self):
        node = self.nodes[0]
        self.log.info("Test protx register (collateral owned by the wallet, wallet-signed payload)")
        mn = self.prepare_unstarted_mn(len(self.nodes) + 2)
        collateral_txid = node.sendtoaddress(mn.collateral_address, mn.get_collateral_value())
        node.sendtoaddress(mn.fundsAddr, 0.001)
        self.bump_mocktime(1)
        self.generate(node, 1)
        collateral_vout = mn.get_collateral_vout(node, collateral_txid)
        mn.set_params(collateral_txid=collateral_txid, collateral_vout=collateral_vout)
        # Unlike register_prepare, this proves ownership of the collateral by signing the
        # payload with the wallet itself (CWallet::SignMessage -> SPKM dispatch)
        raw_tx = mn.register(node, submit=False)
        assert raw_tx is not None
        collateral_outpoint = {"txid": collateral_txid, "vout": collateral_vout}
        assert collateral_outpoint in node.listlockunspent()
        protx_hash = node.sendrawtransaction(raw_tx)
        mn.set_params(proTxHash=protx_hash)
        self.confirm_tx(protx_hash)
        assert protx_hash in node.protx("list", "registered")
        assert_equal(node.protx("info", protx_hash)["collateralHash"], collateral_txid)
        assert "%s-%d" % (collateral_txid, collateral_vout) in node.masternode("list")

    def test_protx_collateral_lock_errors(self):
        node = self.nodes[0]
        self.log.info("Test protx collateral lock ownership on errors")
        mn = self.prepare_unstarted_mn(len(self.nodes) + 4)
        collateral_txid = node.sendtoaddress(mn.collateral_address, mn.get_collateral_value())
        self.bump_mocktime(1)
        self.generate(node, 1)
        collateral_vout = mn.get_collateral_vout(node, collateral_txid)
        mn.set_params(collateral_txid=collateral_txid, collateral_vout=collateral_vout)
        collateral_outpoint = {"txid": collateral_txid, "vout": collateral_vout}
        unfunded_address = node.getnewaddress()

        # A failing call releases only a lock that it acquired itself.
        mn.register(node, submit=False, fundsAddr=unfunded_address,
                    expected_assert_code=-32603, expected_assert_msg="No funds at specified address")
        assert collateral_outpoint not in node.listlockunspent()

        command = "register_prepare_legacy" if mn.legacy else "register_prepare"
        prepare_args = [collateral_txid, collateral_vout, f'127.0.0.1:{mn.nodePort}',
                        mn.ownerAddr, mn.pubKeyOperator, mn.votingAddr, 0,
                        mn.rewards_address, unfunded_address]
        assert_raises_rpc_error(-32603, "No funds at specified address", node.protx, command, *prepare_args)
        assert collateral_outpoint not in node.listlockunspent()

        node.lockunspent(False, [collateral_outpoint])
        mn.register(node, submit=False, fundsAddr=unfunded_address,
                    expected_assert_code=-32603, expected_assert_msg="No funds at specified address")
        assert collateral_outpoint in node.listlockunspent()
        assert_raises_rpc_error(-32603, "No funds at specified address", node.protx, command, *prepare_args)
        assert collateral_outpoint in node.listlockunspent()
        node.lockunspent(True, [collateral_outpoint])

    def test_protx_register_external(self):
        node = self.nodes[0]
        self.log.info("Test protx register_prepare + signmessage + register_submit (external collateral)")
        mn = self.prepare_unstarted_mn(len(self.nodes) + 3)
        collateral_txid = node.sendtoaddress(mn.collateral_address, mn.get_collateral_value())
        node.sendtoaddress(mn.fundsAddr, 0.001)
        self.bump_mocktime(1)
        self.generate(node, 1)
        collateral_vout = mn.get_collateral_vout(node, collateral_txid)
        mn.set_params(collateral_txid=collateral_txid, collateral_vout=collateral_vout)

        command = "register_prepare_legacy" if mn.legacy else "register_prepare"
        prepared = node.protx(command, collateral_txid, collateral_vout, f'127.0.0.1:{mn.nodePort}',
                              mn.ownerAddr, mn.pubKeyOperator, mn.votingAddr, 0, mn.rewards_address, mn.fundsAddr)
        assert_equal(prepared["collateralAddress"], mn.collateral_address)
        collateral_outpoint = {"txid": collateral_txid, "vout": collateral_vout}
        assert collateral_outpoint in node.listlockunspent()
        signature = node.signmessage(prepared["collateralAddress"], prepared["signMessage"])

        assert_raises_rpc_error(-1, "bad-protx-sig", node.protx, "register_submit", prepared["tx"], "AA==")
        assert collateral_outpoint in node.listlockunspent()
        node.createwallet("empty_signing_wallet")
        empty_wallet = node.get_wallet_rpc("empty_signing_wallet")
        assert_raises_rpc_error(-4, "transaction inputs could not be fully signed", empty_wallet.protx,
                                "register_submit", prepared["tx"], signature)
        node.unloadwallet("empty_signing_wallet")
        protx_hash = node.protx("register_submit", prepared["tx"], signature)
        assert collateral_outpoint in node.listlockunspent()
        mn.set_params(proTxHash=protx_hash)
        self.confirm_tx(protx_hash)
        assert protx_hash in node.protx("list", "registered")
        assert_equal(node.protx("info", protx_hash)["collateralHash"], collateral_txid)
        assert "%s-%d" % (collateral_txid, collateral_vout) in node.masternode("list")

    def test_protx_update_service(self, mn: MasternodeInfo):
        node = self.nodes[0]
        self.log.info("Test protx update_service")
        node.sendtoaddress(mn.fundsAddr, 0.001)
        self.bump_mocktime(1)
        self.generate(node, 1)
        new_address = f'127.0.0.2:{mn.nodePort}'
        txid = mn.update_service(node, submit=True, addrs_core_p2p=[new_address])
        assert txid is not None
        self.confirm_tx(txid)
        assert_equal(node.protx("info", mn.proTxHash)["state"]["addresses"]["core_p2p"][0], new_address)

    def test_protx_update_registrar(self, mn: MasternodeInfo):
        node = self.nodes[0]
        self.log.info("Test protx update_registrar (owner key wallet signing)")
        node.sendtoaddress(mn.fundsAddr, 0.001)
        self.bump_mocktime(1)
        self.generate(node, 1)
        old_state = node.protx("info", mn.proTxHash)["state"]
        new_voting_address = node.getnewaddress()
        assert old_state["votingAddress"] != new_voting_address
        txid = mn.update_registrar(node, submit=True, votingAddr=new_voting_address, fundsAddr=mn.fundsAddr)
        assert txid is not None
        self.confirm_tx(txid)
        new_state = node.protx("info", mn.proTxHash)["state"]
        assert_equal(new_state["votingAddress"], new_voting_address)
        assert_equal(new_state["payoutAddress"], old_state["payoutAddress"])


if __name__ == '__main__':
    WalletDashRPCsTest().main()
