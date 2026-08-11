#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test recovery of a governance vote that arrives before its parent object.

A vote naming an object we do not have is held as an orphan, and the missing
parent is fetched through the object request tracker from the peer that sent
the vote: that peer demonstrably has the object but may never announce it.
The node used to broadcast MNGOVERNANCESYNC for every orphan to every peer
instead; that path is gone, so the tracker getdata to the vote's sender is
what keeps the vote from being stranded until ordinary governance sync.
"""

from test_framework.authproxy import JSONRPCException
from test_framework.governance import prepare_object
from test_framework.messages import (
    CInv,
    MSG_GOVERNANCE_OBJECT,
    MSG_GOVERNANCE_OBJECT_VOTE,
    msg_getdata,
    msg_inv,
)
from test_framework.p2p import P2PInterface, p2p_lock
from test_framework.test_framework import DashTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error


class GovernanceOrphanVoteTest(DashTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser)

    def set_test_params(self):
        self.set_dash_test_params(2, 1)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        n0, n1 = self.nodes

        # Without LLMQs the collateral tx can never receive an InstantSend lock, and the miner
        # refuses to include an unlocked tx while InstantSend is enabled.
        n0.sporkupdate("SPORK_2_INSTANTSEND_ENABLED", 4070908800)
        self.wait_for_sporks_same()

        self.log.info("Create a funded, voted-on proposal while node1 cannot see it")
        self.disconnect_nodes(0, 1)
        first_new_height = n0.getblockcount() + 1

        proposal_time = self.mocktime
        proposal = prepare_object(n0, 1, "%064x" % 0, proposal_time, 1, "orphan-vote-test", 1, n0.getnewaddress())
        self.bump_mocktime(6)
        self.generate(n0, 6, sync_fun=self.no_op)
        # The collateral is looked up through the txindex, which processes the new blocks
        # asynchronously; drain the validation queue so submit cannot race it.
        n0.syncwithvalidationinterfacequeue()
        proposal_hash = n0.gobject("submit", "0", 1, proposal_time, proposal["hex"], proposal["collateralHash"])
        n0.gobject("vote-many", proposal_hash, "funding", "yes")
        votes = n0.gobject("getcurrentvotes", proposal_hash)
        assert_equal(len(votes), 1)
        vote_hash = next(iter(votes))

        self.log.info("Give node1 the blocks, so the collateral validates, but not the governance payloads")
        for height in range(first_new_height, n0.getblockcount() + 1):
            n1.submitblock(n0.getblock(n0.getblockhash(height), 0))
        assert_equal(n1.getblockcount(), n0.getblockcount())
        assert_raises_rpc_error(-8, "Unknown governance object", n1.gobject, "get", proposal_hash)

        self.log.info("Capture the raw signed object and vote from node0")
        obj_inv = CInv(MSG_GOVERNANCE_OBJECT, int(proposal_hash, 16))
        vote_inv = CInv(MSG_GOVERNANCE_OBJECT_VOTE, int(vote_hash, 16))
        source = n0.add_p2p_connection(P2PInterface())
        source.send_message(msg_getdata([obj_inv, vote_inv]))
        source.wait_until(lambda: "govobj" in source.last_message and "govobjvote" in source.last_message)
        with p2p_lock:
            obj_msg = source.last_message["govobj"]
            vote_msg = source.last_message["govobjvote"]
        assert_equal(vote_msg.vote.nParentHash, int(proposal_hash, 16))

        self.log.info("An orphan vote makes node1 request the missing parent from the vote's sender")
        peer = n1.add_p2p_connection(P2PInterface())
        peer.send_message(msg_inv([vote_inv]))
        peer.wait_for_getdata([vote_inv.hash])
        peer.send_message(vote_msg)
        # The parent object was never announced to node1, so this getdata can only come from the
        # vote sender being registered with the object request tracker.
        peer.wait_for_getdata([obj_inv.hash])
        with p2p_lock:
            assert "govsync" not in peer.last_message

        self.log.info("Serving the object replays the orphan vote")
        peer.send_message(obj_msg)

        def vote_replayed():
            try:
                return len(n1.gobject("getcurrentvotes", proposal_hash)) == 1
            except JSONRPCException:  # the object itself has not been processed yet
                return False
        self.wait_until(vote_replayed)
        assert_equal(n1.gobject("get", proposal_hash)["FundingResult"]["YesCount"], 1)

        self.connect_nodes(0, 1)


if __name__ == '__main__':
    GovernanceOrphanVoteTest().main()
