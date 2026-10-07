#!/usr/bin/env python3
# Copyright (c) 2016-2025 The Dash Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""
wallet_upgradetohd.py

Test upgrade to a Hierarchical Deterministic wallet via upgradetohd rpc
"""

import os
import re
import shutil

from test_framework.test_framework import BitcoinTestFramework
from test_framework.test_node import ErrorMatch
from test_framework.util import (
    append_config,
    assert_equal,
    assert_raises_rpc_error,
    get_mnemonic,
)


class WalletUpgradeToHDTest(BitcoinTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser)

    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [['-usehd=0']]

    def skip_test_if_missing_module(self):
        self.skip_if_no_wallet()

    def setup_network(self):
        self.add_nodes(self.num_nodes, self.extra_args)
        self.start_nodes()
        self.nodes[0].createwallet(self.default_wallet_name, blank=True, load_on_startup=True)
        self.nodes[0].importprivkey(privkey=self.nodes[0].get_deterministic_priv_key().key, label='coinbase', rescan=True)

    def recover_non_hd(self):
        self.log.info("Recover non-HD wallet to check different upgrade paths")
        node = self.nodes[0]
        self.stop_node(0)
        shutil.copyfile(os.path.join(node.datadir, "non_hd.bak"), os.path.join(node.datadir, self.chain, self.default_wallet_name, self.wallet_data_filename))
        self.start_node(0)
        if not self.options.descriptors:
            assert 'hdchainid' not in node.getwalletinfo()

    def test_mnemonic_passphrase_limit(self):
        self.log.info("New mnemonic seeds reject passphrases beyond 248 UTF-8 bytes")
        node = self.nodes[0]
        node.createwallet("mnemonic-limit", blank=True, descriptors=self.options.descriptors)
        wallet = node.get_wallet_rpc("mnemonic-limit")
        key = node.get_deterministic_priv_key()
        wallet.importprivkey(key.key)
        wallet.encryptwallet("limit passphrase")
        wallet.walletpassphrase("limit passphrase", 100)
        mnemonic = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about"
        for phrase in ("x" * 249, "x" * 247 + "é"):
            for words in (mnemonic, ""):
                assert_raises_rpc_error(-1, "at most 248 bytes", wallet.upgradetohd, words, phrase, "limit passphrase", False)
                signature = wallet.signmessage(key.address, "preflight retained unlock")
                assert node.verifymessage(key.address, signature, "preflight retained unlock")
        for words in ("", "invalid mnemonic"):
            assert_raises_rpc_error(-1, "requires a supplied valid mnemonic", wallet.upgradetohd, words, "x" * 249, "limit passphrase", False, True)
        if not self.options.descriptors:
            assert_raises_rpc_error(-1, "at most 256 bytes", wallet.upgradetohd, mnemonic, "x" * 257, "limit passphrase", False, True)
        wallet.walletlock()
        wallet.unloadwallet()
        node.createwallet("mnemonic-cli", blank=True, descriptors=self.options.descriptors)
        result = node.cli("-rpcwallet=mnemonic-cli").upgradetohd(mnemonic, "x" * 249, "", False, True)
        assert "ignores passphrase bytes after byte 248" in result
        cli_wallet = node.get_wallet_rpc("mnemonic-cli")
        assert_equal(cli_wallet.getnewaddress(), "yiMSVRFwgJSg6nXWKcKgvRU3ZWWXkm1NWo")
        cli_wallet.unloadwallet()

    def test_mnemonic_recovery(self):
        self.log.info("Explicit legacy recovery retains historical addresses, metadata and encrypted backups")
        node = self.nodes[0]
        # restorewallet refuses HD wallets under this test's default -usehd=0
        self.restart_node(0, ["-usehd=1"])
        mnemonic = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about"
        capped = ("yiMSVRFwgJSg6nXWKcKgvRU3ZWWXkm1NWo", "yS354oVpKrfjVG5XunAd74bpo3NEBHVvhf", "yT1eiqTpx9DH6bUcwM3Ddz22aAm5DyHG4L", "yWYeNis2QHyrQ4fA8wUF3xjLRRhq3qaGkA")
        split = ("ydtZKPA4grhS6MRMK14vtwPGb3Hp31RbK1", "yLj9BPyyShy21HVtyGGGz98WQV6jMTxBDL", "ydRgLSVCPkGZCUswXTiWpHuuKqzhGjdJf8", "yiC7BpzebSp617ArhQVfUdFptytkMhBhYy")
        normal = ("ya4hijmdfG9gtJHZoDWhedMYT8da2e3zu5", "ycm9zhUpAXgKrV1ekikATSsJ3UieLcBAZe", "ybS9iPvuafNHn2gexVDLBr1NQq6CddqHhR", "ydMQAaVEf2vkSHFkL2Jf3sU4qxc5Cvby1z")
        cases = [("boundary", "x" * 248, capped), ("suffix-x", "x" * 248 + "X", capped), ("suffix-y", "x" * 248 + "Y", capped), ("split", "x" * 247 + "é", split), ("normal", "TREZOR", normal), ("legacy-max", "x" * 256, capped)]
        if self.options.descriptors:
            cases.append(("descriptor-long", "x" * 300, capped))
        for label, phrase, addresses in cases:
            name = "recovery-" + label
            node.createwallet(name, blank=True, descriptors=self.options.descriptors)
            wallet = node.get_wallet_rpc(name)
            truncated = len(phrase.encode("utf8")) > 248
            result = wallet.upgradetohd(mnemonic, phrase, "", False, truncated)
            assert ("ignores passphrase bytes after byte 248" in result) == truncated
            assert_equal(wallet.getnewaddress(), addresses[0])
            assert_equal(wallet.getrawchangeaddress(), addresses[1])
            assert_equal(get_mnemonic(wallet), (mnemonic, phrase))
            wallet.encryptwallet("recovery wallet passphrase")
            wallet.walletpassphrase("recovery wallet passphrase", 100)
            signature = wallet.signmessage(addresses[0], "encrypted recovery")
            assert node.verifymessage(addresses[0], signature, "encrypted recovery")
            assert_equal(get_mnemonic(wallet), (mnemonic, phrase))
            wallet.walletlock()
            backup = os.path.join(self.options.tmpdir, name + ".backup")
            wallet.backupwallet(backup)
            wallet.unloadwallet()
            restored_name = name + "-restored"
            node.restorewallet(restored_name, backup)
            restored = node.get_wallet_rpc(restored_name)
            restored.walletpassphrase("recovery wallet passphrase", 100)
            assert_equal(get_mnemonic(restored), (mnemonic, phrase))
            signature = restored.signmessage(addresses[0], "restored recovery")
            assert node.verifymessage(addresses[0], signature, "restored recovery")
            assert_equal(restored.getnewaddress(), addresses[2])
            assert_equal(restored.getrawchangeaddress(), addresses[3])
            if not self.options.descriptors and truncated:
                restored.migratewallet(passphrase="recovery wallet passphrase")
                restored = node.get_wallet_rpc(restored_name)
                restored.walletpassphrase("recovery wallet passphrase", 100)
                for desc in restored.listdescriptors(True)["descriptors"]:
                    assert_equal((desc["mnemonic"], desc["mnemonicpassphrase"]), (mnemonic, phrase))
                assert all(restored.getaddressinfo(address)["ismine"] for address in addresses)
                signature = restored.signmessage(addresses[0], "migrated recovery")
                assert node.verifymessage(addresses[0], signature, "migrated recovery")
            restored.walletlock()
            restored.unloadwallet()

    def test_startup_mnemonic_recovery(self):
        self.log.info("Startup mnemonic options reject new long passphrases and recover historical addresses")
        node = self.nodes[0]
        mnemonic = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about"
        args = ["-usehd=1", "-mnemonic=" + mnemonic, "-mnemonicpassphrase=" + "x" * 249]
        for label, words in (("supplied", mnemonic), ("generated", "")):
            for encoding, phrase in (("ascii", "x" * 249), ("utf8", "x" * 247 + "é")):
                self.restart_node(0, ["-usehd=1", "-mnemonic=" + words, "-mnemonicpassphrase=" + phrase])
                name = "startup-limit-" + label + "-" + encoding
                assert_raises_rpc_error(-4, "at most 248 bytes", node.createwallet, name, descriptors=self.options.descriptors)
                assert not os.path.exists(os.path.join(node.datadir, self.chain, name))
        for label, words in (("missing", ""), ("invalid", "invalid mnemonic")):
            self.restart_node(0, ["-usehd=1", "-mnemonic=" + words, "-mnemonicpassphrase=" + "x" * 249, "-allowlegacymnemonicpassphrase=1"])
            name = "startup-invalid-recovery-" + label
            assert_raises_rpc_error(-4, "requires a supplied valid mnemonic", node.createwallet, name, descriptors=self.options.descriptors)
            assert not os.path.exists(os.path.join(node.datadir, self.chain, name))
        if not self.options.descriptors:
            self.restart_node(0, ["-usehd=1", "-mnemonic=" + mnemonic, "-mnemonicpassphrase=" + "x" * 257, "-allowlegacymnemonicpassphrase=1"])
            assert_raises_rpc_error(-4, "at most 256 bytes", node.createwallet, "startup-legacy-too-long", descriptors=False)
            assert not os.path.exists(os.path.join(node.datadir, self.chain, "startup-legacy-too-long"))
        self.restart_node(0, args + ["-allowlegacymnemonicpassphrase=1"])
        result = node.createwallet("startup-recovery", descriptors=self.options.descriptors, load_on_startup=True)
        assert_equal(sum("ignores passphrase bytes after byte 248" in warning for warning in result["warnings"]), 1)
        wallet = node.get_wallet_rpc("startup-recovery")
        assert_equal(wallet.getnewaddress(), "yiMSVRFwgJSg6nXWKcKgvRU3ZWWXkm1NWo")
        assert_equal(wallet.getrawchangeaddress(), "yS354oVpKrfjVG5XunAd74bpo3NEBHVvhf")
        assert_equal(get_mnemonic(wallet), (mnemonic, "x" * 249))
        self.restart_node(0, ["-usehd=1"])
        wallet = node.get_wallet_rpc("startup-recovery")
        assert_equal(get_mnemonic(wallet), (mnemonic, "x" * 249))
        assert_equal(wallet.getnewaddress(), "yT1eiqTpx9DH6bUcwM3Ddz22aAm5DyHG4L")
        wallet.unloadwallet()

        if not self.options.descriptors:
            self.log.info("Actual incomplete legacy wallet startup rejects new long passphrases and recovers with explicit opt-in")
            self.restart_node(0, ["-usehd=1", "-mnemonic=invalid mnemonic"])
            assert_raises_rpc_error(-1, "invalid mnemonic", node.createwallet, "startup-incomplete", descriptors=False)
            self.stop_node(0)
            node.assert_start_raises_init_error(extra_args=args + ["-wallet=startup-incomplete"], expected_msg=re.escape("Mnemonic passphrase is too long, must be at most 248 bytes"), match=ErrorMatch.PARTIAL_REGEX)
            with node.assert_debug_log(["Legacy mnemonic recovery ignores passphrase bytes after byte 248"]):
                self.start_node(0, args + ["-wallet=startup-incomplete", "-allowlegacymnemonicpassphrase=1"])
            wallet = node.get_wallet_rpc("startup-incomplete")
            assert_equal(wallet.getnewaddress(), "yiMSVRFwgJSg6nXWKcKgvRU3ZWWXkm1NWo")
            assert_equal(wallet.getrawchangeaddress(), "yS354oVpKrfjVG5XunAd74bpo3NEBHVvhf")
            assert_equal(get_mnemonic(wallet), (mnemonic, "x" * 249))
            self.stop_node(0, expected_stderr="Warning: Legacy mnemonic recovery ignores passphrase bytes after byte 248. This option is only for recovering an existing wallet.")

    def test_config_section_recovery_opt_in(self):
        self.log.info("A recovery opt-in from the network section of the config file lasts until seed generation")
        node = self.nodes[0]
        mnemonic = "abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about"
        conf_path = os.path.join(node.datadir, "dash.conf")
        with open(conf_path, encoding="utf8") as f:
            conf = f.read()
        append_config(node.datadir, ["allowlegacymnemonicpassphrase=1"])
        self.restart_node(0, ["-usehd=1", "-mnemonic=" + mnemonic, "-mnemonicpassphrase=" + "x" * 249])
        result = node.createwallet("config-recovery", descriptors=self.options.descriptors)
        assert_equal(sum("ignores passphrase bytes after byte 248" in warning for warning in result["warnings"]), 1)
        wallet = node.get_wallet_rpc("config-recovery")
        assert_equal(get_mnemonic(wallet), (mnemonic, "x" * 249))
        assert_equal(wallet.getnewaddress(), "yiMSVRFwgJSg6nXWKcKgvRU3ZWWXkm1NWo")
        assert_equal(wallet.getrawchangeaddress(), "yS354oVpKrfjVG5XunAd74bpo3NEBHVvhf")
        wallet.unloadwallet()
        self.restart_node(0, ["-usehd=1", "-mnemonicpassphrase=" + "x" * 249])
        assert_raises_rpc_error(-4, "requires a supplied valid mnemonic", node.createwallet, "config-recovery-missing", descriptors=self.options.descriptors)
        assert not os.path.exists(os.path.join(node.datadir, self.chain, "config-recovery-missing"))
        self.stop_node(0)
        with open(conf_path, "w", encoding="utf8") as f:
            f.write(conf)

    def run_test(self):
        node = self.nodes[0]
        node.backupwallet(os.path.join(node.datadir, "non_hd.bak"))

        self.log.info("No mnemonic, no mnemonic passphrase, no wallet passphrase")
        assert 'hdchainid' not in node.getwalletinfo()
        balance_before = node.getbalance()
        assert node.upgradetohd()
        mnemonic = get_mnemonic(node)
        if not self.options.descriptors:
            chainid = node.getwalletinfo()['hdchainid']
            assert_equal(len(chainid), 64)
        assert_equal(balance_before, node.getbalance())

        self.log.info("Should be spendable and should use correct paths")
        for i in range(5):
            txid = node.sendtoaddress(node.getnewaddress(), 1)
            outs = node.decoderawtransaction(node.gettransaction(txid)['hex'])['vout']
            for out in outs:
                if out['value'] == 1:
                    keypath = node.getaddressinfo(out['scriptPubKey']['address'])['hdkeypath']
                    if self.options.descriptors:
                        assert_equal(keypath, "m/44h/1h/0h/0/%d" % i)
                    else:
                        assert_equal(keypath, "m/44'/1'/0'/0/%d" % i)
                else:
                    keypath = node.getaddressinfo(out['scriptPubKey']['address'])['hdkeypath']
                    if self.options.descriptors:
                        assert_equal(keypath, "m/44h/1h/0h/1/%d" % i)
                    else:
                        assert_equal(keypath, "m/44'/1'/0'/1/%d" % i)

        self.bump_mocktime(1)
        self.generate(node, 1, sync_fun=self.no_op)

        self.log.info("Should no longer be able to start it with HD disabled")
        self.stop_node(0)
        node.assert_start_raises_init_error(['-usehd=0'], "Error: Error loading %s: You can't disable HD on an already existing HD wallet" % self.default_wallet_name)
        self.extra_args = []
        self.start_node(0, [])
        balance_after = node.getbalance()

        self.recover_non_hd()

        # We spent some coins from non-HD keys to HD ones earlier
        balance_non_HD = node.getbalance()
        assert balance_before != balance_non_HD

        self.log.info("No mnemonic, no mnemonic passphrase, no wallet passphrase, should result in completely different keys")
        assert node.upgradetohd()
        assert mnemonic != get_mnemonic(node)
        if not self.options.descriptors:
            assert chainid != node.getwalletinfo()['hdchainid']
        assert_equal(balance_non_HD, node.getbalance())
        node.keypoolrefill(5)
        node.rescanblockchain()
        # Completely different keys, no HD coins should be recovered
        assert_equal(balance_non_HD, node.getbalance())

        self.recover_non_hd()

        self.log.info("No mnemonic, no mnemonic passphrase, no wallet passphrase, should result in completely different keys")
        self.restart_node(0, extra_args=['-keypool=10'])
        assert node.upgradetohd("", "", "", True)
        # Completely different keys, no HD coins should be recovered
        assert mnemonic != get_mnemonic(node)
        if not self.options.descriptors:
            assert chainid != node.getwalletinfo()['hdchainid']
        assert_equal(balance_non_HD, node.getbalance())

        self.recover_non_hd()

        self.log.info("Same mnemonic, another mnemonic passphrase, no wallet passphrase, should result in a different set of keys")
        new_mnemonic_passphrase = "somewords"
        assert node.upgradetohd(mnemonic[0], new_mnemonic_passphrase)
        assert_equal(mnemonic[0], get_mnemonic(node)[0])
        assert_equal(new_mnemonic_passphrase, get_mnemonic(node)[1])
        if not self.options.descriptors:
            new_chainid = node.getwalletinfo()['hdchainid']
            assert chainid != new_chainid
        assert_equal(balance_non_HD, node.getbalance())
        node.keypoolrefill(5)
        node.rescanblockchain()
        # A different set of keys, no HD coins should be recovered
        new_addresses = (node.getnewaddress(), node.getrawchangeaddress())
        assert_equal(balance_non_HD, node.getbalance())

        self.recover_non_hd()

        self.log.info("Same mnemonic, another mnemonic passphrase, no wallet passphrase, should result in a different set of keys (again)")
        assert node.upgradetohd(mnemonic[0], new_mnemonic_passphrase)
        assert_equal(mnemonic[0], get_mnemonic(node)[0])
        assert_equal(new_mnemonic_passphrase, get_mnemonic(node)[1])
        if not self.options.descriptors:
            assert_equal(new_chainid, node.getwalletinfo()['hdchainid'])
        assert_equal(balance_non_HD, node.getbalance())
        node.keypoolrefill(5)
        node.rescanblockchain()
        # A different set of keys, no HD coins should be recovered, keys should be the same as they were the previous time
        assert_equal(new_addresses, (node.getnewaddress(), node.getrawchangeaddress()))
        assert_equal(balance_non_HD, node.getbalance())

        self.recover_non_hd()

        self.log.info("Same mnemonic, no mnemonic passphrase, no wallet passphrase, should recover all coins after rescan")
        assert node.upgradetohd(mnemonic[0], mnemonic[1])
        assert_equal(mnemonic, get_mnemonic(node))
        if not self.options.descriptors:
            assert_equal(chainid, node.getwalletinfo()['hdchainid'])
        node.keypoolrefill(5)
        assert balance_after != node.getbalance()
        node.rescanblockchain()
        assert_equal(balance_after, node.getbalance())

        self.recover_non_hd()

        self.log.info("Same mnemonic, no mnemonic passphrase, no wallet passphrase, large enough keepool, should recover all coins with no extra rescan")
        self.restart_node(0, extra_args=['-keypool=10'])
        assert node.upgradetohd(mnemonic[0], mnemonic[1])
        assert_equal(mnemonic, get_mnemonic(node))
        if not self.options.descriptors:
            assert_equal(chainid, node.getwalletinfo()['hdchainid'])
        # All coins should be recovered
        assert_equal(balance_after, node.getbalance())

        self.recover_non_hd()

        self.log.info("Same mnemonic, no mnemonic passphrase, no wallet passphrase, large enough keepool, rescan is skipped initially, should recover all coins after rescanblockchain")
        self.restart_node(0, extra_args=['-keypool=10'])
        assert node.upgradetohd(mnemonic[0], mnemonic[1], "", False)
        assert_equal(mnemonic, get_mnemonic(node))
        if not self.options.descriptors:
            assert_equal(chainid, node.getwalletinfo()['hdchainid'])
        assert balance_after != node.getbalance()
        node.rescanblockchain()
        # All coins should be recovered
        assert_equal(balance_after, node.getbalance())

        self.recover_non_hd()

        self.log.info("Same mnemonic, same mnemonic passphrase, encrypt wallet on upgrade, should recover all coins after rescan")
        walletpass = "111pass222"
        assert node.upgradetohd(mnemonic[0], "", walletpass)
        assert_raises_rpc_error(-13, "Error: Please enter the wallet passphrase with walletpassphrase first.", node.rescanblockchain)
        assert_raises_rpc_error(-13, "Error: Please enter the wallet passphrase with walletpassphrase first.", node.dumphdinfo)
        node.walletpassphrase(walletpass, 100)
        assert_equal(mnemonic, get_mnemonic(node))
        if not self.options.descriptors:
            assert_equal(chainid, node.getwalletinfo()['hdchainid'])
        # Note: wallet encryption results in additional keypool topup,
        # so we can't compare new balance to balance_non_HD here,
        # assert_equal(balance_non_HD, node.getbalance())  # won't work
        assert balance_non_HD != node.getbalance()
        node.keypoolrefill(4)
        node.rescanblockchain()
        # All coins should be recovered
        assert_equal(balance_after, node.getbalance())

        self.recover_non_hd()

        self.log.info("Same mnemonic, same mnemonic passphrase, encrypt wallet first, should recover all coins on upgrade after rescan")
        # Null characters are allowed in wallet passphrases since v23
        walletpass = "111\0pass222"
        node.encryptwallet(walletpass)
        assert_raises_rpc_error(-13, "Error: Please enter the wallet passphrase with walletpassphrase first.", node.rescanblockchain)
        assert_raises_rpc_error(-13, "Error: Wallet encrypted but passphrase not supplied to RPC.", node.upgradetohd, mnemonic[0])
        assert_raises_rpc_error(-14, "Error: The wallet passphrase entered was incorrect", node.upgradetohd, mnemonic[0], "", "111")
        assert node.upgradetohd(mnemonic[0], "", walletpass)
        if not self.options.descriptors:
            assert_raises_rpc_error(-13, "Error: Please enter the wallet passphrase with walletpassphrase first.", node.dumphdinfo)
        else:
            assert_raises_rpc_error(-13, "Error: Please enter the wallet passphrase with walletpassphrase first.", node.listdescriptors, True)
        node.walletpassphrase(walletpass, 100)
        assert_equal(mnemonic, get_mnemonic(node))
        if not self.options.descriptors:
            assert_equal(chainid, node.getwalletinfo()['hdchainid'])
        # Note: wallet encryption results in additional keypool topup,
        # so we can't compare new balance to balance_non_HD here,
        # assert_equal(balance_non_HD, node.getbalance())  # won't work
        assert balance_non_HD != node.getbalance()
        node.keypoolrefill(4)
        node.rescanblockchain()
        # All coins should be recovered
        assert_equal(balance_after, node.getbalance())

        self.log.info("Test upgradetohd with user defined mnemonic")
        custom_mnemonic = "similar behave slot swim scissors throw planet view ghost laugh drift calm"
        # this address belongs to custom mnemonic with no passphrase
        custom_address_1 = "yLpq97zZUsFQ2rdMqhcPKkYT36MoPK4Hob"
        # this address belongs to custom mnemonic with passphrase "custom-passphrase"
        custom_address_2 = "yYBPeZQcqgQHu9dxA5pKBWtYbK2hwfFHxf"
        node.sendtoaddress(custom_address_1, 11)
        node.sendtoaddress(custom_address_2, 12)
        self.generate(node, 1)

        node.createwallet("wallet-11", blank=True)
        w11 = node.get_wallet_rpc("wallet-11")
        w11.upgradetohd(custom_mnemonic)
        assert_equal(11, w11.getbalance())
        w11.unloadwallet()

        node.createwallet("wallet-12", blank=True)
        w12 = node.get_wallet_rpc("wallet-12")
        ret = w12.upgradetohd(custom_mnemonic, "custom-passphrase")
        assert_equal(ret, "Make sure that you have backup of your mnemonic.")
        assert_equal(get_mnemonic(w12)[0], custom_mnemonic)
        assert_equal(get_mnemonic(w12)[1], "custom-passphrase")
        assert_equal(12, w12.getbalance())
        w12.unloadwallet()

        self.log.info("Check if null character at the end of mnemonic-passphrase matters")
        node.createwallet("wallet-null", blank=True)
        w_null = node.get_wallet_rpc("wallet-null")
        ret = w_null.upgradetohd(custom_mnemonic, "custom-passphrase\0")
        assert_equal(ret, "Make sure that you have backup of your mnemonic. Your mnemonic passphrase contains a null character (ie - a zero byte). If the passphrase was created with a version of this software prior to 23.0, please try again with only the characters up to — but not including — the first null character. If this is successful, please set a new passphrase to avoid this issue in the future.")
        assert_equal(0, w_null.getbalance())
        assert_equal(get_mnemonic(w_null)[1], "custom-passphrase\0")
        w_null.unloadwallet()

        self.test_mnemonic_passphrase_limit()
        self.test_mnemonic_recovery()
        self.test_startup_mnemonic_recovery()
        self.test_config_section_recovery_opt_in()


if __name__ == '__main__':
    WalletUpgradeToHDTest().main ()
