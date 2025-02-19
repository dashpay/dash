#!/usr/bin/env python3
# Copyright (c) 2016 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""
wallet_upgradetohd.py

Test upgrade to a Hierarchical Deterministic wallet via upgradetohd rpc
"""

import shutil
import os

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)


class WalletUpgradeToHDTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def recover_blank(self):
        self.log.info("Recover non-HD wallet to check different upgrade paths")
        node = self.nodes[0]
        self.stop_node(0)
        shutil.copyfile(os.path.join(node.datadir, "blank.bak"), os.path.join(node.datadir, self.chain, self.default_wallet_name, self.wallet_data_filename))
        self.start_node(0)
        assert 'hdchainid' not in node.get_wallet_rpc("w-1").getwalletinfo()

    def run_test(self):
        node = self.nodes[0]
        node.createwallet("w-1", False, True, "", False, True, True)

        wallet = self.nodes[0].get_wallet_rpc(self.default_wallet_name)
        w1 = self.nodes[0].get_wallet_rpc("w-1")
        w1.backupwallet(os.path.join(node.datadir, "blank.bak"))


        self.log.info("No mnemonic, no mnemonic passphrase, no wallet passphrase")
        assert_equal(len(w1.listdescriptors()['descriptors']), 0)
        balance_before = w1.getbalance()
        balance_non_HD = wallet.getbalance()
        assert w1.upgradetohd()
        mnemonic = w1.listdescriptors(True)['descriptors'][0]['mnemonic']
        desc = w1.listdescriptors()['descriptors'][0]['desc']
        assert_equal(len(desc), 149)
        assert_equal(balance_before, w1.getbalance())

        self.log.info("Should be spendable and should use correct paths")
        for i in range(5):
            txid = wallet.sendtoaddress(w1.getnewaddress(), 1)
            self.sync_all()
            outs = node.decoderawtransaction(w1.gettransaction(txid)['hex'])['vout']
            for out in outs:
                if out['value'] == 1:
                    keypath =w1.getaddressinfo(out['scriptPubKey']['address'])['hdkeypath']
                    assert_equal(keypath, "m/44'/1'/0'/0/%d" % i)
                else:
                    # change doesn't belong to descriptor wallet, skip it
                    pass

        self.bump_mocktime(1)
        self.generate(node, 1)

        self.log.info("Should no longer be able to start it with HD disabled")
        self.stop_node(0)
        node.assert_start_raises_init_error(['-usehd=0'], "Error: Error loading %s: You can't disable HD on an already existing HD wallet" % self.default_wallet_name)
        self.extra_args = []
        self.start_node(0, [])
      
        wallet = self.nodes[0].get_wallet_rpc(self.default_wallet_name)
        w1 = self.nodes[0].get_wallet_rpc("w-1")
        balance_after = w1.getbalance()

        self.recover_blank()

        # We spent some coins from default wallet to descriptorwallet earlier
        assert balance_non_HD != wallet.getbalance()
        balance_non_HD = wallet.getbalance()

        self.log.info("No mnemonic, no mnemonic passphrase, no wallet passphrase, should result in completely different keys")
        assert node.upgradetohd()
        assert mnemonic != node.dumphdinfo()['mnemonic']
        assert desc != node.getwalletinfo()['hdchainid']
        assert_equal(balance_non_HD, node.getbalance())
        node.keypoolrefill(5)
        node.rescanblockchain()
        # Completely different keys, no HD coins should be recovered
        assert_equal(balance_non_HD, node.getbalance())

        self.recover_blank()

        self.log.info("No mnemonic, no mnemonic passphrase, no wallet passphrase, should result in completely different keys")
        self.restart_node(0, extra_args=['-keypool=10'])
        assert node.upgradetohd("", "", "", True)
        # Completely different keys, no HD coins should be recovered
        assert mnemonic != node.dumphdinfo()['mnemonic']
        assert desc != node.getwalletinfo()['hdchainid']
        assert_equal(balance_non_HD, node.getbalance())

        self.recover_blank()

        self.log.info("Same mnemonic, another mnemonic passphrase, no wallet passphrase, should result in a different set of keys")
        new_mnemonic_passphrase = "somewords"
        assert node.upgradetohd(mnemonic, new_mnemonic_passphrase)
        assert_equal(mnemonic, node.dumphdinfo()['mnemonic'])
        new_chainid = node.getwalletinfo()['hdchainid']
        assert desc != new_chainid
        assert_equal(balance_non_HD, node.getbalance())
        node.keypoolrefill(5)
        node.rescanblockchain()
        # A different set of keys, no HD coins should be recovered
        new_addresses = (node.getnewaddress(), node.getrawchangeaddress())
        assert_equal(balance_non_HD, node.getbalance())

        self.recover_blank()

        self.log.info("Same mnemonic, another mnemonic passphrase, no wallet passphrase, should result in a different set of keys (again)")
        assert node.upgradetohd(mnemonic, new_mnemonic_passphrase)
        assert_equal(mnemonic, node.dumphdinfo()['mnemonic'])
        assert_equal(new_chainid, node.getwalletinfo()['hdchainid'])
        assert_equal(balance_non_HD, node.getbalance())
        node.keypoolrefill(5)
        node.rescanblockchain()
        # A different set of keys, no HD coins should be recovered, keys should be the same as they were the previous time
        assert_equal(new_addresses, (node.getnewaddress(), node.getrawchangeaddress()))
        assert_equal(balance_non_HD, node.getbalance())

        self.recover_blank()

        self.log.info("Same mnemonic, no mnemonic passphrase, no wallet passphrase, should recover all coins after rescan")
        assert node.upgradetohd(mnemonic)
        assert_equal(mnemonic, node.dumphdinfo()['mnemonic'])
        assert_equal(desc, node.getwalletinfo()['hdchainid'])
        node.keypoolrefill(5)
        assert balance_after != node.getbalance()
        node.rescanblockchain()
        assert_equal(balance_after, node.getbalance())

        self.recover_blank()

        self.log.info("Same mnemonic, no mnemonic passphrase, no wallet passphrase, large enough keepool, should recover all coins with no extra rescan")
        self.restart_node(0, extra_args=['-keypool=10'])
        assert node.upgradetohd(mnemonic)
        assert_equal(mnemonic, node.dumphdinfo()['mnemonic'])
        assert_equal(desc, node.getwalletinfo()['hdchainid'])
        # All coins should be recovered
        assert_equal(balance_after, node.getbalance())

        self.recover_blank()

        self.log.info("Same mnemonic, no mnemonic passphrase, no wallet passphrase, large enough keepool, rescan is skipped initially, should recover all coins after rescanblockchain")
        self.restart_node(0, extra_args=['-keypool=10'])
        assert node.upgradetohd(mnemonic, "", "", False)
        assert_equal(mnemonic, node.dumphdinfo()['mnemonic'])
        assert_equal(desc, node.getwalletinfo()['hdchainid'])
        assert balance_after != node.getbalance()
        node.rescanblockchain()
        # All coins should be recovered
        assert_equal(balance_after, node.getbalance())

        self.recover_blank()

        self.log.info("Same mnemonic, same mnemonic passphrase, encrypt wallet on upgrade, should recover all coins after rescan")
        walletpass = "111pass222"
        assert node.upgradetohd(mnemonic, "", walletpass)
        node.stop()
        node.wait_until_stopped()
        self.start_node(0, extra_args=['-rescan'])
        assert_raises_rpc_error(-13, "Error: Please enter the wallet passphrase with walletpassphrase first.", node.dumphdinfo)
        node.walletpassphrase(walletpass, 100)
        assert_equal(mnemonic, node.dumphdinfo()['mnemonic'])
        assert_equal(desc, node.getwalletinfo()['hdchainid'])
        # Note: wallet encryption results in additional keypool topup,
        # so we can't compare new balance to balance_non_HD here,
        # assert_equal(balance_non_HD, node.getbalance())  # won't work
        assert balance_non_HD != node.getbalance()
        node.keypoolrefill(4)
        node.rescanblockchain()
        # All coins should be recovered
        assert_equal(balance_after, node.getbalance())

        self.recover_blank()

        self.log.info("Same mnemonic, same mnemonic passphrase, encrypt wallet first, should recover all coins on upgrade after rescan")
        walletpass = "111pass222"
        node.encryptwallet(walletpass)
        node.stop()
        node.wait_until_stopped()
        self.start_node(0, extra_args=['-rescan'])
        assert_raises_rpc_error(-13, "Error: Wallet encrypted but passphrase not supplied to RPC.", node.upgradetohd, mnemonic)
        assert_raises_rpc_error(-1,  "Error: The wallet passphrase entered was incorrect", node.upgradetohd, mnemonic, "", "wrongpass")
        assert node.upgradetohd(mnemonic, "", walletpass)
        assert_raises_rpc_error(-13, "Error: Please enter the wallet passphrase with walletpassphrase first.", node.dumphdinfo)
        node.walletpassphrase(walletpass, 100)
        assert_equal(mnemonic, node.dumphdinfo()['mnemonic'])
        assert_equal(desc, node.getwalletinfo()['hdchainid'])
        # Note: wallet encryption results in additional keypool topup,
        # so we can't compare new balance to balance_non_HD here,
        # assert_equal(balance_non_HD, node.getbalance())  # won't work
        assert balance_non_HD != node.getbalance()
        node.keypoolrefill(4)
        node.rescanblockchain()
        # All coins should be recovered
        assert_equal(balance_after, node.getbalance())


if __name__ == '__main__':
    WalletUpgradeToHDTest().main ()
