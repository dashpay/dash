#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test which queue a mixing wallet starts when it could promote or demote a denomination.

A new promotion/demotion queue has to wait for a counterpart doing the same conversion, so a
wallet that still has coins to mix starts a standard queue instead, and only starts the
conversion queue once there is nothing left to mix.
"""

import time
from decimal import Decimal

from test_framework.test_framework import DashTestFramework
from test_framework.util import assert_equal, softfork_active

D1 = Decimal("1.00001")
D01 = Decimal("0.100001")
D001 = Decimal("0.0100001")
COLLATERAL = Decimal("0.0002")
ROUNDS = 2
FULLY_MIXED_ROUNDS = ROUNDS + 3  # see COINJOIN_RANDOM_ROUNDS
V24_HEIGHT = 300


class CoinJoinRebalanceTest(DashTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser)

    def set_test_params(self):
        common = [f"-vbparams=v24:0:999999999999:{V24_HEIGHT}:10:8:6:5:0"]
        # With a goal of 10, eight 1.0 coins and no 0.1 coins make a 1.0 -> 0.1 demotion opportunity
        mixer = common + ["-coinjoinsessions=1", "-coinjoindenomsgoal=10", "-keypool=400", "-createwalletbackups=10"]
        self.set_dash_test_params(5, 3, extra_args=[common, mixer] + [common] * 3)

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def fund_fully_mixed(self, wallet, amount, count):
        """Give wallet `count` coins of `amount` that are fully mixed: zero-fee self-mixing
        transactions mined directly, each one counted as a CoinJoin round."""
        node = self.nodes[0]
        txids = [node.sendtoaddress(wallet.getnewaddress(), amount) for _ in range(count)]
        self.generate(node, 1)
        coins = [{'txid': txid, 'vout': next(o['n'] for o in node.getrawtransaction(txid, True)['vout'] if o['value'] == amount)}
                 for txid in txids]
        for _ in range(FULLY_MIXED_ROUNDS):
            raw = wallet.createrawtransaction(coins, [{wallet.getnewaddress(): amount} for _ in coins])
            tx = wallet.signrawtransactionwithwallet(raw)['hex']
            self.generateblock(node, node.getnewaddress(), [tx])
            txid = wallet.decoderawtransaction(tx)['txid']
            coins = [{'txid': txid, 'vout': n} for n in range(count)]
        wallet.syncwithvalidationinterfacequeue()
        assert all(u['coinjoin_rounds'] >= FULLY_MIXED_ROUNDS for u in wallet.listunspent() if u['amount'] == amount)

    def wait_for_session_denomination(self, wallet, target):
        wallet.setcoinjoinamount(target)
        assert_equal(wallet.coinjoin('start'), "Mixing requested")

        denomination = None

        def session():
            nonlocal denomination
            self.bump_mocktime(1)
            time.sleep(0.1)
            sessions = wallet.getcoinjoininfo()['sessions']
            if sessions:
                denomination = sessions[0]['denomination']
            return denomination is not None
        self.wait_until(session)
        wallet.coinjoin('stop')
        wallet.coinjoin('reset')
        return denomination

    def run_test(self):
        node, wallet = self.nodes[0], self.nodes[1]
        # There are no quorums to InstantSend-lock transactions, which miners would wait for
        node.sporkupdate("SPORK_2_INSTANTSEND_ENABLED", 4070908800)
        self.wait_for_sporks_same()

        # Clients skip masternodes that are next in line for payment
        def all_paid():
            if all(h > 0 for h in node.masternodelist("lastpaidblock").values()):
                return True
            self.generate(node, 1)
            return False
        self.wait_until(all_paid)
        while not softfork_active(node, 'v24'):
            self.generate(node, 10)

        self.fund_fully_mixed(wallet, D1, 8)
        for amount in [D001] * 4 + [COLLATERAL] * 3:
            node.sendtoaddress(wallet.getnewaddress(), amount)
        self.generate(node, 1)
        wallet.syncwithvalidationinterfacequeue()
        wallet.setcoinjoinrounds(ROUNDS)

        self.log.info("A wallet with coins left to mix starts a standard queue for them")
        assert_equal(self.wait_for_session_denomination(wallet, target=20), D001)

        self.log.info("With nothing left to mix, the wallet starts the demotion queue")
        assert_equal(self.wait_for_session_denomination(wallet, target=2), D01)
        unmixed = [{'txid': u['txid'], 'vout': u['vout']} for u in wallet.listunspent() if u['amount'] == D001]
        assert_equal(len(unmixed), 4)
        wallet.lockunspent(False, unmixed)
        assert_equal(self.wait_for_session_denomination(wallet, target=20), D01)


if __name__ == '__main__':
    CoinJoinRebalanceTest().main()
