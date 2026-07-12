#!/usr/bin/env python3
# Copyright (c) 2024-2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""
Test per-peer governance inv request tracking.

After #7442, governance object/vote INV authorization no longer uses a
governance-owned m_requested_hash_time cache. Pending requests live in the
net-layer per-peer tracker (m_object_announced / m_object_in_flight):

  - a new INV is queued via RequestObject and fetched with getdata
  - a duplicate INV from the same peer is not re-queued while tracked
  - governance UpdateCachesAndClean does not clear that per-peer state
  - after the in-flight request expires, the same INV can be requested again
"""

from test_framework.messages import (
    CInv,
    msg_inv,
    MSG_GOVERNANCE_OBJECT,
    MSG_GOVERNANCE_OBJECT_VOTE,
)
from test_framework.p2p import (
    P2PInterface,
    p2p_lock,
)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    force_finish_mnsync,
)

# Constants from src/net_processing.cpp (GetObjectInterval default + expiry).
GETDATA_TX_INTERVAL = 60
TX_EXPIRY_INTERVAL = GETDATA_TX_INTERVAL * 10
# Expiry is scanned when m_check_expiry_timer elapses, which is scheduled at
# roughly TX_EXPIRY_INTERVAL/2 + U[0, TX_EXPIRY_INTERVAL] after each check.
WAIT_FOR_INFLIGHT_EXPIRY = TX_EXPIRY_INTERVAL + TX_EXPIRY_INTERVAL // 2
# NetGovernance::Schedule periodic CheckAndRemove / UpdateCachesAndClean.
DATA_CLEANUP_TIME = 5 * 60

INV_COMMAND = {
    MSG_GOVERNANCE_OBJECT: "govobj",
    MSG_GOVERNANCE_OBJECT_VOTE: "govobjvote",
}


class GovernanceInvPeer(P2PInterface):
    def __init__(self):
        super().__init__()
        self.gov_getdata_count = 0
        self.gov_getdata_hashes = []

    def on_getdata(self, message):
        for inv in message.inv:
            if inv.type in (MSG_GOVERNANCE_OBJECT, MSG_GOVERNANCE_OBJECT_VOTE):
                self.gov_getdata_count += 1
                self.gov_getdata_hashes.append(inv.hash)


class GovernanceInvsTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1

    def run_test(self):
        # Each case advances mocktime by ~15 minutes for in-flight expiry. Restart
        # between cases so mnsync / scheduler state from one case cannot poison the
        # next (ConfirmInventoryRequest short-circuits when !IsBlockchainSynced).
        cases = [
            (CInv(MSG_GOVERNANCE_OBJECT, 1), "object"),
            (CInv(MSG_GOVERNANCE_OBJECT_VOTE, 2), "vote"),
        ]
        for i, (inv, name) in enumerate(cases):
            if i > 0:
                self.stop_nodes()
                self.start_nodes()
            self.test_request_tracking(inv, name)

    def _request_log(self, inv):
        # Matches PeerManagerImpl::RequestObject debug line:
        #   RequestObject -- inv=(govobj <hash>), ...
        return f"RequestObject -- inv=({INV_COMMAND[inv.type]} {inv.hash:064x})"

    def test_request_tracking(self, inv, name):
        node = self.nodes[0]
        force_finish_mnsync(node)
        assert node.mnsync("status")["IsBlockchainSynced"]
        peer = node.add_p2p_connection(GovernanceInvPeer())
        inv_msg = msg_inv([inv])
        request_msg = self._request_log(inv)

        self.log.info(f"Send dummy governance {name} inv and make sure it is queued for fetch")
        with node.assert_debug_log([request_msg]):
            peer.send_and_ping(inv_msg)
        # Non-tx objects use process_time = now (no inbound delay), so getdata
        # is issued on the next SendMessages pass.
        peer.wait_until(lambda: peer.gov_getdata_count >= 1, timeout=10)
        with p2p_lock:
            assert_equal(peer.gov_getdata_hashes[-1], inv.hash)
            first_count = peer.gov_getdata_count

        self.log.info(
            f"Send dummy governance {name} inv again and make sure it is not re-queued "
            "while the per-peer announcement is live"
        )
        with node.assert_debug_log([], [request_msg]):
            peer.send_and_ping(inv_msg)
        with p2p_lock:
            assert_equal(peer.gov_getdata_count, first_count)

        self.log.info(
            "Force governance UpdateCachesAndClean and confirm the net-layer "
            "tracker still suppresses the duplicate"
        )
        with node.assert_debug_log(["UpdateCachesAndClean"]):
            node.mockscheduler(DATA_CLEANUP_TIME + 1)
        with node.assert_debug_log([], [request_msg]):
            peer.send_and_ping(inv_msg)
        with p2p_lock:
            assert_equal(peer.gov_getdata_count, first_count)

        self.log.info(
            f"Expire the in-flight governance {name} request and make sure the "
            "same inv is accepted again"
        )
        # Advance far enough for both the in-flight entry and the expiry-scan
        # timer, then poke SendMessages so announced/in_flight are cleared.
        self.bump_mocktime(WAIT_FOR_INFLIGHT_EXPIRY, nodes=[node])
        # Large mocktime jumps can reset mnsync (no tip update for
        # MASTERNODE_SYNC_RESET_SECONDS); re-finish so ConfirmInventoryRequest
        # does not short-circuit as "already have".
        force_finish_mnsync(node)
        assert node.mnsync("status")["IsBlockchainSynced"]
        with node.assert_debug_log([f"timeout of inflight object {INV_COMMAND[inv.type]} {inv.hash:064x}"]):
            peer.sync_with_ping()
        with node.assert_debug_log([request_msg]):
            peer.send_and_ping(inv_msg)
        peer.wait_until(lambda: peer.gov_getdata_count >= first_count + 1, timeout=10)
        with p2p_lock:
            assert_equal(peer.gov_getdata_hashes[-1], inv.hash)

        node.disconnect_p2ps()


if __name__ == "__main__":
    GovernanceInvsTest().main()
