#!/usr/bin/env python3
# Copyright (c) 2015-2025 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

'''
feature_llmq_signing.py

Checks LLMQs signing sessions

'''

from test_framework.messages import uint256_to_string
from test_framework.test_framework import (
    DashTestFramework,
    MasternodeInfo,
)
from test_framework.util import assert_equal, assert_raises_rpc_error, force_finish_mnsync


q_type=100
class LLMQSigningTest(DashTestFramework):
    def set_test_params(self):
        self.set_dash_test_params(6, 5)
        self.set_dash_llmq_test_params(5, 3)

    def add_options(self, parser):
        self.add_wallet_options(parser)
        parser.add_argument("--spork21", dest="spork21", default=False, action="store_true",
                            help="Test with spork21 enabled")

    def run_test(self):

        self.nodes[0].sporkupdate("SPORK_17_QUORUM_DKG_ENABLED", 0)
        if self.options.spork21:
            self.nodes[0].sporkupdate("SPORK_21_QUORUM_ALL_CONNECTED", 0)
        self.wait_for_sporks_same()

        self.mine_quorum()

        if self.options.spork21:
            assert self.mninfo[0].get_node(self).getconnectioncount() == self.llmq_size

        id = "0000000000000000000000000000000000000000000000000000000000000001"
        msgHash = "0000000000000000000000000000000000000000000000000000000000000002"
        msgHashConflict = "0000000000000000000000000000000000000000000000000000000000000003"

        def check_sigs(hasrecsigs, isconflicting1, isconflicting2):
            for mn in self.mninfo: # type: MasternodeInfo
                if mn.get_node(self).quorum("hasrecsig", q_type, id, msgHash) != hasrecsigs:
                    return False
                if mn.get_node(self).quorum("isconflicting", q_type, id, msgHash) != isconflicting1:
                    return False
                if mn.get_node(self).quorum("isconflicting", q_type, id, msgHashConflict) != isconflicting2:
                    return False
            return True

        def wait_for_sigs(hasrecsigs, isconflicting1, isconflicting2, timeout):
            self.wait_until(lambda: check_sigs(hasrecsigs, isconflicting1, isconflicting2), timeout = timeout)

        def wait_for_sigs_bumping(hasrecsigs, isconflicting1, isconflicting2, timeout, max_mocktime_advance=30):
            """Like wait_for_sigs but periodically bumps mocktime so that
            concentrated-send retries (which use mocktime-based scheduling)
            can fire.  Limits total mocktime advance to max_mocktime_advance
            seconds to avoid triggering SESSION_NEW_SHARES_TIMEOUT (60s)
            cleanup of in-progress sig share sessions."""
            import time as _time
            deadline = _time.time() + timeout * self.options.timeout_factor
            total_advanced = 0
            while _time.time() < deadline:
                if check_sigs(hasrecsigs, isconflicting1, isconflicting2):
                    return
                if total_advanced < max_mocktime_advance:
                    self.bump_mocktime(1, update_schedulers=False)
                    total_advanced += 1
                _time.sleep(0.5)
            # Final check with assertion
            wait_for_sigs(hasrecsigs, isconflicting1, isconflicting2, 1)

        def assert_sigs_nochange(hasrecsigs, isconflicting1, isconflicting2, timeout):
            assert not self.wait_until(lambda: not check_sigs(hasrecsigs, isconflicting1, isconflicting2), timeout = timeout, do_assert = False)

        # Initial state
        wait_for_sigs(False, False, False, 1)

        # Sign first share without any optional parameter, should not result in recovered sig
        self.mninfo[0].get_node(self).quorum("sign", q_type, id, msgHash)
        assert_sigs_nochange(False, False, False, 3)
        # Sign second share and test optional quorumHash parameter, should not result in recovered sig
        # 1. Providing an invalid quorum hash should fail and cause no changes for sigs
        assert not self.mninfo[1].get_node(self).quorum("sign", q_type, id, msgHash, msgHash)
        assert_sigs_nochange(False, False, False, 3)
        # 2. Providing a valid quorum hash should succeed and cause no changes for sigss
        quorumHash = self.mninfo[1].get_node(self).quorum("selectquorum", q_type, id)["quorumHash"]
        assert self.mninfo[1].get_node(self).quorum("sign", q_type, id, msgHash, quorumHash)
        assert_sigs_nochange(False, False, False, 3)
        # Sign third share and test optional submit parameter if spork21 is enabled, should result in recovered sig
        # and conflict for msgHashConflict
        if self.options.spork21:
            # 1. Providing an invalid quorum hash and set submit=false, should throw an error
            assert_raises_rpc_error(-8, 'quorum not found', self.mninfo[2].get_node(self).quorum, "sign", q_type, id, msgHash, id, False)
            # 2. Providing a valid quorum hash and set submit=false, should return a valid sigShare object
            sig_share_rpc_1 = self.mninfo[2].get_node(self).quorum("sign", q_type, id, msgHash, quorumHash, False)
            sig_share_rpc_2 = self.mninfo[2].get_node(self).quorum("sign", q_type, id, msgHash, "", False)
            assert_equal(sig_share_rpc_1, sig_share_rpc_2)
            assert_sigs_nochange(False, False, False, 3)
            # 3. Verify the returned sigShare has the expected fields
            for field in ["llmqType", "quorumHash", "quorumMember", "id", "msgHash", "signHash", "signature"]:
                assert field in sig_share_rpc_1, f"Missing field {field} in sigShare"
            # 4. Now submit the third share normally so that recovery can proceed
            # reliably via the concentrated send path (the submit=false RPC was
            # already validated above)
            self.mninfo[2].get_node(self).quorum("sign", q_type, id, msgHash)
        else:
            # If spork21 is not enabled just sign regularly
            self.mninfo[2].get_node(self).quorum("sign", q_type, id, msgHash)

        if self.options.spork21:
            # Concentrated sends use mocktime-based retry scheduling. With
            # frozen mocktime each MN's share is sent only once (attempt 0).
            # Advance mocktime so retries to additional recovery members can
            # fire, making recovery robust against any single delivery failure.
            wait_for_sigs_bumping(True, False, True, 30)
        else:
            wait_for_sigs(True, False, True, 15)

        # Test `quorum verify` rpc
        node = self.mninfo[0].get_node(self)
        recsig = node.quorum("getrecsig", q_type, id, msgHash)
        # Find quorum automatically
        height = node.getblockcount()
        height_bad = node.getblockheader(recsig["quorumHash"])["height"]
        hash_bad = node.getblockhash(0)
        assert node.quorum("verify", q_type, id, msgHash, recsig["sig"])
        assert node.quorum("verify", q_type, id, msgHash, recsig["sig"], "", height)
        assert not node.quorum("verify", q_type, id, msgHashConflict, recsig["sig"])
        assert not node.quorum("verify", q_type, id, msgHash, recsig["sig"], "", height_bad)
        # Use specific quorum
        assert node.quorum("verify", q_type, id, msgHash, recsig["sig"], recsig["quorumHash"])
        assert not node.quorum("verify", q_type, id, msgHashConflict, recsig["sig"], recsig["quorumHash"])
        assert_raises_rpc_error(-8, "quorum not found", node.quorum, "verify", q_type, id, msgHash, recsig["sig"], hash_bad)

        # Mine one more quorum, so that we have 2 active ones, nothing should change
        self.mine_quorum()
        assert_sigs_nochange(True, False, True, 3)

        # Create a recovered sig for the oldest quorum i.e. the active quorum which will be moved
        # out of the active set when a new quorum appears
        request_id = 2
        oldest_quorum_hash = node.quorum("list")["llmq_test"][-1]
        # Search for a request id which selects the last active quorum
        while True:
            selected_hash = node.quorum('selectquorum', q_type, uint256_to_string(request_id))["quorumHash"]
            if selected_hash == oldest_quorum_hash:
                break
            else:
                request_id += 1
        # Produce the recovered signature
        id = uint256_to_string(request_id)
        for mn in self.mninfo: # type: MasternodeInfo
            mn.get_node(self).quorum("sign", q_type, id, msgHash)
        # And mine a quorum to move the quorum which signed out of the active set
        self.mine_quorum()
        # Verify the recovered sig. This triggers the "signHeight + dkgInterval" verification
        recsig = node.quorum("getrecsig", q_type, id, msgHash)
        assert node.quorum("verify", q_type, id, msgHash, recsig["sig"], "", node.getblockcount())

        recsig_time = self.mocktime

        # Mine 2 more quorums, so that the one used for the the recovered sig should become inactive, nothing should change
        self.mine_quorum()
        self.mine_quorum()
        # Wait for recovered sig to propagate to all nodes (may be delayed under UBSAN/sanitizer load)
        if self.options.spork21:
            wait_for_sigs_bumping(True, False, True, 30)
        else:
            wait_for_sigs(True, False, True, 15)
        assert_sigs_nochange(True, False, True, 3)

        # fast forward until 0.5 days before cleanup is expected, recovered sig should still be valid
        self.bump_mocktime(recsig_time + int(60 * 60 * 24 * 6.5) - self.mocktime, update_schedulers=False)
        # Cleanup starts every 5 seconds
        wait_for_sigs(True, False, True, 15)
        # fast forward 1 day, recovered sig should not be valid anymore
        self.bump_mocktime(int(60 * 60 * 24 * 1), update_schedulers=False)
        # Cleanup starts every 5 seconds
        wait_for_sigs(False, False, False, 15)

        for i in range(2):
            self.mninfo[i].get_node(self).quorum("sign", q_type, id, msgHashConflict)
        for i in range(2, 5):
            self.mninfo[i].get_node(self).quorum("sign", q_type, id, msgHash)
        if self.options.spork21:
            wait_for_sigs_bumping(True, False, True, 30)
        else:
            wait_for_sigs(True, False, True, 15)

        if self.options.spork21:
            id = uint256_to_string(request_id + 1)

            # Isolate the node that is responsible for the recovery of a signature and assert that recovery fails
            q = self.nodes[0].quorum('selectquorum', q_type, id)
            mn: MasternodeInfo = self.get_mninfo(q['recoveryMembers'][0])
            mn.get_node(self).setnetworkactive(False)
            self.wait_until(lambda: mn.get_node(self).getconnectioncount() == 0)
            for i in range(4):
                self.mninfo[i].get_node(self).quorum("sign", q_type, id, msgHash)
            assert_sigs_nochange(False, False, False, 3)
            # Need to re-connect so that it later gets the recovered sig
            mn.get_node(self).setnetworkactive(True)
            self.connect_nodes(mn.nodeIdx, 0)
            force_finish_mnsync(mn.get_node(self))
            # Make sure intra-quorum connections were also restored
            self.bump_mocktime(1)  # need this to bypass quorum connection retry timeout
            self.wait_until(lambda: mn.get_node(self).getconnectioncount() == self.llmq_size, timeout=10)
            mn.get_node(self).ping()
            self.wait_until(lambda: all('pingwait' not in peer for peer in mn.get_node(self).getpeerinfo()))
            # Advance mocktime past the daemon's 5-second signing session
            # cleanup cadence so recovery responsibility rotates to the next
            # member. Use 10s to guarantee at least one full cycle completes.
            self.bump_mocktime(10)
            wait_for_sigs_bumping(True, False, True, 15)

if __name__ == '__main__':
    LLMQSigningTest().main()
