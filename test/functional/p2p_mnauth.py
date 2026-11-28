#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""
Test that a masternode sends MNAUTH only to inbound peers that reached it on its registered
service: the local port must match and the connection must not come from another network class.
"""

import platform

from test_framework.test_framework import (
    DashTestFramework,
    MasternodeInfo,
)
from test_framework.util import (
    assert_equal,
    force_finish_mnsync,
    p2p_port,
)


class P2PMNAUTHTest(DashTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser)

    def set_test_params(self):
        # Keep every bind of the masternode visible here instead of the implicit bind=127.0.0.1
        self.bind_to_localhost_only = False
        self.mn_port = p2p_port(2)
        self.alt_port = p2p_port(10)
        # Only Linux routes all of 127.0.0.0/8 to the loopback interface by default
        self.has_loopback_aliases = platform.system() == "Linux"

        self.mn_args = [f"-externalip=127.0.0.1:{self.mn_port}"]
        binds = [f"-bind=127.0.0.1:{self.mn_port}", f"-bind=127.0.0.1:{self.alt_port}"]
        if self.has_loopback_aliases:
            binds.append(f"-bind=127.0.0.2:{self.mn_port}")
        self.set_dash_test_params(3, 1, extra_args=[[], [], self.mn_args + binds])

    def probe(self, addr, *, expect_mnauth):
        transport = 'v2' if self.options.v2transport else 'v1'
        self.log.info(f"Probe {addr} over {transport}, expect {'MNAUTH' if expect_mnauth else 'no MNAUTH'}")
        if expect_mnauth:
            connector_log = f"Masternode probe successful for {self.mn.proTxHash}"
            mn_log = "Sending MNAUTH"
        else:
            connector_log = "connection is a masternode probe but first received message is not MNAUTH"
            mn_log = "Not sending MNAUTH on unexpected local service"
        with self.mn_node.assert_debug_log([mn_log]), self.connector.assert_debug_log([connector_log]):
            assert_equal(self.connector.masternode("connect", addr, self.options.v2transport), "successfully connected")

    def wait_for_ready(self):
        self.wait_until(lambda: self.mn_node.masternode("status")["state"] == "READY")
        assert_equal(self.mn_node.masternode("status")["service"], f"127.0.0.1:{self.mn_port}")

    def run_test(self):
        self.mn: MasternodeInfo = self.mninfo[0]
        self.mn_node = self.mn.get_node(self)
        self.connector = self.nodes[1]
        assert_equal(self.mn.nodePort, self.mn_port)

        self.wait_for_ready()
        self.probe(f"127.0.0.1:{self.mn_port}", expect_mnauth=True)
        self.probe(f"127.0.0.1:{self.alt_port}", expect_mnauth=False)
        if self.has_loopback_aliases:
            # NAT-style: the bind address differs from the advertised one, the port matches
            self.probe(f"127.0.0.2:{self.mn_port}", expect_mnauth=True)

        self.log.info("Tag the registered port as an onion listener: port matches, only the network differs")
        self.stop_node(self.mn.nodeIdx)
        self.start_node(self.mn.nodeIdx, extra_args=[f"-masternodeblsprivkey={self.mn.keyOperator}",
                                                     f"-bind=127.0.0.1:{self.mn_port}=onion"] + self.mn_args)
        force_finish_mnsync(self.mn_node)
        self.wait_for_ready()
        self.probe(f"127.0.0.1:{self.mn_port}", expect_mnauth=False)


if __name__ == '__main__':
    P2PMNAUTHTest().main()
