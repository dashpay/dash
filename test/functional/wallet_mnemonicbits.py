#!/usr/bin/env python3
# Copyright (c) 2023-2025 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test -mnemonicbits wallet option."""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
)

class WalletMnemonicbitsTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def get_mnemonic(self, node):
        if not self.options.descriptors:
            return node.dumphdinfo()["mnemonic"]

        mnemonic = None
        descriptors = node.listdescriptors(True)['descriptors']
        for desc in descriptors:
            if desc['desc'][:4] == 'pkh(':
                if mnemonic is None:
                    mnemonic = desc['mnemonic']
                else:
                    assert_equal(mnemonic, desc['mnemonic'])
            elif desc['desc'][:6] == 'combo(':
                assert 'mnemonic' not in desc
            else:
                raise AssertionError(f"Unknown descriptor type: {desc['desc']}")
        return mnemonic

    def run_test(self):
        self.log.info("Test -mnemonicbits")

        self.log.info("Invalid -mnemonicbits should rise an error")
        self.stop_node(0)
        self.nodes[0].assert_start_raises_init_error(['-mnemonicbits=123'], "Error: Invalid '-mnemonicbits'. Allowed values: 128, 160, 192, 224, 256.")
        self.start_node(0)

        mnemonic_pre = self.get_mnemonic(self.nodes[0])


        self.nodes[0].encryptwallet('pass')
        self.nodes[0].walletpassphrase('pass', 100)
        if self.options.descriptors:
            for desc in self.nodes[0].listdescriptors()['descriptors']:
                assert "mnemonic" not in desc

            mnemonic_count = 0
            cb_count = 0
            descriptors = self.nodes[0].listdescriptors(True)['descriptors']
            for desc in descriptors:
                if 'mnemonic' not in desc:
                    assert not desc['active']
                    # skip imported coinbase private key
                    cb_count += 1
                    continue
                assert_equal(len(desc['mnemonic'].split()), 12)
                mnemonic_count += 1
                assert desc['mnemonic'] == mnemonic_pre
                assert desc['active']
            # there should 3 descriptors in total
            # One of them is inactive imported private key for coinbase. It has no mnemonic
            # Two other should be active and have mnemonic
            assert_equal(mnemonic_count, 2)
            assert_equal(cb_count, 1)
            assert_equal(len(descriptors), 3)
        else:
            assert_equal(len(self.nodes[0].dumphdinfo()["mnemonic"].split()), 12)  # 12 words by default
            # legacy HD wallets could have only one chain
            assert_equal(mnemonic_pre, self.nodes[0].dumphdinfo()["mnemonic"])

        self.log.info("Can have multiple wallets with different mnemonic length loaded at the same time")
        self.restart_node(0, extra_args=["-mnemonicbits=160"])
        self.nodes[0].createwallet("wallet_160")
        self.restart_node(0, extra_args=["-mnemonicbits=192"])
        self.nodes[0].createwallet("wallet_192")
        self.restart_node(0, extra_args=["-mnemonicbits=224"])
        self.nodes[0].createwallet("wallet_224")
        self.restart_node(0, extra_args=["-mnemonicbits=256"])
        self.nodes[0].get_wallet_rpc(self.default_wallet_name).walletpassphrase('pass', 100)
        self.nodes[0].loadwallet("wallet_160")
        self.nodes[0].loadwallet("wallet_192")
        self.nodes[0].loadwallet("wallet_224")
        self.nodes[0].createwallet("wallet_256", blank=True, descriptors=self.options.descriptors)  # blank wallet
        self.nodes[0].get_wallet_rpc("wallet_256").upgradetohd()

        assert_equal(len(self.get_mnemonic(self.nodes[0].get_wallet_rpc(self.default_wallet_name)).split()), 12)  # 12 words by default
        assert_equal(len(self.get_mnemonic(self.nodes[0].get_wallet_rpc("wallet_160")).split()), 15)              # 15 words
        assert_equal(len(self.get_mnemonic(self.nodes[0].get_wallet_rpc("wallet_192")).split()), 18)              # 18 words
        assert_equal(len(self.get_mnemonic(self.nodes[0].get_wallet_rpc("wallet_224")).split()), 21)              # 21 words
        assert_equal(len(self.get_mnemonic(self.nodes[0].get_wallet_rpc("wallet_256")).split()), 24)              # 24 words


if __name__ == '__main__':
    WalletMnemonicbitsTest().main ()
