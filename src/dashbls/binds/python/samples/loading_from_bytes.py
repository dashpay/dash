#!/usr/bin/env python3
# coding: latin-1

#
# Copyright (c) 2020-present, Chia Network Inc.
# Copyright (c) 2026-present, The Dash Core developers
# SPDX-License-Identifier: Apache-2.0
# See the accompanying file LICENSE or https://opensource.org/licenses/Apache-2.0
#

"""Deserialize keys and signatures from bytes."""

from dashbls import AugSchemeMPL, G1Element, G2Element, PrivateKey

SEED: bytes = bytes.fromhex("003206f418c701193458c013120c5906dc12663ad1520c3e596eb6092c14fe16")


def loading_from_bytes() -> None:
    # Stand in for bytes that arrived over the wire or came off disk
    original_sk: PrivateKey = AugSchemeMPL.key_gen(SEED)
    message: bytes = bytes([1, 2, 3, 4, 5])
    sk_bytes: bytes = bytes(original_sk)
    pk_bytes: bytes = bytes(original_sk.get_g1())
    signature_bytes: bytes = bytes(AugSchemeMPL.sign(original_sk, message))

    # from_bytes validates: it rejects points that are malformed or outside the
    # correct subgroup, and raises ValueError rather than returning junk
    sk: PrivateKey = PrivateKey.from_bytes(sk_bytes)
    pk: G1Element = G1Element.from_bytes(pk_bytes)
    signature: G2Element = G2Element.from_bytes(signature_bytes)

    assert sk == original_sk
    assert pk == sk.get_g1()
    assert AugSchemeMPL.verify(pk, message, signature)

    print(f"public key: {bytes(pk).hex()}")
    print(f"signature:  {bytes(signature).hex()}")


if __name__ == "__main__":
    loading_from_bytes()
