#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test that an oversized govsync bloom filter is rejected.

A MNGOVERNANCESYNC ("govsync") request carries a peer-supplied CBloomFilter. For a
per-object request the node tests that filter against every cached vote, and
CBloomFilter::contains() loops nHashFuncs times. nHashFuncs is deserialized without
bounds, so an unbounded value would force an enormous amount of work while the message
processing mutex is held. The handler must reject any filter that is not within the
standard size constraints (vData <= 36000 bytes, nHashFuncs <= 50), matching filterload.
"""
from test_framework.messages import msg_generic, msg_govsync, ser_compact_size, ser_uint256
from test_framework.p2p import P2PInterface
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import force_finish_mnsync

# CBloomFilter size constraints (src/common/bloom.h).
MAX_HASH_FUNCS = 50
# serialize.h MAX_SIZE: the largest count ReadCompactSize() accepts, so a declared
# vData length of this value reaches the vData cap, not the compact-size guard.
MAX_SIZE = 0x02000000


class GovsyncBloomCapTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1

    def run_test(self):
        node = self.nodes[0]
        force_finish_mnsync(node)

        self.log.info("A govsync request with a well-formed bloom filter is accepted (no false positive)")
        good_peer = node.add_p2p_connection(P2PInterface())
        good_peer.send_message(msg_govsync(nHashFuncs=MAX_HASH_FUNCS))
        good_peer.sync_with_ping()
        assert good_peer.is_connected
        node.disconnect_p2ps()

        self.log.info("A govsync request with an out-of-bounds bloom filter (nHashFuncs > 50) is rejected and the peer disconnected")
        bad_peer = node.add_p2p_connection(P2PInterface())
        bad_peer.send_message(msg_govsync(nHashFuncs=0xFFFFFFFF))
        bad_peer.wait_for_disconnect()

        self.log.info("A govsync request declaring an oversized filter vData length with the bytes omitted is rejected before allocation")
        # nProp (32 bytes) then a CompactSize(MAX_SIZE) vData length with no bytes. Without the
        # cap this would fall into net_processing's outer catch (no Misbehaving, no disconnect).
        raw_peer = node.add_p2p_connection(P2PInterface())
        raw_payload = ser_uint256(0) + ser_compact_size(MAX_SIZE)
        with node.assert_debug_log(['Misbehaving']):
            raw_peer.send_message(msg_generic(b'govsync', raw_payload))
            raw_peer.wait_for_disconnect()


if __name__ == '__main__':
    GovsyncBloomCapTest().main()
