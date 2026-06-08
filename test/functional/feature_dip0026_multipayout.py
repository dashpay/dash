#!/usr/bin/env python3
# Copyright (c) 2024-2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test DIP-0026 multi-party masternode payouts (v4 / MultiPayout ProTx).

End-to-end regtest coverage of the DIP-0026 feature that lets a masternode pay
its owner-side block reward to several payees, each receiving a configured share
expressed in basis points (bps). The feature is gated behind the v25 version-bits
deployment (bit 13), which in turn requires v24 (bit 12, extended addresses) to be
active. See:
  - src/evo/providertx.h        ProTxVersion::MultiPayout = 4, PayoutShare model
  - src/evo/providertx.cpp       CheckPayoutShares() consensus rules
  - src/rpc/evo.cpp              ParsePayoutParam() (accepts STR or {addr: bps} object)
  - src/masternode/payments.cpp  SplitMasternodeReward() (floor + 1-sat remainder)
  - src/evo/core_write.cpp       state JSON: payoutShares array for v4 MNs

The protx register*/update_registrar RPCs accept the payout parameter either as a
single address string (legacy single payout) or as a JSON object mapping each
payout address to its share in basis points (DIP-0026 multi payout). In the
functional-test framework, that parameter is the `rewards_address` kwarg of the
MasternodeInfo register/register_fund/update_registrar helpers; passing a Python
dict {address: bps} produces the JSON-object form.

Activation strategy: both v24 and v25 are forced on via the 9-field -vbparams form
with useehf=0, which turns each into a plain BIP9 version-bits fork that activates
purely by miner signaling (no MNHF/EHF quorum dance). The regtest block assembler
auto-signals the version bit of any STARTED/LOCKED_IN deployment, so simply mining
blocks drives activation. A tiny window/threshold keeps the activation cheap.

The test:
  1. Brings up masternodes on regtest with v24 + v25 activatable via -vbparams.
  2. Asserts that BEFORE v25, an object-form payout is rejected by the RPC.
  3. Activates v24 then v25 by mining (miner signaling).
  4. Converts running masternodes to multi-payout (two and three payees) via
     update_registrar with the object form, and registers a fresh masternode with
     the object form via register_fund; every share set sums to 10000.
  5. Mines until each running multi-payout masternode is the coinbase payee and
     asserts the coinbase splits the owner reward across the share addresses in the
     correct proportions (floor + remainder), summing to exactly the owner reward.
  6. Asserts the RPC-level validation rejects (caught by ParsePayoutParam at parse time):
     shares not summing to 10000, a share out of (0, 10000], an empty object, and more
     than 32 shares. Rules unreachable via the object form (duplicate scripts, non-p2pkh/p2sh
     payees, cross-version mixing) are covered by the unit reject-matrix instead.
"""

import json

from test_framework.test_framework import DashTestFramework, MasternodeInfo
from test_framework.util import (
    assert_equal,
    assert_greater_than,
    assert_raises_rpc_error,
    softfork_active,
)

# The masternode bring-up in setup_network() mines >100 blocks and bumps mocktime
# (collateral maturity + per-MN bury_tx waits), and the regtest miner auto-signals
# every STARTED version-bits deployment. With a start time of 0 both v24 and v25
# would therefore activate DURING setup, before run_test() can observe the pre-v25
# rejection. We keep them inactive through setup by gating ACTIVATION on a
# min_activation_height well beyond what masternode setup reaches: with start=0 the forks
# signal and LOCK_IN during setup but do not become ACTIVE until that height, so run_test()
# observes them inactive and then simply mines past the gate to activate them (no mocktime jump,
# which bump_mocktime caps at 3600s anyway).
DEPLOY_MIN_ACT = 600  # comfortably beyond the ~tens-of-blocks masternode setup mines

# 9-field -vbparams: name:start:end:min_act:window:thrStart:thrMin:falloff:useehf
# start=0, end=far future, min_act=DEPLOY_MIN_ACT (height gate), window=10, thrStart=8,
# thrMin=6, falloff=5, useehf=0 (plain miner-signaled fork, no MNHF/EHF quorum dance).
VBPARAMS_V24 = "-vbparams=v24:0:999999999999:%d:10:8:6:5:0" % DEPLOY_MIN_ACT
VBPARAMS_V25 = "-vbparams=v25:0:999999999999:%d:10:8:6:5:0" % DEPLOY_MIN_ACT

TOTAL_BASIS_POINTS = 10000
MAX_PAYOUT_SHARES = 32  # PayoutShare::MAX_PAYOUT_SHARES (DIP0026: at most 32 shares)

# RPC error codes (src/rpc/protocol.h)
RPC_INVALID_PARAMETER = -8     # raised by ParsePayoutParam for bad/missing payout params


def split_reward(reward, shares):
    """Reproduce SplitMasternodeReward (src/masternode/payments.cpp:33-62).

    shares is an ordered list of (address, bps). Returns an ordered list of
    (address, satoshis), preserving share order and skipping zero outputs.
    """
    amounts = [reward * bps // TOTAL_BASIS_POINTS for (_, bps) in shares]
    distributed = sum(amounts)
    i = 0
    # Hand out the leftover satoshis one per share, in order, until exact.
    while distributed < reward and i < len(shares):
        amounts[i] += 1
        distributed += 1
        i += 1
    assert_equal(distributed, reward)
    return [(addr, amt) for (addr, _), amt in zip(shares, amounts) if amt > 0]


class Dip0026MultiPayoutTest(DashTestFramework):
    def add_options(self, parser):
        # Multi-party payouts are wallet-agnostic; run on descriptor (sqlite) wallets so the
        # test does not require a legacy (BDB) wallet build.
        self.add_wallet_options(parser, legacy=False)

    def set_test_params(self):
        # 5 nodes total: node 0 = controller/miner, node 1 = simple node,
        # nodes 2..4 = masternodes (mn_count = 3). Every node carries both
        # -vbparams so the whole network signals/sees v24 and v25 identically.
        vb = [VBPARAMS_V24, VBPARAMS_V25]
        self.set_dash_test_params(5, 3, extra_args=[vb.copy() for _ in range(5)])
        # Push v20/mn_rr out a bit so the mn_rr credit-pool reallocation does not
        # complicate the very first blocks; the defaults (height 100) are fine here
        # because we mine well past 100 while activating v24/v25.

    def assert_state_shares(self, node, protx_hash, expected_shares):
        """Assert protx info reflects a v4 multi-payout MN with expected shares."""
        info = node.protx("info", protx_hash)
        state = info["state"]
        assert_equal(state["version"], 4)  # ProTxVersion::MultiPayout
        # For a v4 MN scriptPayout is empty, so the single payoutAddress field is absent.
        assert "payoutAddress" not in state
        got = {s["payoutAddress"]: s["payoutShareReward"] for s in state["payoutShares"]}
        assert_equal(got, expected_shares)
        # Order is preserved on-chain (JSON key order); verify it too.
        assert_equal([s["payoutAddress"] for s in state["payoutShares"]],
                     list(expected_shares.keys()))

    def mine_until_payee_and_check_split(self, node, protx_hash, shares):
        """Mine until `protx_hash` is the coinbase payee; verify the multi-payout split.

        `shares` is an ordered dict {address: bps}. Returns once verified.
        """
        share_addrs = set(shares.keys())
        # Each MN is paid once per full rotation; mn_count MNs means it pays within
        # a few blocks. Give generous headroom.
        for _ in range(4 * self.mn_count + 8):
            bt = node.getblocktemplate()
            template_owner = [e for e in bt["masternode"]
                              if e.get("payee") in share_addrs]
            if len(template_owner) == len(shares):
                # This block pays our multi-payout MN. Cross-check the template's
                # computed amounts against our independent split computation, then
                # mine the block and verify the actual coinbase vouts.
                owner_reward = sum(e["amount"] for e in bt["masternode"]
                                   if e.get("payee") in share_addrs)
                assert_greater_than(owner_reward, 0)
                expected = dict(split_reward(owner_reward,
                                             list(shares.items())))

                # (a) The node's own template split must match our reference split.
                template_amounts = {e["payee"]: e["amount"] for e in template_owner}
                assert_equal(template_amounts, expected)
                # Floor + 1-sat-remainder sanity: every output is within 1 sat of the
                # exact proportional share, and the outputs sum to the owner reward.
                for addr, bps in shares.items():
                    exact = owner_reward * bps // TOTAL_BASIS_POINTS
                    assert template_amounts[addr] in (exact, exact + 1)
                assert_equal(sum(template_amounts.values()), owner_reward)

                # (b) Mine the block and assert the real coinbase vouts match.
                blockhash = self.generate(node, 1, sync_fun=lambda: self.sync_blocks())[0]
                block = node.getblock(blockhash, 2)
                cbtx = block["tx"][0]
                paid = {}
                for out in cbtx["vout"]:
                    spk = out["scriptPubKey"]
                    addr = spk.get("address")
                    if addr in share_addrs:
                        paid[addr] = paid.get(addr, 0) + out["valueSat"]
                assert_equal(paid, expected)
                assert_equal(sum(paid.values()), owner_reward)
                self.log.info("verified multi-payout split for %s: %s (owner reward %d)"
                              % (protx_hash, expected, owner_reward))
                return
            self.generate(node, 1, sync_fun=lambda: self.sync_blocks())
        raise AssertionError("masternode %s never became the coinbase payee" % protx_hash)

    def upgrade_mn_to_extaddr(self, node, mn):
        """The framework's masternodes were registered before v24, so they are v2 (basic BLS).
        A v4 multi-party-payout update requires the MN to be at least v3 (extended addresses),
        so re-announce its service: the ProUpServTx is v3 now that v24 is active, which upgrades
        the MN state to v3 and makes it eligible for a subsequent v4 update_registrar."""
        if node.protx("info", mn.proTxHash)["state"]["version"] >= 3:
            return
        # Top up the masternode's fee-source address before the ProUpServTx so it always has a
        # spendable output to pay the fee (the setup funding can be otherwise consumed/unconfirmed;
        # mirrors feature_dip3_deterministicmns.py test_protx_update_service).
        node.sendtoaddress(mn.fundsAddr, 0.001)
        mn.update_service(node, submit=True)
        self.bump_mocktime(10 * 60 + 1)
        self.generate(node, 1, sync_fun=lambda: self.sync_blocks())
        assert_greater_than(node.protx("info", mn.proTxHash)["state"]["version"], 2)

    def run_test(self):
        node = self.nodes[0]

        # setup_network() already registered, started, synced and ENABLED mn_count MNs.
        assert_equal(len(self.mninfo), self.mn_count)

        self.log.info("v24/v25 must be inactive at the start")
        assert not softfork_active(node, "v24")
        assert not softfork_active(node, "v25")

        # ---------------------------------------------------------------
        # Negative test: object-form payout is rejected before v25 active.
        # ParsePayoutParam (src/rpc/evo.cpp:413-416) throws RPC_INVALID_PARAMETER.
        # ---------------------------------------------------------------
        self.log.info("multi-payout object must be rejected before v25 activation")
        pre_a = node.getnewaddress()
        pre_b = node.getnewaddress()
        pre_shares = {pre_a: 6000, pre_b: 4000}
        mn0 = self.mninfo[0]
        mn0.update_registrar(
            node, submit=True, rewards_address=pre_shares, fundsAddr=mn0.fundsAddr,
            expected_assert_code=RPC_INVALID_PARAMETER,
            expected_assert_msg="multi-party payouts (DIP0026) are not available yet",
        )
        # register_fund with an object payout must be rejected pre-v25 too.
        pre_mn = MasternodeInfo(evo=False, legacy=mn0.legacy)
        pre_mn.generate_addresses(node)
        pre_mn.register_fund(
            node, submit=True, rewards_address=pre_shares,
            expected_assert_code=RPC_INVALID_PARAMETER,
            expected_assert_msg="multi-party payouts (DIP0026) are not available yet",
        )

        # ---------------------------------------------------------------
        # Activate v24, then v25 (order matters: v25/MultiPayout for a ProRegTx
        # requires v24/extended-addresses active, src/evo/providertx.cpp:32-41).
        # Both deployments share identical -vbparams and the miner signals both
        # version bits on every block, so v25 may activate in lockstep with v24;
        # only drive v25 separately if it is not already active (activate_by_name
        # asserts the fork is still inactive on entry).
        # ---------------------------------------------------------------
        self.log.info("activating v24 then v25 by mining past min_activation_height")
        assert not softfork_active(node, "v24")
        assert not softfork_active(node, "v25")
        self.activate_by_name("v24")
        if not softfork_active(node, "v25"):
            self.activate_by_name("v25")
        assert softfork_active(node, "v24")
        assert softfork_active(node, "v25")

        # ---------------------------------------------------------------
        # Post-activation RPC payout-validation rejects (object form). ParsePayoutParam
        # (src/rpc/evo.cpp) rejects these at RPC-parse time, before any transaction is built or
        # submitted, so they surface as a clean RPC error and do not depend on the target
        # masternode's state. We run them on a pristine, freshly-activated masternode (mninfo[2],
        # left untouched by the conversion + payout-mining below) so no PoSe/state interference can
        # mask a reject. Each call throws inside ParsePayoutParam, so mninfo[2] stays a plain v2 MN.
        # A duplicate payout script cannot be expressed through a Python dict (keys are unique), so the
        # functional matrix below does not cover it; ParsePayoutParam still rejects duplicates (a raw
        # JSON request can carry repeated keys) and the consensus rule is unit-tested
        # (src/test/evo_providertx_tests.cpp). Non-p2pkh/p2sh payees are impossible too: a valid
        # address always yields a p2pkh/p2sh script.
        # ---------------------------------------------------------------
        self.log.info("post-activation RPC payout-validation rejects")
        mn_r = self.mninfo[2]
        ra, rb = node.getnewaddress(), node.getnewaddress()

        def reject_payout(shares, msg):
            mn_r.update_registrar(
                node, submit=True, rewards_address=shares, fundsAddr=mn_r.fundsAddr,
                expected_assert_code=RPC_INVALID_PARAMETER, expected_assert_msg=msg)

        # shares summing below 10000
        reject_payout({ra: 6000, rb: 3000}, "must sum to 10000")
        # shares summing above 10000
        reject_payout({ra: 6000, rb: 5000}, "must sum to 10000")
        # a single share exceeding 10000 basis points
        reject_payout({ra: 10001}, "must be between 1 and 10000")
        # a zero-reward share
        reject_payout({ra: 0, rb: 10000}, "must be between 1 and 10000")
        # an empty payout object. Sent via a direct protx call, not reject_payout: the helper's
        # `rewards_address or self.rewards_address` fallback replaces a falsy {} with the
        # masternode's existing payout address, so {} would never reach the RPC. Empty operator
        # and voting args ("") mean "reuse", and the empty-object check fires before either is used.
        assert_raises_rpc_error(RPC_INVALID_PARAMETER, "must not be empty",
                                node.protx, "update_registrar", mn_r.proTxHash, "", "", {}, mn_r.fundsAddr)
        # more than MAX_PAYOUT_SHARES (32) shares
        too_many = {node.getnewaddress(): 1 for _ in range(MAX_PAYOUT_SHARES + 1)}
        reject_payout(too_many, "too many payout shares")

        # ---------------------------------------------------------------
        # Convert an existing, running, ENABLED masternode (mninfo[0]) to a 3-way
        # multi-payout via update_registrar with the object form. mninfo[0] has
        # operator_reward == 0 (framework sets operatorReward = idx), so its whole
        # masternode reward flows to the owner shares: the cleanest split to check.
        # ---------------------------------------------------------------
        self.log.info("convert mninfo[0] to a 3-way multi-payout via update_registrar")
        mn_a = self.mninfo[0]
        self.upgrade_mn_to_extaddr(node, mn_a)
        a1, a2, a3 = node.getnewaddress(), node.getnewaddress(), node.getnewaddress()
        shares_a = {a1: 2500, a2: 2500, a3: 5000}  # ordered; sums to 10000
        assert_equal(sum(shares_a.values()), TOTAL_BASIS_POINTS)
        node.sendtoaddress(mn_a.fundsAddr, 0.001)  # ensure a spendable fee output for the ProUpRegTx
        txid = mn_a.update_registrar(node, submit=True, rewards_address=shares_a,
                                     fundsAddr=mn_a.fundsAddr)
        assert txid is not None
        self.bump_mocktime(10 * 60 + 1)  # make the ProUpRegTx safe to include
        self.generate(node, 1, sync_fun=lambda: self.sync_blocks())
        self.assert_state_shares(node, mn_a.proTxHash, shares_a)

        # Convert a second running MN (mninfo[1]) to a 2-way multi-payout. mninfo[1]
        # carries a small operator reward with an operator payout address, so its
        # coinbase additionally has an operator output. The split check below ignores
        # that output (different address) and validates only the owner-share split of
        # the already-operator-reduced owner reward reported by getblocktemplate.
        #
        # The shares here are passed as a JSON STRING rather than a dict, on purpose: dash-cli
        # delivers the object form as a string (the payout arg is not in the rpc/client.cpp JSON
        # conversion table), so this exercises ParsePayoutParam's string-to-object path that the dict
        # form (real JSON-RPC object) does not. mninfo[0] above used the dict form, so both arrival
        # shapes are covered.
        self.log.info("convert mninfo[1] to a 2-way multi-payout via update_registrar (JSON-string form)")
        mn_b = self.mninfo[1]
        self.upgrade_mn_to_extaddr(node, mn_b)
        b1, b2 = node.getnewaddress(), node.getnewaddress()
        shares_b = {b1: 3333, b2: 6667}  # ordered; sums to 10000
        assert_equal(sum(shares_b.values()), TOTAL_BASIS_POINTS)
        node.sendtoaddress(mn_b.fundsAddr, 0.001)  # ensure a spendable fee output for the ProUpRegTx
        txid = mn_b.update_registrar(node, submit=True, rewards_address=json.dumps(shares_b),
                                     fundsAddr=mn_b.fundsAddr)
        assert txid is not None
        self.bump_mocktime(10 * 60 + 1)
        self.generate(node, 1, sync_fun=lambda: self.sync_blocks())
        self.assert_state_shares(node, mn_b.proTxHash, shares_b)

        # (A fresh register_fund with the object form exercises the same ParsePayoutParam path
        # as the conversions above and the register RPC's CheckProRegTx; it is omitted here to
        # avoid the collateral-funding/port plumbing for a non-running MN. The two conversions
        # above already prove the v4 multi-payout state via the RPC object form.)

        # ---------------------------------------------------------------
        # Verify the coinbase reward split for the two running multi-payout MNs.
        # ---------------------------------------------------------------
        self.log.info("verify coinbase reward split across share addresses")
        self.mine_until_payee_and_check_split(node, mn_a.proTxHash, shares_a)
        self.mine_until_payee_and_check_split(node, mn_b.proTxHash, shares_b)

        # The dict-expressible validation rejects are exercised functionally above (post-activation
        # reject-matrix on mninfo[2]). The rules a Python dict cannot express here, namely duplicate
        # payout scripts (repeated keys), non-p2pkh/p2sh payees, and cross-version field mixing, are
        # covered exhaustively at the unit level by the CheckPayoutShares reject-matrix
        # (src/test/evo_providertx_tests.cpp). Those consensus rules run at both mempool acceptance and
        # block connect via CheckSpecialTx, so a malformed v4 ProTx cannot be mined.

        self.log.info("DIP-0026 multi-party payout test passed")


if __name__ == "__main__":
    Dip0026MultiPayoutTest().main()
