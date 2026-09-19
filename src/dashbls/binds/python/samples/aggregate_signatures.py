#!/usr/bin/env python3
# coding: latin-1

#
# Copyright (c) 2020-present, Chia Network Inc.
# Copyright (c) 2026-present, The Dash Core developers
# SPDX-License-Identifier: Apache-2.0
# See the accompanying file LICENSE or https://opensource.org/licenses/Apache-2.0
#

"""Aggregate signatures from multiple keys over multiple messages."""

from dashbls import AugSchemeMPL, G1Element, G2Element, PrivateKey

SEED: bytes = bytes.fromhex("003206f418c701193458c013120c5906dc12663ad1520c3e596eb6092c14fe16")


def aggregate_signatures() -> None:
    # Two independent signers, each with their own message
    sk1: PrivateKey = AugSchemeMPL.key_gen(bytes([1]) + SEED[1:])
    sk2: PrivateKey = AugSchemeMPL.key_gen(bytes([2]) + SEED[1:])

    message1: bytes = bytes([1, 2, 3, 4, 5])
    message2: bytes = bytes([1, 2, 3, 4, 5, 6, 7])

    pk1: G1Element = sk1.get_g1()
    sig1: G2Element = AugSchemeMPL.sign(sk1, message1)

    pk2: G1Element = sk2.get_g1()
    sig2: G2Element = AugSchemeMPL.sign(sk2, message2)

    # Signatures can be non-interactively combined by anyone
    agg_sig: G2Element = AugSchemeMPL.aggregate([sig1, sig2])

    # Verifying an aggregate over distinct messages needs every public key and
    # every message, in the order they were signed in
    ok = AugSchemeMPL.aggregate_verify([pk1, pk2], [message1, message2], agg_sig)
    assert ok

    print(f"aggregate signature: {bytes(agg_sig).hex()}")


if __name__ == "__main__":
    aggregate_signatures()
