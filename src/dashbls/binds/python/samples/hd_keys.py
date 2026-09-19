#!/usr/bin/env python3
# coding: latin-1

#
# Copyright (c) 2020-present, Chia Network Inc.
# Copyright (c) 2026-present, The Dash Core developers
# SPDX-License-Identifier: Apache-2.0
# See the accompanying file LICENSE or https://opensource.org/licenses/Apache-2.0
#

"""Derive hardened and unhardened HD keys, per EIP-2333."""

from dashbls import AugSchemeMPL, G1Element, PrivateKey

SEED: bytes = bytes.fromhex("003206f418c701193458c013120c5906dc12663ad1520c3e596eb6092c14fe16")


def hd_keys() -> None:
    master_sk: PrivateKey = AugSchemeMPL.key_gen(SEED)
    master_pk: G1Element = master_sk.get_g1()

    # Hardened derivation needs the private key, and the resulting child cannot
    # be derived from the public key alone
    child: PrivateKey = AugSchemeMPL.derive_child_sk(master_sk, 152)
    grandchild: PrivateKey = AugSchemeMPL.derive_child_sk(child, 952)

    # Unhardened derivation can be mirrored on the public side, so a watch-only
    # holder of the master public key can compute the same child public keys
    child_u: PrivateKey = AugSchemeMPL.derive_child_sk_unhardened(master_sk, 22)
    grandchild_u: PrivateKey = AugSchemeMPL.derive_child_sk_unhardened(child_u, 0)

    child_u_pk: G1Element = AugSchemeMPL.derive_child_pk_unhardened(master_pk, 22)
    grandchild_u_pk: G1Element = AugSchemeMPL.derive_child_pk_unhardened(child_u_pk, 0)

    assert grandchild_u_pk == grandchild_u.get_g1()

    print(f"hardened grandchild:   {bytes(grandchild.get_g1()).hex()}")
    print(f"unhardened grandchild: {bytes(grandchild_u_pk).hex()}")


if __name__ == "__main__":
    hd_keys()
