#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test CoinJoin wallet unload and stop/reset cleanup in a shared session.

Two wallets join the same masternode queue. Reset and unload one participant
while the session is live, then require the survivor to report ERROR and verify
its cleanup. Queue timeouts are advanced with mocktime.
"""

from decimal import Decimal

from test_framework.test_framework import DashTestFramework
from test_framework.util import assert_equal, assert_greater_than

MASTERNODES = 3


class WalletCoinJoinMixingTest(DashTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser)

    def set_test_params(self):
        # CoinJoin requires automatic backups for legacy wallets. One session
        # per wallet keeps both participants in the same masternode queue.
        mixing_args = ["-debug=coinjoin", "-createwalletbackups=1", "-keypool=400", "-coinjoinsessions=1"]
        extra_args = [["-debug=coinjoin"], mixing_args, mixing_args] + [["-debug=coinjoin"]] * MASTERNODES
        self.set_dash_test_params(3 + MASTERNODES, MASTERNODES, extra_args=extra_args)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        self.w1, self.w2 = self.nodes[1:3]
        self.prepare_chain()
        self.log.info("Fund two mixing wallets with confirmed denominations and collaterals")
        for wallet in (self.w1, self.w2):
            outputs = {wallet.getnewaddress(): Decimal("1.00001000") for _ in range(8)}
            outputs.update({wallet.getnewaddress(): Decimal("0.00020000") for _ in range(2)})
            self.nodes[0].sendmany("", outputs)
            wallet.setcoinjoinamount(2)
            wallet.setcoinjoinrounds(2)
        self.bump_mocktime(1)
        self.generate(self.nodes[0], 1)

        self.log.info("Join a shared session and isolate the second participant")
        server = self.start_shared_session()
        for wallet in (self.w1, self.w2):
            info = wallet.getcoinjoininfo()
            assert_equal(info['running'], True)
            assert_equal(len(info['sessions']), 1)
            assert_greater_than(len(wallet.listlockunspent()), 0)

        # Test reset-driven unlocking on this live session, not on an empty
        # client or one whose timeout has already unlocked its coins.
        self.stop_and_reset(self.w2)
        self.log.info("Unload and reload the participant while the survivor is still mixing")
        self.nodes[2].unloadwallet(self.default_wallet_name)
        self.nodes[2].loadwallet(self.default_wallet_name)
        self.w2 = self.nodes[2].get_wallet_rpc(self.default_wallet_name)
        assert_equal(self.w2.getcoinjoininfo()['sessions'], [])
        assert_equal(self.w2.listlockunspent(), [])
        self.nodes[2].setnetworkactive(True)
        self.connect_nodes(2, 0)
        self.connect_nodes(2, server.index)

        self.log.info("Require the survivor to report an interrupted session")

        def survivor_reports_error():
            if any(session['state'] == 'ERROR' for session in self.w1.getcoinjoininfo()['sessions']):
                return True
            self.bump_mocktime(1)
            return False

        self.wait_until(survivor_reports_error, timeout=120)
        self.stop_and_reset(self.w1)

    def stop_and_reset(self, wallet):
        wallet.coinjoin('stop')
        assert_equal(wallet.getcoinjoininfo()['running'], False)
        assert_equal(wallet.coinjoin('reset'), "Mixing was reset")
        assert_equal(wallet.getcoinjoininfo()['sessions'], [])
        assert_equal(wallet.listlockunspent(), [])

    def prepare_chain(self):
        # There are no InstantSend quorums here, but the framework enables the
        # InstantSend spork by default, which makes the miner hold back
        # non-locked transactions for 10 minutes. CoinJoin does not need
        # InstantSend, so turn it off.
        self.nodes[0].sporkupdate("SPORK_2_INSTANTSEND_ENABLED", 4070908800)
        self.wait_for_sporks_same()

        self.log.info("Make sure every masternode has been paid at least once")

        # CCoinJoinClientSession skips masternodes that are next in the
        # payment queue, which on regtest means masternodes that have never
        # been paid would never be picked for mixing.
        def all_masternodes_paid():
            if all(h > 0 for h in self.nodes[0].masternodelist("lastpaidblock").values()):
                return True
            self.bump_mocktime(1)
            self.generate(self.nodes[0], 1, sync_fun=lambda: self.sync_blocks())
            return False
        self.wait_until(all_masternodes_paid, timeout=60)

    def start_shared_session(self):
        # The initial DSQ is announced only to peers already connected to the
        # selected masternode, so connect the joiner before starting the queue.
        for mn in self.mninfo:
            self.connect_nodes(2, mn.nodeIdx)
        self.w1.coinjoin('start')

        def starter_has_queue():
            sessions = self.w1.getcoinjoininfo()['sessions']
            if sessions and sessions[0].get('protxhash'):
                mn = self.get_mninfo(sessions[0]['protxhash'])
                assert mn is not None
                if mn.get_node(self).getcoinjoininfo()['state'] == 'QUEUE':
                    return True
            self.bump_mocktime(1)
            return False

        self.wait_until(starter_has_queue, timeout=120)
        protxhash = self.w1.getcoinjoininfo()['sessions'][0]['protxhash']
        mn = self.get_mninfo(protxhash)
        assert mn is not None and mn.nodeIdx is not None
        server = mn.get_node(self)

        def joiner_has_queue():
            if self.w2.getcoinjoininfo()['queue_size'] > 0:
                return True
            self.bump_mocktime(1)
            return False

        self.wait_until(joiner_has_queue, timeout=120)
        # This log is emitted only after the second collateral is accepted,
        # unlike the client's QUEUE state which precedes its pending DSACCEPT.
        # Keep the server's mocktime fixed until the joiner is disconnected.
        with server.busy_wait_for_debug_log([b'CCoinJoinServer::AddUserToExistingSession -- new user accepted,']):
            self.w2.coinjoin('start')

            def joiner_selected_queue():
                sessions = self.w2.getcoinjoininfo()['sessions']
                if sessions and sessions[0].get('protxhash') == protxhash:
                    assert_equal(sessions[0]['state'], 'QUEUE')
                    return True
                self.bump_mocktime(1, nodes=[self.w2])
                return False

            self.wait_until(joiner_selected_queue, timeout=120)
            self.bump_mocktime(3, nodes=[self.w2])
        self.isolate_node(2)

        def server_accepting_entries():
            if server.getcoinjoininfo()['state'] == 'ACCEPTING_ENTRIES':
                return True
            self.bump_mocktime(1, nodes=[node for node in self.nodes if node != self.w2])
            return False

        self.wait_until(server_accepting_entries, timeout=120)
        assert_equal(self.w2.getcoinjoininfo()['sessions'][0]['entries_count'], 0)
        return server


if __name__ == '__main__':
    WalletCoinJoinMixingTest().main()
