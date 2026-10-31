#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test which listener the automatic Tor onion service forwards to.

A fake Tor control port records the commands dashd sends. The default
-listenonion must never stop the node from starting: without a usable onion
bind it still talks to Tor, but creates no onion service.
"""

import os
import socket
import threading

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    p2p_port,
)

ONION_ID = "pg6mmjiyjmcrsslvykfwnntlaru7p5svn6y2ymmju6nubxndf4pscryd"
NO_TARGET_LOG = "tor: Not creating an onion service because there is no dedicated onion bind"


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

    def start_with_tor(self, extra_args, *, expected_log):
        """Start node 0 with the fake Tor control port and return the commands of its Tor control connection."""
        node = self.nodes[0]
        connections_before = len(self.tor.connections)
        log_offset = os.path.getsize(node.debug_log_path) if os.path.exists(node.debug_log_path) else 0
        node.start(extra_args + [f"-torcontrol=127.0.0.1:{self.tor.port}", "-debug=tor"])
        node.wait_for_rpc_connection()

        def logged():
            with open(node.debug_log_path, "rb") as f:
                f.seek(log_offset)
                return expected_log.encode() in f.read()
        self.wait_until(logged)
        assert_equal(len(self.tor.connections), connections_before + 1)
        commands = self.tor.connections[-1]
        # dashd logs before the GETINFO it queued reaches the fake Tor.
        self.wait_until(lambda: "GETINFO net/listeners/socks" in commands)
        return commands

    def run_test(self):
        self.log.info("-bind without a dedicated onion bind starts and creates no onion service")
        commands = self.start_with_tor([], expected_log=NO_TARGET_LOG)
        assert_equal(commands, ["PROTOCOLINFO 1", "AUTHENTICATE", "GETINFO net/listeners/socks"])
        self.stop_node(0)

        self.log.info("A wildcard onion bind starts and creates no onion service")
        commands = self.start_with_tor([f"-bind=0.0.0.0:{p2p_port(1)}=onion"], expected_log=NO_TARGET_LOG)
        assert_equal(add_onion_target(commands), None)
        self.stop_node(0)

        self.log.info("With wildcard and specific onion binds, the first specific one is the target")
        specific = f"127.0.0.1:{p2p_port(2)}"
        commands = self.start_with_tor([f"-bind=0.0.0.0:{p2p_port(1)}=onion", f"-bind={specific}=onion"], expected_log="ADD_ONION successful")
        assert_equal(add_onion_target(commands), specific)
        self.stop_node(0, expected_stderr=f"Warning: More than one onion bind address is provided. Using {specific} for the automatically created Tor onion service.")


if __name__ == '__main__':
    TorOnionTargetTest().main()
