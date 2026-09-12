#!/usr/bin/env python3
# Copyright (c) 2020-2022 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test error messages for 'getaddressinfo' and 'validateaddress' RPC commands."""

from test_framework.test_framework import BitcoinTestFramework

from test_framework.script_util import (
    keyhash_to_p2pkh_script,
    scripthash_to_p2sh_script,
)
from test_framework.segwit_addr import (
    DIP18_TYPE_P2PKH,
    Encoding,
    bech32_encode,
    convertbits,
)
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)

PLATFORM_HRP = 'tdash'
PLATFORM_KEYHASH = bytes.fromhex('f7da0a2b5cbd4ff6bb2c4d89b67d2f3ffeec0525')
PLATFORM_SCRIPTHASH = bytes.fromhex('43fa183cf3fb6e9e7dc62b692aeb4fc8d8045636')


def platform_address(encoding, type_byte, payload):
    return bech32_encode(encoding, PLATFORM_HRP, convertbits([type_byte] + list(payload), 8, 5))


# DIP-18 Platform addresses (test vectors of DIP-0018): valid Bech32m, but never Dash Core addresses
BECH32_VALID = 'tdash1krma5z3ttj75la4m93xcndna9ullamq9y5fzq2j7'
BECH32_VALID_CAPITALS = 'TDASH1KRMA5Z3TTJ75LA4M93XCNDNA9ULLAMQ9Y5FZQ2J7'
BECH32_VALID_P2SH = 'tdash1sppl5xpu70aka8nacc4kj2htflydspzkxc8jtru5'

# Well-formed Bech32(m) strings whose DIP-18 payload is invalid
BECH32_INVALID_ENCODING = platform_address(Encoding.BECH32, DIP18_TYPE_P2PKH, PLATFORM_KEYHASH)
BECH32_INVALID_TYPE_BYTE = platform_address(Encoding.BECH32M, 0x00, PLATFORM_KEYHASH)
BECH32_INVALID_SIZE = platform_address(Encoding.BECH32M, DIP18_TYPE_P2PKH, PLATFORM_KEYHASH[:-1])

BECH32_INVALID_PREFIX = 'bc1pw508d6qejxtdg4y5r3zarvary0c5xw7kw508d6qejxtdg4y5r3zarvary0c5xw7k7grplx'
BECH32_TOO_LONG = 'tdash1krma5z3ttj75la4m93xcndna9ullamq9y5fzq2j7krma5z3ttj75la4m93xcndna9ullamq9y5fzq2j7krma5z3ttj75la4m93xcndna9ullamq9y5fzq2j7'
BECH32_ONE_ERROR = 'tdash1krma4z3ttj75la4m93xcndna9ullamq9y5fzq2j7'
BECH32_ONE_ERROR_CAPITALS = 'TDASH1KRMA5Z3TTJ75LA4M93XCNDNA9ULLAMQ9Y4FZQ2J7'
BECH32_TWO_ERRORS = 'tdash1krma4z3ttj75la4m93xcndna8ullamq9y5fzq2j7'  # should be tdash1krma5z3ttj75la4m93xcndna9ullamq9y5fzq2j7
BECH32_P2SH_TWO_ERRORS = 'tdash1sppl5xpu70aka8nacd4kj2htflydspzkxc8jtrs5'  # should be tdash1sppl5xpu70aka8nacc4kj2htflydspzkxc8jtru5
BECH32_NO_SEPARATOR = 'tdashkrma5z3ttj75la4m93xcndna9ullamq9y5fzq2j7'
BECH32_INVALID_CHAR = 'tdash1krmo5z3ttj75la4m93xcndna9ullamq9y5fzq2j7'

BASE58_VALID = 'yjQ5gLvGRtmq1cwc4kePLCrzQ8GVCh9Gaz'
BASE58_INVALID_PREFIX = 'XpG61qAVhdyN7AqVZQsHfJL7AEk4dPVinc'
BASE58_INVALID_CHECKSUM = 'yjQ5gLvGRtmq1cwc4kePLCrzQ8GVCh9Gaa'
BASE58_INVALID_LENGTH = '2VKf7XKMrp4bVNVmuRbyCewkP8FhGLP2E54LHDPakr9Sq5mtU2'

INVALID_ADDRESS = 'asfah14i8fajz0123f'
INVALID_ADDRESS_2 = '1q049ldschfnwystcqnsvyfpj23mpsg3jcedq9xv'

class InvalidAddressErrorMessageTest(BitcoinTestFramework):
    def add_options(self, parser):
        self.add_wallet_options(parser)

    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1

    def check_valid(self, addr):
        info = self.nodes[0].validateaddress(addr)
        assert info['isvalid']
        assert 'error' not in info
        assert 'error_locations' not in info

    def check_invalid(self, addr, error_str, error_locations=None):
        res = self.nodes[0].validateaddress(addr)
        assert not res['isvalid']
        assert 'isplatform' not in res
        assert_equal(res['error'], error_str)
        if error_locations:
            assert_equal(res['error_locations'], error_locations)
        else:
            assert_equal(res['error_locations'], [])

    def check_platform(self, addr, normalized, script, is_script):
        res = self.nodes[0].validateaddress(addr)
        assert_equal(res['isvalid'], True)
        assert_equal(res['isplatform'], True)
        assert_equal(res['address'], normalized)
        # Described against the credit output script an asset lock would carry
        # for it, consistent with getaddressinfo
        assert_equal(res['scriptPubKey'], script.hex())
        assert_equal(res['isscript'], is_script)
        assert 'error' not in res
        assert 'error_locations' not in res

    def test_validateaddress(self):
        # Invalid Bech32
        self.check_invalid(BECH32_INVALID_PREFIX, 'Not a valid Bech32m or Base58 encoding')
        self.check_invalid(BECH32_TOO_LONG, 'Bech32 string too long', list(range(90, len(BECH32_TOO_LONG))))
        self.check_invalid(BECH32_ONE_ERROR, 'Invalid Bech32m checksum', [10])
        self.check_invalid(BECH32_TWO_ERRORS, 'Invalid Bech32m checksum', [10, 30])
        self.check_invalid(BECH32_ONE_ERROR_CAPITALS, 'Invalid Bech32m checksum', [39])
        self.check_invalid(BECH32_NO_SEPARATOR, 'Missing separator')
        self.check_invalid(BECH32_INVALID_CHAR, 'Invalid Base 32 character', [9])
        self.check_invalid(BECH32_P2SH_TWO_ERRORS, 'Invalid Bech32m checksum', [23, 44])

        # Bech32 strings with a valid checksum but an invalid DIP-18 Platform payload
        self.check_invalid(BECH32_INVALID_ENCODING, 'DIP-18 Platform addresses require bech32m checksum')
        self.check_invalid(BECH32_INVALID_TYPE_BYTE, 'Unknown DIP-18 type byte')
        self.check_invalid(BECH32_INVALID_SIZE, 'Invalid Platform address payload length')

        # Valid Bech32m: DIP-18 Platform addresses, reported as such and normalized to lower case
        p2pkh_script = keyhash_to_p2pkh_script(PLATFORM_KEYHASH)
        p2sh_script = scripthash_to_p2sh_script(PLATFORM_SCRIPTHASH)
        self.check_platform(BECH32_VALID, BECH32_VALID, p2pkh_script, False)
        self.check_platform(BECH32_VALID_CAPITALS, BECH32_VALID, p2pkh_script, False)
        self.check_platform(BECH32_VALID_P2SH, BECH32_VALID_P2SH, p2sh_script, True)

        # Invalid Base58
        self.check_invalid(BASE58_INVALID_PREFIX, 'Invalid prefix for Base58-encoded address')
        self.check_invalid(BASE58_INVALID_CHECKSUM, 'Invalid checksum or length of Base58 address')
        self.check_invalid(BASE58_INVALID_LENGTH, 'Invalid checksum or length of Base58 address')

        # Valid Base58
        self.check_valid(BASE58_VALID)

        # Invalid address format
        self.check_invalid(INVALID_ADDRESS, 'Not a valid Bech32m or Base58 encoding')
        self.check_invalid(INVALID_ADDRESS_2, 'Not a valid Bech32m or Base58 encoding')

    def test_getaddressinfo(self):
        node = self.nodes[0]

        assert_raises_rpc_error(-5, "Invalid Platform address payload length", node.getaddressinfo, BECH32_INVALID_SIZE)

        assert_raises_rpc_error(-5, "Not a valid Bech32m or Base58 encoding", node.getaddressinfo, BECH32_INVALID_PREFIX)

        assert_raises_rpc_error(-5, "Invalid prefix for Base58-encoded address", node.getaddressinfo, BASE58_INVALID_PREFIX)

        assert_raises_rpc_error(-5, "Not a valid Bech32m or Base58 encoding", node.getaddressinfo, INVALID_ADDRESS)

        # A DIP-18 Platform address is described against the credit output script an
        # asset lock would carry for it
        info = node.getaddressinfo(BECH32_VALID)
        assert_equal(info['isplatform'], True)
        assert_equal(info['address'], BECH32_VALID)
        assert_equal(info['isscript'], False)
        assert_equal(info['scriptPubKey'], keyhash_to_p2pkh_script(PLATFORM_KEYHASH).hex())

        info = node.getaddressinfo(BECH32_VALID_P2SH)
        assert_equal(info['isplatform'], True)
        assert_equal(info['isscript'], True)

        # A regular Dash address is not reported as a Platform one
        assert 'isplatform' not in node.getaddressinfo(BASE58_VALID)

    def run_test(self):
        self.test_validateaddress()

        if self.is_wallet_compiled():
            self.init_wallet(node=0)
            self.test_getaddressinfo()


if __name__ == '__main__':
    InvalidAddressErrorMessageTest().main()
