#!/usr/bin/env python3
# coding: latin-1

#
# Copyright (c) 2020-present, Chia Network Inc.
# Copyright (c) 2026-present, The Dash Core developers
# SPDX-License-Identifier: Apache-2.0
# See the accompanying file LICENSE or https://opensource.org/licenses/Apache-2.0
#

"""Serialize keys and signatures to bytes."""

from dashbls import AugSchemeMPL, G1Element, G2Element, PrivateKey

SEED: bytes = bytes.fromhex("003206f418c701193458c013120c5906dc12663ad1520c3e596eb6092c14fe16")


def serializing_to_bytes() -> None:
    sk: PrivateKey = AugSchemeMPL.key_gen(SEED)
    pk: G1Element = sk.get_g1()
    signature: G2Element = AugSchemeMPL.sign(sk, bytes([1, 2, 3, 4, 5]))

    sk_bytes: bytes = bytes(sk)  # 32 bytes
    pk_bytes: bytes = bytes(pk)  # 48 bytes
    signature_bytes: bytes = bytes(signature)  # 96 bytes

    assert (len(sk_bytes), len(pk_bytes), len(signature_bytes)) == (32, 48, 96)

    # Serialized private keys are secret material; treat them the way you would
    # treat the seed they came from, and never log them.
    print(f"public key: {pk_bytes.hex()}")
    print(f"signature:  {signature_bytes.hex()}")


if __name__ == "__main__":
    serializing_to_bytes()
