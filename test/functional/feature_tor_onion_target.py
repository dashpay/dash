#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test which listener the automatic Tor onion service forwards to.

A fake Tor control port records the commands dashd sends. The default
-listenonion must never stop the node from starting: without a usable onion
bind it still talks to Tor, but creates no onion service.

This is the only test that binds the fixed regtest onion target port, so it
does not collide with other tests run in parallel.
"""

import os
import socket
import threading

from test_framework.messages import msg_getaddr
from test_framework.p2p import P2PInterface
from test_framework.test_framework import BitcoinTestFramework
from test_framework.test_node import ErrorMatch
from test_framework.util import (
    assert_equal,
    p2p_port,
)

# Regtest default onion service target
ONION_TARGET_HOST = "127.0.0.1"
ONION_TARGET_PORT = 19896
ONION_TARGET = f"{ONION_TARGET_HOST}:{ONION_TARGET_PORT}"
EXTERNAL_IP = "1.2.3.4"
ONION_ID = "pg6mmjiyjmcrsslvykfwnntlaru7p5svn6y2ymmju6nubxndf4pscryd"
NO_TARGET_LOG = "tor: Not creating an onion service because there is no dedicated onion bind"
IMPLICIT_BIND_FAILED_LOG = f"Unable to bind the default onion service target {ONION_TARGET}, continuing without it"


class FakeTorControl:
    """Answers PROTOCOLINFO, AUTHENTICATE, GETINFO and ADD_ONION, and records the commands of each connection."""

    def __init__(self):
        self.srv = socket.create_server(("127.0.0.1", 0))
        self.port = self.srv.getsockname()[1]
        self.connections = []
        threading.Thread(target=self._accept, daemon=True).start()

    def _accept(self):
        while True:
            try:
                conn, _ = self.srv.accept()
            except OSError:
                return
            commands = []
            self.connections.append(commands)
            threading.Thread(target=self._serve, args=(conn, commands), daemon=True).start()

    def _serve(self, conn, commands):
        with conn, conn.makefile("rwb", buffering=0) as f:
            for line in f:
                cmd = line.decode().strip()
                commands.append(cmd)
                if cmd.startswith("PROTOCOLINFO"):
                    f.write(b'250-PROTOCOLINFO 1\r\n250-AUTH METHODS=NULL\r\n250-VERSION Tor="0.4.8.12"\r\n250 OK\r\n')
                elif cmd == "GETINFO net/listeners/socks":
                    f.write(b'250-net/listeners/socks="127.0.0.1:9050"\r\n250 OK\r\n')
                elif cmd.startswith("ADD_ONION"):
                    f.write(f"250-ServiceID={ONION_ID}\r\n250-PrivateKey=ED25519-V3:AAAA\r\n250 OK\r\n".encode())
                else:
                    f.write(b"250 OK\r\n")


class AddrReceiver(P2PInterface):
    def __init__(self):
        super().__init__()
        self.addrs = []

    def on_addr(self, message):
        self.addrs += [a.ip for a in message.addrs]

    def on_addrv2(self, message):
        self.addrs += [a.ip for a in message.addrs]


def add_onion_target(commands):
    """Return the target of the ADD_ONION command (ADD_ONION <key> Port=<virtport>,<target>), or None."""
    add_onion = [c for c in commands if c.startswith("ADD_ONION")]
    return add_onion[0].split("Port=")[1].split(",", 1)[1] if add_onion else None


class TorOnionTargetTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1

    def setup_network(self):
        self.tor = FakeTorControl()
        self.add_nodes(self.num_nodes)
        # The framework disables -listenonion for every node; this test needs its default.
        self.nodes[0].replace_in_config([("listenonion=0\n", "")])

    def start_with_tor(self, extra_args, *, expected_logs):
        """Start node 0 with the fake Tor control port and return the commands of its Tor control connection."""
        node = self.nodes[0]
        connections_before = len(self.tor.connections)
        log_offset = os.path.getsize(node.debug_log_path) if os.path.exists(node.debug_log_path) else 0
        node.start(extra_args + [f"-torcontrol=127.0.0.1:{self.tor.port}", "-debug=tor"])
        node.wait_for_rpc_connection()

        def logged():
            with open(node.debug_log_path, "rb") as f:
                f.seek(log_offset)
                log = f.read()
            return all(msg.encode() in log for msg in expected_logs)
        self.wait_until(logged)
        assert_equal(len(self.tor.connections), connections_before + 1)
        commands = self.tor.connections[-1]
        # dashd logs before the GETINFO it queued reaches the fake Tor.
        self.wait_until(lambda: "GETINFO net/listeners/socks" in commands)
        return commands

    def run_test(self):
        node = self.nodes[0]

        self.log.info("-bind without a dedicated onion bind gets the default onion bind as Tor target")
        commands = self.start_with_tor([f"-externalip={EXTERNAL_IP}"], expected_logs=["ADD_ONION successful"])
        assert_equal(add_onion_target(commands), ONION_TARGET)
        self.generate(node, 1, sync_fun=self.no_op)

        self.log.info("A peer forwarded by Tor is an onion peer and isn't told our clearnet address")
        onion_peer = node.add_p2p_connection(AddrReceiver(), dstaddr=ONION_TARGET_HOST, dstport=ONION_TARGET_PORT)
        clearnet_peer = node.add_p2p_connection(AddrReceiver())
        networks = {p["addrbind"]: p["network"] for p in node.getpeerinfo()}
        assert_equal(networks[ONION_TARGET], "onion")
        assert_equal(networks[f"127.0.0.1:{p2p_port(0)}"], "not_publicly_routable")
        for peer in (onion_peer, clearnet_peer):
            peer.send_and_ping(msg_getaddr())
        self.wait_until(lambda: EXTERNAL_IP in clearnet_peer.addrs)
        onion_peer.sync_with_ping()
        assert EXTERNAL_IP not in onion_peer.addrs
        self.stop_node(0)

        self.log.info("A wildcard onion bind starts and creates no onion service")
        commands = self.start_with_tor([f"-bind=0.0.0.0:{p2p_port(1)}=onion"], expected_logs=[NO_TARGET_LOG])
        assert_equal(add_onion_target(commands), None)
        self.stop_node(0)

        self.log.info("With wildcard and specific onion binds, the first specific one is the target")
        specific = f"127.0.0.1:{p2p_port(2)}"
        commands = self.start_with_tor([f"-bind=0.0.0.0:{p2p_port(1)}=onion", f"-bind={specific}=onion"], expected_logs=["ADD_ONION successful"])
        assert_equal(add_onion_target(commands), specific)
        self.stop_node(0, expected_stderr=f"Warning: More than one onion bind address is provided. Using {specific} for the automatically created Tor onion service.")

        self.log.info("Our own -whitebind on the default onion target is neither the Tor target nor tagged as onion")
        self.start_with_tor([f"-whitebind=noban@{ONION_TARGET}"], expected_logs=[IMPLICIT_BIND_FAILED_LOG, NO_TARGET_LOG])
        node.add_p2p_connection(P2PInterface(), dstaddr=ONION_TARGET_HOST, dstport=ONION_TARGET_PORT)
        assert_equal([p["network"] for p in node.getpeerinfo()], ["not_publicly_routable"])
        self.stop_node(0)

        self.log.info("Hold the default onion target port, as another instance on this host would")
        with socket.create_server((ONION_TARGET_HOST, ONION_TARGET_PORT)):
            self.log.info("-bind with the default onion bind taken starts and creates no onion service")
            commands = self.start_with_tor([], expected_logs=[IMPLICIT_BIND_FAILED_LOG, NO_TARGET_LOG])
            assert_equal(commands, ["PROTOCOLINFO 1", "AUTHENTICATE", "GETINFO net/listeners/socks"])
            self.stop_node(0)

            self.log.info("An explicit -listenonion=1 requires the default onion bind")
            node.assert_start_raises_init_error(
                ["-listenonion=1", f"-torcontrol=127.0.0.1:{self.tor.port}"],
                f"Error: Unable to bind to {ONION_TARGET} on this computer.",
                match=ErrorMatch.PARTIAL_REGEX,
            )

            self.log.info("An explicit onion bind on a taken port still fails")
            node.assert_start_raises_init_error(
                [f"-bind={ONION_TARGET}=onion"],
                f"Error: Unable to bind to {ONION_TARGET} on this computer.",
                match=ErrorMatch.PARTIAL_REGEX,
            )

            self.log.info("Without -bind, the default onion bind taken starts and creates no onion service")
            node.replace_in_config([("bind=127.0.0.1\n", "")])
            # Without -bind, the framework's connect=0 would otherwise turn off listening.
            commands = self.start_with_tor(["-listen=1"], expected_logs=[IMPLICIT_BIND_FAILED_LOG, NO_TARGET_LOG])
            assert_equal(add_onion_target(commands), None)
            self.stop_node(0)

            self.log.info("Without -bind, the default onion bind taken starts with -listenonion=0 too")
            self.start_node(0, extra_args=["-listen=1", "-listenonion=0"])
            self.stop_node(0)


if __name__ == '__main__':
    TorOnionTargetTest().main()
