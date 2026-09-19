#!/usr/bin/env python3
# coding: latin-1

#
# Copyright (c) 2020-present, Chia Network Inc.
# Copyright (c) 2026-present, The Dash Core developers
# SPDX-License-Identifier: Apache-2.0
# See the accompanying file LICENSE or https://opensource.org/licenses/Apache-2.0
#

"""Verify aggregated signatures quickly using the Proof of Possession scheme."""

from dashbls import G1Element, PopSchemeMPL, PrivateKey

SEED: bytes = bytes.fromhex("003206f418c701193458c013120c5906dc12663ad1520c3e596eb6092c14fe16")


def proof_of_possession() -> None:
    sk1: PrivateKey = PopSchemeMPL.key_gen(bytes([1]) + SEED[1:])
    sk2: PrivateKey = PopSchemeMPL.key_gen(bytes([2]) + SEED[1:])
    sk3: PrivateKey = PopSchemeMPL.key_gen(bytes([3]) + SEED[1:])

    pk1: G1Element = sk1.get_g1()
    pk2: G1Element = sk2.get_g1()
    pk3: G1Element = sk3.get_g1()

    message: bytes = bytes([1, 2, 3, 4, 5])

    # If the same message is signed, you can use Proof of Possession (PopScheme) for efficiency.
    # A proof of possession MUST be passed around with the PK to ensure security.
    pop_sig1 = PopSchemeMPL.sign(sk1, message)
    pop_sig2 = PopSchemeMPL.sign(sk2, message)
    pop_sig3 = PopSchemeMPL.sign(sk3, message)
    pop1 = PopSchemeMPL.pop_prove(sk1)
    pop2 = PopSchemeMPL.pop_prove(sk2)
    pop3 = PopSchemeMPL.pop_prove(sk3)

    assert PopSchemeMPL.pop_verify(pk1, pop1)
    assert PopSchemeMPL.pop_verify(pk2, pop2)
    assert PopSchemeMPL.pop_verify(pk3, pop3)

    pop_sig_agg = PopSchemeMPL.aggregate([pop_sig1, pop_sig2, pop_sig3])

    assert PopSchemeMPL.fast_aggregate_verify([pk1, pk2, pk3], message, pop_sig_agg)

    # Aggregate public key, indistinguishable from a single public key
    pop_agg_pk: G1Element = pk1 + pk2 + pk3
    assert PopSchemeMPL.verify(pop_agg_pk, message, pop_sig_agg)

    # Aggregate private keys
    pop_agg_sk: PrivateKey = PrivateKey.aggregate([sk1, sk2, sk3])
    assert PopSchemeMPL.sign(pop_agg_sk, message) == pop_sig_agg

    print(f"aggregate public key: {bytes(pop_agg_pk).hex()}")


if __name__ == "__main__":
    proof_of_possession()
