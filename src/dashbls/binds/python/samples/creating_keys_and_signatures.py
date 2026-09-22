#!/usr/bin/env python3
# coding: latin-1

#
# Copyright (c) 2020-present, Chia Network Inc.
# Copyright (c) 2026-present, The Dash Core developers
# SPDX-License-Identifier: Apache-2.0
# See the accompanying file LICENSE or https://opensource.org/licenses/Apache-2.0
#

"""Generate a keypair and produce/verify a signature."""

from dashbls import AugSchemeMPL, G1Element, G2Element, PrivateKey

SEED: bytes = bytes.fromhex("003206f418c701193458c013120c5906dc12663ad1520c3e596eb6092c14fe16")


def creating_keys_and_signatures() -> None:
    # Example seed. Always use a secure RNG with sufficient entropy to generate
    # a seed (at least 32 bytes).
    sk: PrivateKey = AugSchemeMPL.key_gen(SEED)
    pk: G1Element = sk.get_g1()

    message: bytes = bytes([1, 2, 3, 4, 5])
    signature: G2Element = AugSchemeMPL.sign(sk, message)

    # Verify the signature
    ok: bool = AugSchemeMPL.verify(pk, message, signature)
    assert ok

    print(f"public key: {bytes(pk).hex()}")
    print(f"signature:  {bytes(signature).hex()}")


if __name__ == "__main__":
    creating_keys_and_signatures()
