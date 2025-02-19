#!/usr/bin/env python3
# Copyright (c) 2015-2020 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test node responses to invalid network messages."""


from test_framework.messages import (
    CBlockHeader,
    CInv,
    msg_ping,
    ser_string,
    MAX_HEADERS_COMPRESSED_RESULT,
    MAX_HEADERS_UNCOMPRESSED_RESULT,
    MAX_INV_SIZE,
    MAX_PROTOCOL_MESSAGE_LENGTH,
    msg_getdata,
    msg_headers,
    msg_headers2,
    msg_inv,
    MSG_TX,
    msg_version,
)
from test_framework.p2p import (
    P2PDataStore, P2PInterface
)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal

VALID_DATA_LIMIT = MAX_PROTOCOL_MESSAGE_LENGTH - 5  # Account for the 5-byte length prefix


class msg_unrecognized:
    """Nonsensical message. Modeled after similar types in test_framework.messages."""

    msgtype = b'badmsg\x01'

    def __init__(self, *, str_data):
        self.str_data = str_data.encode() if not isinstance(str_data, bytes) else str_data

    def serialize(self):
        return ser_string(self.str_data)

    def __repr__(self):
        return "{}(data={})".format(self.msgtype, self.str_data)


class InvalidMessagesTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [["-whitelist=addr@127.0.0.1"]]

    def run_test(self):
        self.test_buffer()
        self.test_duplicate_version_msg()
        self.test_magic_bytes()
        self.test_checksum()
        self.test_size()
        self.test_msgtype()
        self.test_oversized_inv_msg()
        self.test_oversized_getdata_msg()
        self.test_oversized_headers_msg()
        self.test_resource_exhaustion()

    def test_buffer(self):
        self.log.info("Test message with header split across two buffers is received")
        conn = self.nodes[0].add_p2p_connection(P2PDataStore())
        # After add_p2p_connection both sides have the verack processed.
        # However the pong from conn in reply to the ping from the node has not
        # been processed and recorded in totalbytesrecv.
        # Flush the pong from conn by sending a ping from conn.
        conn.sync_with_ping(timeout=2)
        # Create valid message
        msg = conn.build_message(msg_ping(nonce=12345))
        cut_pos = 12  # Chosen at an arbitrary position within the header
        # Send message in two pieces
        before = self.nodes[0].getnettotals()['totalbytesrecv']
        conn.send_raw_message(msg[:cut_pos])
        # Wait until node has processed the first half of the message
        self.wait_until(lambda: self.nodes[0].getnettotals()['totalbytesrecv'] != before)
        middle = self.nodes[0].getnettotals()['totalbytesrecv']
        assert_equal(middle, before + cut_pos)
        conn.send_raw_message(msg[cut_pos:])
        conn.sync_with_ping(timeout=2)
        self.nodes[0].disconnect_p2ps()

    def test_duplicate_version_msg(self):
        self.log.info("Test duplicate version message is ignored")
        conn = self.nodes[0].add_p2p_connection(P2PDataStore())
        with self.nodes[0].assert_debug_log(['redundant version message from peer']):
            conn.send_and_ping(msg_version())
        self.nodes[0].disconnect_p2ps()

    def test_magic_bytes(self):
        # Skip with v2, magic bytes are v1-specific
        if self.options.v2transport:
            return
        self.log.info("Test message with invalid magic bytes disconnects peer")
        conn = self.nodes[0].add_p2p_connection(P2PDataStore())
        with self.nodes[0].assert_debug_log(['Header error: Wrong MessageStart ffffffff received']):
            msg = conn.build_message(msg_unrecognized(str_data="d"))
            # modify magic bytes
            msg = b'\xff' * 4 + msg[4:]
            conn.send_raw_message(msg)
            conn.wait_for_disconnect(timeout=5)
        self.nodes[0].disconnect_p2ps()

    def test_checksum(self):
        # Skip with v2, the checksum is v1-specific
        if self.options.v2transport:
            return
        self.log.info("Test message with invalid checksum logs an error")
        conn = self.nodes[0].add_p2p_connection(P2PDataStore())
        with self.nodes[0].assert_debug_log(['Header error: Wrong checksum (badmsg, 2 bytes), expected 78df0a04 was ffffffff']):
            msg = conn.build_message(msg_unrecognized(str_data="d"))
            # Checksum is after start bytes (4B), message type (12B), len (4B)
            cut_len = 4 + 12 + 4
            # modify checksum
            msg = msg[:cut_len] + b'\xff' * 4 + msg[cut_len + 4:]
            conn.send_raw_message(msg)
            conn.sync_with_ping(timeout=1)
        # Check that traffic is accounted for (24 bytes header + 2 bytes payload)
        assert_equal(self.nodes[0].getpeerinfo()[0]['bytesrecv_per_msg']['*other*'], 26)
        self.nodes[0].disconnect_p2ps()

    def test_size(self):
        self.log.info("Test message with oversized payload disconnects peer")
        conn = self.nodes[0].add_p2p_connection(P2PDataStore())
        error_msg = (
            ['V2 transport error: packet too large (3145742 bytes)'] if self.options.v2transport
            else ['Header error: Size too large (badmsg, 3145729 bytes)']
        )
        with self.nodes[0].assert_debug_log(error_msg):
            msg = msg_unrecognized(str_data="d"*(VALID_DATA_LIMIT + 1))
            msg = conn.build_message(msg)
            conn.send_raw_message(msg)
            conn.wait_for_disconnect(timeout=5)
        self.nodes[0].disconnect_p2ps()

    def test_msgtype(self):
        self.log.info("Test message with invalid message type logs an error")
        conn = self.nodes[0].add_p2p_connection(P2PDataStore())
        if self.options.v2transport:
            msgtype = 99 # not defined
            msg = msg_unrecognized(str_data="d")
            contents = msgtype.to_bytes(1, 'big') + msg.serialize()
            tmsg = conn.v2_state.v2_enc_packet(contents, ignore=False)
            with self.nodes[0].assert_debug_log(['V2 transport error: invalid message type']):
                conn.send_raw_message(tmsg)
                conn.sync_with_ping(timeout=1)
            # Check that traffic is accounted for (20 bytes plus 3 bytes contents)
            assert_equal(self.nodes[0].getpeerinfo()[0]['bytesrecv_per_msg']['*other*'], 23)
        else:
            with self.nodes[0].assert_debug_log(['Header error: Invalid message type']):
                msg = msg_unrecognized(str_data="d")
                msg = conn.build_message(msg)
                # Modify msgtype
                msg = msg[:7] + b'\x00' + msg[7 + 1:]
                conn.send_raw_message(msg)
                conn.sync_with_ping(timeout=1)
                # Check that traffic is accounted for (24 bytes header + 2 bytes payload)
                assert_equal(self.nodes[0].getpeerinfo()[0]['bytesrecv_per_msg']['*other*'], 26)
        self.nodes[0].disconnect_p2ps()

    def test_oversized_msg(self, msg, size):
        msg_type = msg.msgtype.decode('ascii')
        self.log.info("Test {} message of size {} is logged as misbehaving".format(msg_type, size))
        with self.nodes[0].assert_debug_log(['Misbehaving', '{} message size = {}'.format(msg_type, size)]):
            self.nodes[0].add_p2p_connection(P2PInterface()).send_and_ping(msg)
        self.nodes[0].disconnect_p2ps()

    def test_oversized_inv_msg(self):
        size = MAX_INV_SIZE + 1
        self.test_oversized_msg(msg_inv([CInv(MSG_TX, 1)] * size), size)

    def test_oversized_getdata_msg(self):
        size = MAX_INV_SIZE + 1
        self.test_oversized_msg(msg_getdata([CInv(MSG_TX, 1)] * size), size)

    def test_oversized_headers_msg(self):
        size = MAX_HEADERS_UNCOMPRESSED_RESULT + 1
        self.test_oversized_msg(msg_headers([CBlockHeader()] * size), size)

    def test_oversized_headers2_msg(self):
        size = MAX_HEADERS_COMPRESSED_RESULT + 1
        self.test_oversized_msg(msg_headers2([CBlockHeader()] * size), size)

    def test_resource_exhaustion(self):
        self.log.info("Test node stays up despite many large junk messages")
        # Don't use v2 here - the non-optimised encryption would take too long to encrypt
        # the large messages
        conn = self.nodes[0].add_p2p_connection(P2PDataStore(), supports_v2_p2p=False)
        conn2 = self.nodes[0].add_p2p_connection(P2PDataStore(), supports_v2_p2p=False)
        msg_at_size = msg_unrecognized(str_data="b" * VALID_DATA_LIMIT)

        assert len(msg_at_size.serialize()) == MAX_PROTOCOL_MESSAGE_LENGTH

        self.log.info("(a) Send 80 messages, each of maximum valid data size (4MB)")
        for _ in range(80):
            conn.send_message(msg_at_size)

        # Check that, even though the node is being hammered by nonsense from one
        # connection, it can still service other peers in a timely way.
        self.log.info("(b) Check node still services peers in a timely way")
        for _ in range(20):
            conn2.sync_with_ping(timeout=2)

        self.log.info("(c) Wait for node to drop junk messages, while remaining connected")
        conn.sync_with_ping(timeout=400)

        # Despite being served up a bunch of nonsense, the peers should still be connected.
        assert conn.is_connected
        assert conn2.is_connected
        self.nodes[0].disconnect_p2ps()


if __name__ == '__main__':
    InvalidMessagesTest().main()
