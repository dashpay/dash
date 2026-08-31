#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""
Test that MNAUTH is suppressed when an inbound arrives over a different network class
than the masternode's registered service. Specifically: an IPv4-registered masternode
must not emit MNAUTH on a Tor-tagged inbound, even when the local port matches — that
would deanonymize the masternode by linking its onion endpoint to its IPv4 identity.

Set up: bind the masternode's listener at its registered port with `=onion`, so that any
inbound arriving there is treated by dashd as a Tor connection (m_inbound_onion=true).
Port match is then guaranteed and only the network differs, isolating the network check.
"""

from test_framework.test_framework import (
    DashTestFramework,
    MasternodeInfo,
)
from test_framework.util import (
    assert_equal,
    p2p_port,
)


class P2PMNAUTHOnionTest(DashTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser)

    def set_test_params(self):
        self.bind_to_localhost_only = False
        self.mn_port = p2p_port(2)
        self.set_dash_test_params(3, 1, extra_args=[
            [],
            [],
            [
                f"-bind=127.0.0.1:{self.mn_port}=onion",
                f"-externalip=127.0.0.1:{self.mn_port}",
            ],
        ])

    def run_test(self):
        masternode: MasternodeInfo = self.mninfo[0]
        masternode_node = masternode.get_node(self)
        connector = self.nodes[1]
        use_v2transport = self.options.v2transport

        expected_addr = f"127.0.0.1:{masternode.nodePort}"

        self.wait_until(lambda: masternode_node.masternode("status")["state"] == "READY")
        assert_equal(masternode_node.masternode("status")["service"], expected_addr)

        self.log.info(f"Probe the MN over {'v2' if use_v2transport else 'v1'} via its onion-tagged listener; ports match, network differs, expect no MNAUTH")
        with masternode_node.assert_debug_log(["Not sending MNAUTH on unexpected local service"]):
            with connector.assert_debug_log(["connection is a masternode probe but first received message is not MNAUTH"]):
                assert_equal(connector.masternode("connect", expected_addr, use_v2transport), "successfully connected")


if __name__ == '__main__':
    P2PMNAUTHOnionTest().main()
