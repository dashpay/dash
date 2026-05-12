#!/usr/bin/env python3
# Copyright (c) 2017 Pieter Wuille
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Reference implementation for Bech32/Bech32m and segwit addresses."""
import unittest
from enum import Enum

CHARSET = "qpzry9x8gf2tvdw0s3jn54khce6mua7l"
BECH32_CONST = 1
BECH32M_CONST = 0x2bc830a3

class Encoding(Enum):
    """Enumeration type to list the various supported encodings."""
    BECH32 = 1
    BECH32M = 2


def bech32_polymod(values):
    """Internal function that computes the Bech32 checksum."""
    generator = [0x3b6a57b2, 0x26508e6d, 0x1ea119fa, 0x3d4233dd, 0x2a1462b3]
    chk = 1
    for value in values:
        top = chk >> 25
        chk = (chk & 0x1ffffff) << 5 ^ value
        for i in range(5):
            chk ^= generator[i] if ((top >> i) & 1) else 0
    return chk


def bech32_hrp_expand(hrp):
    """Expand the HRP into values for checksum computation."""
    return [ord(x) >> 5 for x in hrp] + [0] + [ord(x) & 31 for x in hrp]


def bech32_verify_checksum(hrp, data):
    """Verify a checksum given HRP and converted data characters."""
    check = bech32_polymod(bech32_hrp_expand(hrp) + data)
    if check == BECH32_CONST:
        return Encoding.BECH32
    elif check == BECH32M_CONST:
        return Encoding.BECH32M
    else:
        return None

def bech32_create_checksum(encoding, hrp, data):
    """Compute the checksum values given HRP and data."""
    values = bech32_hrp_expand(hrp) + data
    const = BECH32M_CONST if encoding == Encoding.BECH32M else BECH32_CONST
    polymod = bech32_polymod(values + [0, 0, 0, 0, 0, 0]) ^ const
    return [(polymod >> 5 * (5 - i)) & 31 for i in range(6)]


def bech32_encode(encoding, hrp, data):
    """Compute a Bech32 or Bech32m string given HRP and data values."""
    combined = data + bech32_create_checksum(encoding, hrp, data)
    return hrp + '1' + ''.join([CHARSET[d] for d in combined])


def bech32_decode(bech):
    """Validate a Bech32/Bech32m string, and determine HRP and data."""
    if ((any(ord(x) < 33 or ord(x) > 126 for x in bech)) or
            (bech.lower() != bech and bech.upper() != bech)):
        return (None, None, None)
    bech = bech.lower()
    pos = bech.rfind('1')
    if pos < 1 or pos + 7 > len(bech) or len(bech) > 90:
        return (None, None, None)
    if not all(x in CHARSET for x in bech[pos+1:]):
        return (None, None, None)
    hrp = bech[:pos]
    data = [CHARSET.find(x) for x in bech[pos+1:]]
    encoding = bech32_verify_checksum(hrp, data)
    if encoding is None:
        return (None, None, None)
    return (encoding, hrp, data[:-6])


def convertbits(data, frombits, tobits, pad=True):
    """General power-of-2 base conversion."""
    acc = 0
    bits = 0
    ret = []
    maxv = (1 << tobits) - 1
    max_acc = (1 << (frombits + tobits - 1)) - 1
    for value in data:
        if value < 0 or (value >> frombits):
            return None
        acc = ((acc << frombits) | value) & max_acc
        bits += frombits
        while bits >= tobits:
            bits -= tobits
            ret.append((acc >> bits) & maxv)
    if pad:
        if bits:
            ret.append((acc << (tobits - bits)) & maxv)
    elif bits >= frombits or ((acc << (tobits - bits)) & maxv):
        return None
    return ret


DIP18_TYPE_P2PKH = 0xb0
DIP18_TYPE_P2SH = 0x80


def encode_platform_p2pkh(hrp, keyhash):
    """Encode a 20-byte keyhash as a DIP-18 Platform P2PKH bech32m address."""
    assert len(keyhash) == 20
    payload = [DIP18_TYPE_P2PKH] + list(keyhash)
    data = convertbits(payload, 8, 5)
    return bech32_encode(Encoding.BECH32M, hrp, data)


def encode_platform_p2sh(hrp, scripthash):
    """Encode a 20-byte scripthash as a DIP-18 Platform P2SH bech32m address."""
    assert len(scripthash) == 20
    payload = [DIP18_TYPE_P2SH] + list(scripthash)
    data = convertbits(payload, 8, 5)
    return bech32_encode(Encoding.BECH32M, hrp, data)


def decode_platform_address(hrp, addr):
    """Decode a DIP-18 bech32m platform address. Returns (type_byte, hash_bytes) or (None, None)."""
    encoding, hrpgot, data = bech32_decode(addr)
    if encoding != Encoding.BECH32M or hrpgot != hrp:
        return (None, None)
    payload = convertbits(data, 5, 8, pad=False)
    if payload is None or len(payload) != 21:
        return (None, None)
    type_byte = payload[0]
    if type_byte not in (DIP18_TYPE_P2PKH, DIP18_TYPE_P2SH):
        return (None, None)
    return (type_byte, bytes(payload[1:]))


class TestFrameworkScript(unittest.TestCase):
    def test_platform_encode_decode(self):
        def test_platform_roundtrip(hrp, addr, expected_type):
            typ, payload = decode_platform_address(hrp, addr)
            self.assertIsNotNone(typ)
            self.assertEqual(typ, expected_type)
            if expected_type == DIP18_TYPE_P2PKH:
                self.assertEqual(encode_platform_p2pkh(hrp, payload), addr)
            else:
                self.assertEqual(encode_platform_p2sh(hrp, payload), addr)

        # DIP-18 P2PKH
        test_platform_roundtrip('dash', 'dash1krma5z3ttj75la4m93xcndna9ullamq9y5e9n5rs', DIP18_TYPE_P2PKH)
        test_platform_roundtrip('tdash', 'tdash1krma5z3ttj75la4m93xcndna9ullamq9y5fzq2j7', DIP18_TYPE_P2PKH)
        # DIP-18 P2SH
        test_platform_roundtrip('dash', 'dash1sppl5xpu70aka8nacc4kj2htflydspzkxch4cad6', DIP18_TYPE_P2SH)
        test_platform_roundtrip('tdash', 'tdash1sppl5xpu70aka8nacc4kj2htflydspzkxc8jtru5', DIP18_TYPE_P2SH)
