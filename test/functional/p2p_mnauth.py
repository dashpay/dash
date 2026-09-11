#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

"""
Test MNAUTH emission on the registered masternode service only.
"""

import platform

from test_framework.test_framework import (
    DashTestFramework,
    MasternodeInfo,
)
from test_framework.util import (
    assert_equal,
    p2p_port,
)


class P2PMNAUTHTest(DashTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser)

    def set_test_params(self):
        # Disable the framework's implicit `bind=127.0.0.1` so all binds for the
        # masternode are visible in this file rather than coming from the conf.
        self.bind_to_localhost_only = False
        self.alt_port = p2p_port(10)
        self.mn_port = p2p_port(2)
        # NAT-style variant requires a non-127.0.0.1 loopback bind; only Linux
        # routes 127.0.0.0/8 to lo by default.
        self.nat_capable = platform.system() == "Linux"

        mn_args = [
            f"-bind=127.0.0.1:{self.mn_port}",
            f"-bind=127.0.0.1:{self.alt_port}",
            f"-externalip=127.0.0.1:{self.mn_port}",
        ]
        if self.nat_capable:
            mn_args.append(f"-bind=127.0.0.2:{self.mn_port}")

        self.set_dash_test_params(3, 1, extra_args=[[], [], mn_args])

    def run_test(self):
        masternode: MasternodeInfo = self.mninfo[0]
        masternode_node = masternode.get_node(self)
        connector = self.nodes[1]
        use_v2transport = self.options.v2transport

        expected_addr = f"127.0.0.1:{masternode.nodePort}"
        alternate_addr = f"127.0.0.1:{self.alt_port}"

        self.wait_until(lambda: masternode_node.masternode("status")["state"] == "READY")
        assert_equal(masternode_node.masternode("status")["service"], expected_addr)

        self.log.info(f"Connect to the registered masternode service over {'v2' if use_v2transport else 'v1'} and expect MNAUTH")
        with connector.assert_debug_log([f"Masternode probe successful for {masternode.proTxHash}"]):
            assert_equal(connector.masternode("connect", expected_addr, use_v2transport), "successfully connected")

        self.log.info(f"Connect to the alternate bind over {'v2' if use_v2transport else 'v1'} and expect no MNAUTH")
        with masternode_node.assert_debug_log(["Not sending MNAUTH on unexpected local service"]):
            with connector.assert_debug_log(["connection is a masternode probe but first received message is not MNAUTH"]):
                assert_equal(connector.masternode("connect", alternate_addr, use_v2transport), "successfully connected")

        if self.nat_capable:
            nat_addr = f"127.0.0.2:{self.mn_port}"
            self.log.info(f"Connect to a different loopback IP on the registered port over {'v2' if use_v2transport else 'v1'} (NAT-style: addrBind address differs from advertised externalip, ports match) and expect MNAUTH")
            with connector.assert_debug_log([f"Masternode probe successful for {masternode.proTxHash}"]):
                assert_equal(connector.masternode("connect", nat_addr, use_v2transport), "successfully connected")


if __name__ == '__main__':
    P2PMNAUTHTest().main()
