#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""
feature_llmq_dkg_intake.py

Adversarial P2P tests for DKG message-intake hardening:
  - pushed DKG messages (qcontrib/qcomplaint/qjustify/qpcommit) from a peer that is
    not MNAuth-verified are rejected before retention.
  - oversized DKG payloads are rejected (before deserialization / retention) even
    from a verified peer.
  - structural pre-validation: truncated, trailing, or parametrically out-of-bounds
    DKG payloads are rejected before retention even from a verified peer.
  - BLS objects are not materialized at intake: a structurally plausible payload with
    an invalid BLS encoding reaches the DKG worker and is rejected there.
  - the per-sender retention quota is keyed by the MNAuth-verified proTxHash, so
    reconnecting under a fresh NodeId does not refill it.
  - late, framing-valid messages stay bounded across distinct senders and are
    discarded without BLS materialization before the next round initializes.
  - a well-formed DKG message that the peer never announced and was never asked for
    is dropped before retention, even from a verified peer.

The node must not crash; rejected malformed messages must be scored where the
matching worker still processes them.
"""

from test_framework.messages import ser_compact_size, ser_uint256
from test_framework.p2p import P2PInterface
from test_framework.test_framework import DashTestFramework
from test_framework.util import wait_until_helper

LLMQ_TEST = 100

# A masternode protx/operator-pubkey pair accepted by the regtest-only `mnauth`
# debug RPC, used to mark a P2P connection as MNAuth-verified without BLS signing.
FAKE_PROTX = "cecf37bf0ec05d2d22cb8227f88074bb882b94cd2081ba318a5a444b1b15b9fd"
FAKE_PUBKEY = "8e7afdb849e5e2a085b035b62e21c0940c753f2d4501325743894c37162f287bccaffbedd60c36581dabbf127a22e43f"

DKG_PUSH_TYPES = [b"qcontrib", b"qcomplaint", b"qjustify", b"qpcommit"]
VALID_BLS_PUBKEY = bytes.fromhex(FAKE_PUBKEY)
INVALID_NONZERO_BLS_PUBKEY = b"\xff" * 48

# LLMQ_TEST dkgInterval; phaseBlocks=2, so stage 0=Initialized, 2=Contribute, 4=Complain.
CYCLE_LENGTH = 24

# Mirrors CDKGPendingMessages in src/llmq/dkgsessionhandler.h: the handler is built
# with maxMessagesPerSender = params.size * 2, and the queue-wide remote cap is
# twice that again. Both are per message type.
MAX_MESSAGES_PER_SENDER_FACTOR = 2
MAX_PENDING_REMOTE_FACTOR = 4


class msg_dkg_raw:
    """A DKG push message carrying an arbitrary raw payload (for adversarial intake tests)."""
    __slots__ = ("msgtype", "payload")

    def __init__(self, msgtype, payload=b""):
        self.msgtype = msgtype
        self.payload = payload

    def serialize(self):
        return self.payload

    def __repr__(self):
        return "msg_dkg_raw(type=%s, len=%d)" % (self.msgtype, len(self.payload))


def get_p2p_id(node, uacomment=None):
    def get_id():
        for p in node.getpeerinfo():
            for p2p in node.p2ps:
                if uacomment is not None and p2p.uacomment != uacomment:
                    continue
                if p["subver"] == p2p.strSubVer:
                    return p["id"]
        return None
    wait_until_helper(lambda: get_id() is not None, timeout=10)
    return get_id()


def wait_for_banscore(node, peer_id, expected_score):
    def get_score():
        for peer in node.getpeerinfo():
            if peer["id"] == peer_id:
                return peer["banscore"]
        return None
    wait_until_helper(lambda: get_score() == expected_score, timeout=10)


class DkgIntakeTest(DashTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser)

    def set_test_params(self):
        # -whitelist keeps the adversarial peer connected even after it crosses the
        #   discouragement threshold, so banscore stays observable for the score==100 cases.
        # -debug=net surfaces the Misbehaving reason strings in debug.log, while
        # -debug=llmq-dkg exposes worker and queue-boundary behavior.
        extra_args = [[
            "-whitelist=127.0.0.1",
            "-debug=net",
            "-debug=llmq-dkg",
            "-deprecatedrpc=banscore",
        ]] * 4
        self.set_dash_test_params(4, 3, extra_args=extra_args)

    def quorum_hash_prefix(self):
        # llmqType (1 byte) + quorumHash (32 bytes, little-endian) -- the on-wire prefix
        # shared by every DKG message, used so oversized/malformed payloads resolve to a
        # real in-progress quorum and reach the size/structural checks.
        return bytes([LLMQ_TEST]) + ser_uint256(int(self.quorum_hash, 16))

    def qcontrib_payload(self, blob_count, vvec_pubkey=VALID_BLS_PUBKEY, protx_hash=0):
        # CDKGContribution: llmqType, quorumHash, proTxHash, vvec, contributions, sig.
        # LLMQ_TEST uses threshold=2/minSize=2 by default, so blob_count=1 is
        # well-formed enough to deserialize but below the contribution lower bound.
        r = self.quorum_hash_prefix()
        r += ser_uint256(protx_hash)
        r += ser_compact_size(2) + vvec_pubkey + VALID_BLS_PUBKEY  # BLSVerificationVector
        r += VALID_BLS_PUBKEY  # CBLSIESMultiRecipientBlobs::ephemeralPubKey
        r += b"\x00" * 32  # CBLSIESMultiRecipientBlobs::ivSeed
        r += ser_compact_size(blob_count)
        for _ in range(blob_count):
            r += ser_compact_size(32) + b"\x00" * 32
        r += b"\x00" * 96  # sig
        return r

    def add_verified_peer(self, node, uacomment=None, protx=FAKE_PROTX):
        peer = node.add_p2p_connection(P2PInterface(), uacomment=uacomment)
        peer_id = get_p2p_id(node, uacomment)
        assert node.mnauth(peer_id, protx, FAKE_PUBKEY)
        return peer, peer_id

    def run_test(self):
        node0 = self.nodes[0]
        node0.sporkupdate("SPORK_17_QUORUM_DKG_ENABLED", 0)
        self.wait_for_sporks_same()

        # Mine a quorum so we have a quorumHash that resolves to a valid DKG base block.
        self.quorum_hash = self.mine_quorum()

        # Target an active masternode -- the realistic victim of these messages.
        mn_node = self.mninfo[0].get_node(self)

        self.test_unverified_sender_rejected(mn_node)
        self.test_oversized_rejected(mn_node)
        self.test_malformed_rejected(mn_node)
        self.test_trailing_bytes_rejected(mn_node)
        self.test_malformed_bls_pubkey_rejected_by_worker(mn_node)
        self.test_late_messages_bounded(mn_node)
        self.test_under_min_contribution_blobs_rejected(mn_node)
        self.test_unrequested_rejected(mn_node)

    def test_unverified_sender_rejected(self, node):
        self.log.info("Pushed DKG messages from a non-verified peer are rejected (Misbehaving 10 each)")
        peer = node.add_p2p_connection(P2PInterface())
        peer_id = get_p2p_id(node)
        wait_for_banscore(node, peer_id, 0)
        score = 0
        for msgtype in DKG_PUSH_TYPES:
            with node.assert_debug_log(["DKG message from non-verified peer"]):
                peer.send_message(msg_dkg_raw(msgtype, self.quorum_hash_prefix()))
                peer.sync_with_ping()
            score += 10
            wait_for_banscore(node, peer_id, score)
        node.disconnect_p2ps()

    def test_oversized_rejected(self, node):
        self.log.info("Oversized DKG payloads are rejected even from a verified peer (Misbehaving 100)")
        peer, peer_id = self.add_verified_peer(node)
        wait_for_banscore(node, peer_id, 0)
        # >1 MiB clears the hard ceiling regardless of quorum params, and stays under the
        # 3 MiB transport cap so the message is delivered to the handler.
        payload = self.quorum_hash_prefix() + b"\x00" * (1024 * 1024 + 4096)
        with node.assert_debug_log(["oversized DKG message"]):
            peer.send_message(msg_dkg_raw(b"qcontrib", payload))
            peer.sync_with_ping()
        wait_for_banscore(node, peer_id, 100)
        node.disconnect_p2ps()

    def test_malformed_rejected(self, node):
        self.log.info("Malformed DKG payloads are rejected even from a verified peer (Misbehaving 100)")
        peer, peer_id = self.add_verified_peer(node)
        wait_for_banscore(node, peer_id, 0)
        # Valid llmqType + quorumHash prefix, then too few bytes to deserialize a
        # CDKGContribution -> structural pre-validation rejects it before retention.
        payload = self.quorum_hash_prefix() + b"\x00\x00\x00\x00"
        with node.assert_debug_log(["malformed DKG message"]):
            peer.send_message(msg_dkg_raw(b"qcontrib", payload))
            peer.sync_with_ping()
        wait_for_banscore(node, peer_id, 100)
        node.disconnect_p2ps()

    def test_trailing_bytes_rejected(self, node):
        self.log.info("QCONTRIB with trailing bytes is rejected at intake (Misbehaving 100)")
        peer, peer_id = self.add_verified_peer(node)
        wait_for_banscore(node, peer_id, 0)
        with node.assert_debug_log(["malformed DKG message"]):
            peer.send_message(msg_dkg_raw(
                b"qcontrib",
                self.qcontrib_payload(blob_count=2) + b"\x00",
            ))
            peer.sync_with_ping()
        wait_for_banscore(node, peer_id, 100)
        node.disconnect_p2ps()

    def _start_fresh_dkg_cycle(self, nodes):
        """Land on the base block of a fresh DKG cycle (phase 1 / Initialized)."""
        skip_count = CYCLE_LENGTH - (self.nodes[0].getblockcount() % CYCLE_LENGTH)
        # move_blocks (not plain generate) so mocktime keeps up with the DKG phase clock.
        self.move_blocks(nodes, skip_count)
        self.quorum_hash = self.nodes[0].getbestblockhash()
        self.wait_for_quorum_phase(self.quorum_hash, 1, self.llmq_size, None, 0, self.mninfo)

    def test_malformed_bls_pubkey_rejected_by_worker(self, node):
        self.log.info("QCONTRIB BLS decoding is deferred to the DKG worker (Misbehaving 100)")
        nodes = [self.nodes[0]] + [mn.get_node(self) for mn in self.mninfo]
        # Queue during Initialized; Contribute's matching drain deserializes and scores.
        self._start_fresh_dkg_cycle(nodes)

        peer, peer_id = self.add_verified_peer(node)
        wait_for_banscore(node, peer_id, 0)
        with node.assert_debug_log(
            ["failed to deserialize message"],
            unexpected_msgs=["malformed DKG message"],
            timeout=10,
        ):
            peer.send_message(msg_dkg_raw(b"qcontrib", self.qcontrib_payload(
                blob_count=2,
                vvec_pubkey=INVALID_NONZERO_BLS_PUBKEY,
            )))
            peer.sync_with_ping()
            wait_for_banscore(node, peer_id, 0)
            self.move_blocks(nodes, 2)
        wait_for_banscore(node, peer_id, 100)
        node.disconnect_p2ps()

    def _send_late_qcontrib(self, node, peer, nonce):
        """Send a framing-valid, BLS-invalid QCONTRIB that no on-time worker will drain."""
        peer.send_message(msg_dkg_raw(b"qcontrib", self.qcontrib_payload(
            blob_count=2,
            vvec_pubkey=INVALID_NONZERO_BLS_PUBKEY,
            # Unique bytes per message so retention is bounded by the quotas rather
            # than by duplicate-hash suppression.
            protx_hash=nonce,
        )))
        peer.sync_with_ping()

    def test_late_messages_bounded(self, node):
        self.log.info("Late QCONTRIB retention is bounded per sender and queue-wide, then cleared without BLS decoding")
        nodes = [self.nodes[0]] + [mn.get_node(self) for mn in self.mninfo]
        self._start_fresh_dkg_cycle(nodes)
        stage = self.nodes[0].getblockcount() % CYCLE_LENGTH
        assert stage == 0, "expected DKG cycle base, got stage %d" % stage
        # Park in Complain so nothing drains pendingContributions for the rest of the round.
        complain_stage = 4
        self.move_blocks(nodes, complain_stage - stage)
        assert self.nodes[0].getblockcount() % CYCLE_LENGTH == complain_stage

        # Part 1: the per-sender quota is keyed by the verified proTxHash, so a peer
        # that reconnects under a fresh NodeId keeps spending the same budget.
        sender_quota = MAX_MESSAGES_PER_SENDER_FACTOR * self.llmq_size
        nonce = 0
        for i in range(sender_quota):
            nonce += 1
            peer, peer_id = self.add_verified_peer(node, "dkg-reconnect-%d" % i)
            self._send_late_qcontrib(node, peer, nonce)
            wait_for_banscore(node, peer_id, 0)
            peer.peer_disconnect()
            peer.wait_for_disconnect()

        nonce += 1
        quota_peer, quota_peer_id = self.add_verified_peer(node, "dkg-reconnect-over")
        with node.assert_debug_log(["too many messages from %s" % FAKE_PROTX]):
            self._send_late_qcontrib(node, quota_peer, nonce)
        wait_for_banscore(node, quota_peer_id, 0)

        # Part 2: distinct senders each get their own quota, but the queue-wide cap
        # still bounds total retention. Keep the last accepted peer connected to
        # verify that round-start clearing does not score stale BLS encodings.
        queue_limit = MAX_PENDING_REMOTE_FACTOR * self.llmq_size
        retained_peer_id = None
        for i in range(queue_limit - sender_quota):
            nonce += 1
            peer, peer_id = self.add_verified_peer(node, "dkg-sender-%d" % i, protx="%064x" % (0xd0 + i))
            self._send_late_qcontrib(node, peer, nonce)
            wait_for_banscore(node, peer_id, 0)
            retained_peer_id = peer_id

        nonce += 1
        overflow_peer, overflow_peer_id = self.add_verified_peer(node, "dkg-sender-over", protx="%064x" % 0xffff)
        with node.assert_debug_log(["pending queue full"]):
            self._send_late_qcontrib(node, overflow_peer, nonce)
        wait_for_banscore(node, overflow_peer_id, 0)

        # Crossing the phase boundary must clear a bounded raw queue and finish
        # initializing the next session without deserializing stale BLS points.
        remaining = CYCLE_LENGTH - (self.nodes[0].getblockcount() % CYCLE_LENGTH)
        with node.assert_debug_log(
            [],
            unexpected_msgs=["message failed structure check", "failed to deserialize message"],
            timeout=60,
        ):
            self.move_blocks(nodes, remaining)
            self.quorum_hash = self.nodes[0].getbestblockhash()
            self.wait_for_quorum_phase(self.quorum_hash, 1, self.llmq_size, None, 0, self.mninfo)
        assert retained_peer_id is not None
        wait_for_banscore(node, retained_peer_id, 0)
        wait_for_banscore(node, overflow_peer_id, 0)
        node.disconnect_p2ps()

    def test_under_min_contribution_blobs_rejected(self, node):
        self.log.info("QCONTRIB with fewer than minSize encrypted blobs is rejected (Misbehaving 100)")
        peer, peer_id = self.add_verified_peer(node)
        wait_for_banscore(node, peer_id, 0)
        with node.assert_debug_log(["malformed DKG message"]):
            peer.send_message(msg_dkg_raw(b"qcontrib", self.qcontrib_payload(blob_count=1)))
            peer.sync_with_ping()
        wait_for_banscore(node, peer_id, 100)
        node.disconnect_p2ps()

    def test_unrequested_rejected(self, node):
        self.log.info("A well-formed but unrequested DKG message is dropped (Misbehaving 10)")
        peer, peer_id = self.add_verified_peer(node)
        wait_for_banscore(node, peer_id, 0)
        # Passes every earlier check (verified sender, known quorum, size, structure) and is
        # rejected purely because the peer neither announced it nor was asked for it. DKG
        # messages only ever travel inv -> getdata, so a pushed one is unsolicited by
        # definition and must not reach the pending queues.
        with node.assert_debug_log(["unrequested DKG message"]):
            peer.send_message(msg_dkg_raw(b"qcontrib", self.qcontrib_payload(blob_count=2)))
            peer.sync_with_ping()
        wait_for_banscore(node, peer_id, 10)
        node.disconnect_p2ps()


if __name__ == '__main__':
    DkgIntakeTest().main()
