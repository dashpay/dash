#!/usr/bin/env python3
# Copyright (c) 2024 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

'''
rpc_protx_legacy_bls.py

Regression test for protx BLS scheme mismatch bug.

Tests that masternodes registered with legacy BLS (before V19 activation)
can successfully use protx update_service and protx revoke after V19
activation. This is a regression test for a bug where the RPC used the
masternode's state version to determine the BLS signing scheme, causing
a mismatch with the validator which uses the global scheme flag.

The bug scenario:
1. Masternode registered before V19 has legacy BLS keys (nVersion=1)
2. After V19 activation, global BLS scheme switches to basic
3. protx commands were incorrectly using the MN's state version to determine
   signing scheme, causing "bad-protx-sig" errors

The fix ensures that protx commands use the network deployment status
(V19 active = basic scheme) rather than the MN's state version.
'''

from test_framework.test_framework import DashTestFramework, MasternodeInfo
from test_framework.util import assert_equal


class ProtxLegacyBLSTest(DashTestFramework):
    def set_test_params(self):
        # Set up with V19 activating at block 200
        # This gives us time to register masternodes before V19
        self.extra_args = [[
            '-testactivationheight=v19@200',
            '-deprecatedrpc=legacy_mn',  # Required for legacy BLS key generation
        ]] * 4
        # Start with 3 masternodes (registered before V19, so legacy BLS)
        self.set_dash_test_params(4, 3, extra_args=self.extra_args)

    def run_test(self):
        self.log.info("Testing protx operations for legacy BLS masternodes after V19 activation")

        # At this point, we have 3 masternodes registered before V19
        # They all have legacy=True (legacy BLS scheme)
        assert len(self.mninfo) == 3
        for mn in self.mninfo:
            assert mn.legacy, "Pre-V19 masternodes should use legacy BLS"
            self.log.info(f"MN {mn.proTxHash[:16]}... has legacy={mn.legacy}")

        # Record the masternode we will revoke
        mn_to_revoke: MasternodeInfo = self.mninfo[0]
        self.log.info(f"Will revoke legacy MN: {mn_to_revoke.proTxHash}")

        # Activate V19
        self.log.info("Activating V19...")
        self.activate_by_name('v19', expected_activation_height=200)
        self.log.info(f"V19 activated at height {self.nodes[0].getblockcount()}")

        # Verify the legacy masternode still exists and is valid
        mn_list = self.nodes[0].protx('list', 'registered', True)
        mn_hashes = [mn['proTxHash'] for mn in mn_list]
        assert mn_to_revoke.proTxHash in mn_hashes, "Legacy MN should still be registered"

        # Test 1: Update service for a legacy BLS masternode after V19
        # This tests that protx update_service works correctly
        mn_to_update: MasternodeInfo = self.mninfo[1]
        self.log.info(f"Updating service for legacy MN {mn_to_update.proTxHash} after V19 activation...")
        self.test_update_service_legacy_mn(mn_to_update)

        # Test 2: Revoke the legacy BLS masternode after V19
        # This is the scenario that was broken - the RPC would create a signature
        # using the legacy scheme (based on MN state) but the validator would
        # verify using the basic scheme (based on global flag), causing failure.
        self.log.info(f"Revoking legacy MN {mn_to_revoke.proTxHash} after V19 activation...")
        self.test_revoke_legacy_mn(mn_to_revoke)

        self.log.info("SUCCESS: Legacy BLS masternode operations work correctly after V19 activation")

    def test_update_service_legacy_mn(self, mn_to_update: MasternodeInfo):
        """
        Test that a legacy BLS masternode can update service after V19 activation.
        This tests the protx_update_service fix.
        """
        # Fund the update transaction
        funds_address = self.nodes[0].getnewaddress()
        fund_txid = self.nodes[0].sendtoaddress(funds_address, 1)
        self.bump_mocktime(10 * 60 + 1)  # Make tx safe to include in block
        tip = self.generate(self.nodes[0], 1)[0]
        assert_equal(self.nodes[0].getrawtransaction(fund_txid, 1, tip)['confirmations'], 1)

        # Update the masternode service - this should succeed with the fix
        protx_result = mn_to_update.update_service(
            self.nodes[0],
            submit=True,
            fundsAddr=funds_address
        )

        self.log.info(f"Update service transaction: {protx_result}")

        # Confirm the update transaction
        self.bump_mocktime(10 * 60 + 1)
        tip = self.generate(self.nodes[0], 1)[0]
        assert_equal(self.nodes[0].getrawtransaction(protx_result, 1, tip)['confirmations'], 1)

        self.log.info(f"Successfully updated service for legacy MN {mn_to_update.proTxHash[:16]}...")

    def test_revoke_legacy_mn(self, mn_to_revoke: MasternodeInfo):
        """
        Test that a legacy BLS masternode can be revoked after V19 activation.
        This is the core regression test.
        """
        # Fund the revoke transaction
        funds_address = self.nodes[0].getnewaddress()
        fund_txid = self.nodes[0].sendtoaddress(funds_address, 1)
        self.bump_mocktime(10 * 60 + 1)  # Make tx safe to include in block
        tip = self.generate(self.nodes[0], 1)[0]
        assert_equal(self.nodes[0].getrawtransaction(fund_txid, 1, tip)['confirmations'], 1)

        # Revoke the masternode - this should succeed with the fix
        # Before the fix, this would fail with "bad-protx-sig"
        protx_result = mn_to_revoke.revoke(
            self.nodes[0],
            submit=True,
            reason=1,  # REASON_TERMINATION_OF_SERVICE
            fundsAddr=funds_address
        )

        self.log.info(f"Revoke transaction: {protx_result}")

        # Confirm the revoke transaction
        self.bump_mocktime(10 * 60 + 1)
        tip = self.generate(self.nodes[0], 1, sync_fun=self.no_op)[0]
        assert_equal(self.nodes[0].getrawtransaction(protx_result, 1, tip)['confirmations'], 1)

        # Handle node disconnects from revoked masternode
        node_idx = mn_to_revoke.nodeIdx
        self.wait_until(lambda: self.nodes[node_idx].getconnectioncount() == 0)
        self.connect_nodes(node_idx, 0)
        self.sync_all()

        # Verify the masternode is now revoked (PoSe banned)
        mn_list = self.nodes[0].protx('list', 'registered', True)
        for mn in mn_list:
            if mn['proTxHash'] == mn_to_revoke.proTxHash:
                # Revoked masternodes have PoSeBanHeight set
                assert mn['state']['PoSeBanHeight'] != -1, "Revoked MN should be PoSe banned"
                self.log.info(f"MN {mn_to_revoke.proTxHash[:16]}... is now PoSe banned at height {mn['state']['PoSeBanHeight']}")
                break

        # Remove from mninfo
        self.mninfo.remove(mn_to_revoke)


if __name__ == '__main__':
    ProtxLegacyBLSTest().main()
