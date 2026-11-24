#!/usr/bin/env python3
# Copyright (c) 2026 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test which files automatic wallet backup pruning deletes from the backups directory."""

import os
from datetime import datetime, timedelta, timezone

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


class WalletAutoBackupTest(BitcoinTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser, descriptors=False)

    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.wallet_names = []
        self.extra_args = [['-createwalletbackups=2', '-maxwalletbackups=4']]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def run_test(self):
        node = self.nodes[0]
        backups_dir = node.chain_path / "backups"
        created = datetime.fromtimestamp(self.mocktime - self.mocktime % 60, timezone.utc)
        loaded = created + timedelta(minutes=1)

        def backup(wallet, **age):
            return f"{wallet}.{loaded - timedelta(**age):%Y-%m-%d-%H-%M}"

        self.log.info("Creating a wallet writes its first backup")
        node.setmocktime(int(created.timestamp()))
        node.createwallet("alice")
        assert_equal(os.listdir(backups_dir), [backup("alice", minutes=1)])
        node.unloadwallet("alice")

        pruned = [
            backup("alice", minutes=30),  # beyond the 2 most recent and under an hour old
            backup("alice", hours=3),  # not the oldest of the 2-4 hour range
            backup("alice", hours=1000),  # -maxwalletbackups leaves room for 2 ranges
        ]
        kept = [
            backup("alice", minutes=1),
            backup("alice", hours=3, minutes=30),
            backup("alice", hours=100),
            backup("alice", days=-1),  # dated after the backup that triggers pruning
            "alice.old",
            backup("bob", hours=2000),
        ]
        for name in pruned + kept:
            (backups_dir / name).touch()

        self.log.info("Loading it writes another backup and prunes only its own timestamped backups")
        node.setmocktime(int(loaded.timestamp()))
        node.loadwallet("alice")
        assert_equal(sorted(os.listdir(backups_dir)), sorted(kept + [backup("alice")]))


if __name__ == '__main__':
    WalletAutoBackupTest().main()
