#!/usr/bin/env python3
# coding: latin-1

#
# Copyright (c) 2020-present, Chia Network Inc.
# Copyright (c) 2026-present, The Dash Core developers
# SPDX-License-Identifier: Apache-2.0
# See the accompanying file LICENSE or https://opensource.org/licenses/Apache-2.0
#

"""Build an aggregate signature out of an aggregate signature and another signature."""

from dashbls import AugSchemeMPL, G1Element, G2Element, PrivateKey

SEED: bytes = bytes.fromhex("003206f418c701193458c013120c5906dc12663ad1520c3e596eb6092c14fe16")


def tree_aggregates() -> None:
    sk1: PrivateKey = AugSchemeMPL.key_gen(bytes([1]) + SEED[1:])
    sk2: PrivateKey = AugSchemeMPL.key_gen(bytes([2]) + SEED[1:])
    sk3: PrivateKey = AugSchemeMPL.key_gen(bytes([3]) + SEED[1:])

    message1: bytes = bytes([1, 2, 3, 4, 5])
    message2: bytes = bytes([1, 2, 3, 4, 5, 6, 7])
    message3: bytes = bytes([100, 2, 254, 88, 90, 45, 23])

    pk1: G1Element = sk1.get_g1()
    pk2: G1Element = sk2.get_g1()
    pk3: G1Element = sk3.get_g1()

    # Aggregate the first two signatures on their own...
    agg_sig: G2Element = AugSchemeMPL.aggregate(
        [AugSchemeMPL.sign(sk1, message1), AugSchemeMPL.sign(sk2, message2)]
    )

    # ...then fold a third signature into that aggregate. Aggregation composes,
    # so an aggregate is indistinguishable from a plain signature to the next
    # round, and the tree can be built in any shape.
    sig3: G2Element = AugSchemeMPL.sign(sk3, message3)
    agg_sig_final: G2Element = AugSchemeMPL.aggregate([agg_sig, sig3])

    ok = AugSchemeMPL.aggregate_verify(
        [pk1, pk2, pk3], [message1, message2, message3], agg_sig_final
    )
    assert ok

    print(f"tree aggregate: {bytes(agg_sig_final).hex()}")


if __name__ == "__main__":
    tree_aggregates()
