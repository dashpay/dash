#!/usr/bin/env python3
# coding: latin-1

#
# Copyright (c) 2020-present, Chia Network Inc.
# Copyright (c) 2026-present, The Dash Core developers
# SPDX-License-Identifier: Apache-2.0
# See the accompanying file LICENSE or https://opensource.org/licenses/Apache-2.0
#

"""Unit tests for the dashbls package."""

import binascii
import importlib

import pytest
from dashbls import (
    AugSchemeMPL,
    BasicSchemeMPL,
    G1Element,
    G2Element,
    GTElement,
    PopSchemeMPL,
    PrivateKey,
    Util,
)

# fmt: off
SEED = bytes([
    0, 50, 6, 244, 24, 199, 1, 25, 52, 88, 192, 19, 18, 12, 89, 6,
    220, 18, 102, 58, 209, 82, 12, 62, 89, 110, 182, 9, 44, 20, 254, 22
])
# fmt: on
MSG = bytes([100, 2, 254, 88, 90, 45, 23])
MSG2 = bytes([1, 2, 3, 4, 5, 6, 7, 8, 9, 10])

SCHEMES = (BasicSchemeMPL, AugSchemeMPL, PopSchemeMPL)
SCHEME_IDS = [scheme.__name__ for scheme in SCHEMES]

G1_DST = b"BLS_SIG_BLS12381G1_XMD:SHA-256_SSWU_RO_NUL_"
G2_DST = b"BLS_SIG_BLS12381G2_XMD:SHA-256_SSWU_RO_NUL_"

# Keeps md_hmac's alloca of the seed off the stack, see CopySeed in the binds
SEED_LIMIT = 64 * 1024


def _derive_two_keypairs() -> tuple[PrivateKey, G1Element, PrivateKey, G1Element]:
    seed1 = bytes([1]) + SEED[1:]
    sk1 = BasicSchemeMPL.key_gen(seed1)
    seed2 = bytes([2]) + seed1[1:]
    sk2 = BasicSchemeMPL.key_gen(seed2)
    return sk1, sk1.get_g1(), sk2, sk2.get_g1()


@pytest.fixture
def keypairs() -> tuple[PrivateKey, G1Element, PrivateKey, G1Element]:
    return _derive_two_keypairs()


def test_private_key_and_public_key_roundtrip() -> None:
    sk = BasicSchemeMPL.key_gen(SEED)
    pk = sk.get_g1()
    assert sk == PrivateKey.from_bytes(bytes(sk))
    assert pk == G1Element.from_bytes(bytes(pk))


def test_scalar_multiplication_by_private_key() -> None:
    """The operators took `bn_t`, which pybind11 has no caster for, so every call
    raised TypeError regardless of argument."""
    sk = BasicSchemeMPL.key_gen(SEED)
    g1 = G1Element.generator()
    assert g1 * sk == sk.get_g1()
    assert sk * g1 == sk.get_g1()

    g2 = G2Element.generator()
    assert g2 * sk == sk * g2
    assert g2 * sk != G2Element()


def test_scalar_multiplication_is_additive(
    keypairs: tuple[PrivateKey, G1Element, PrivateKey, G1Element],
) -> None:
    sk1, _pk1, sk2, _pk2 = keypairs
    g1 = G1Element.generator()
    assert (g1 * sk1) + (g1 * sk2) == g1 * PrivateKey.aggregate([sk1, sk2])


@pytest.mark.parametrize("scheme", SCHEMES, ids=SCHEME_IDS)
def test_sign_and_verify_roundtrip(scheme: type) -> None:
    sk = BasicSchemeMPL.key_gen(SEED)
    pk = sk.get_g1()
    sig = scheme.sign(sk, MSG)
    assert sig == G2Element.from_bytes(bytes(sig))
    assert scheme.verify(pk, MSG, sig)


@pytest.mark.parametrize("scheme", SCHEMES, ids=SCHEME_IDS)
def test_aggregate_verify_same_message(
    scheme: type, keypairs: tuple[PrivateKey, G1Element, PrivateKey, G1Element]
) -> None:
    sk1, pk1, sk2, pk2 = keypairs
    agg_pk = pk1 + pk2
    if scheme is AugSchemeMPL:
        sig1 = scheme.sign(sk1, MSG, agg_pk)
        sig2 = scheme.sign(sk2, MSG, agg_pk)
    else:
        sig1 = scheme.sign(sk1, MSG)
        sig2 = scheme.sign(sk2, MSG)
    agg_sig = scheme.aggregate([sig1, sig2])
    assert scheme.verify(agg_pk, MSG, agg_sig)


@pytest.mark.parametrize("scheme", SCHEMES, ids=SCHEME_IDS)
def test_aggregate_verify_different_messages(
    scheme: type, keypairs: tuple[PrivateKey, G1Element, PrivateKey, G1Element]
) -> None:
    sk1, pk1, sk2, pk2 = keypairs
    sig1 = scheme.sign(sk1, MSG)
    sig2 = scheme.sign(sk2, MSG2)
    agg_sig = scheme.aggregate([sig1, sig2])
    assert scheme.aggregate_verify([pk1, pk2], [MSG, MSG2], agg_sig)


@pytest.mark.parametrize("scheme", SCHEMES, ids=SCHEME_IDS)
def test_manual_pairing_matches_aggregate_signature(
    scheme: type, keypairs: tuple[PrivateKey, G1Element, PrivateKey, G1Element]
) -> None:
    sk1, pk1, sk2, pk2 = keypairs
    sig1 = scheme.sign(sk1, MSG)
    sig2 = scheme.sign(sk2, MSG2)
    agg_sig = scheme.aggregate([sig1, sig2])

    if scheme is AugSchemeMPL:
        # AugSchemeMPL requires prepending the public key to the message
        aug_msg1 = bytes(pk1) + MSG
        aug_msg2 = bytes(pk2) + MSG2
    else:
        aug_msg1 = MSG
        aug_msg2 = MSG2
    pair1 = pk1.pair(scheme.g2_from_message(aug_msg1))
    pair2 = pk2.pair(scheme.g2_from_message(aug_msg2))
    pair = pair1 * pair2
    agg_sig_pair = G1Element.generator().pair(agg_sig)
    assert pair == agg_sig_pair


@pytest.mark.parametrize("scheme", SCHEMES, ids=SCHEME_IDS)
def test_hd_key_derivation_sign_and_verify(
    scheme: type, keypairs: tuple[PrivateKey, G1Element, PrivateKey, G1Element]
) -> None:
    sk1, pk1, _sk2, _pk2 = keypairs
    child = scheme.derive_child_sk(sk1, 123)
    child_u = scheme.derive_child_sk_unhardened(sk1, 123)
    child_u_pk = scheme.derive_child_pk_unhardened(pk1, 123)

    sig_child = scheme.sign(child, MSG)
    assert scheme.verify(child.get_g1(), MSG, sig_child)

    sig_u_child = scheme.sign(child_u, MSG)
    assert scheme.verify(child_u_pk, MSG, sig_u_child)


# Invalid inputs from https://github.com/algorand/bls_sigs_ref/blob/master/python-impl/serdesZ.py
INVALID_G1_VECTORS = [
    # infinity points: too short
    "c000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
    # infinity points: not all zeros
    "c00000000000000000000000000001000000000000000000000000000000000000000000000000000000000000000000",
    # bad tags
    "3a0111ea397fe69a4b1ba7b6434bacd764774b84f38512bf6730d2a0f6b0f6241eabfffeb153ffffb9feffffffffaaaa",
    "7a0111ea397fe69a4b1ba7b6434bacd764774b84f38512bf6730d2a0f6b0f6241eabfffeb153ffffb9feffffffffaaaa",
    "fa0111ea397fe69a4b1ba7b6434bacd764774b84f38512bf6730d2a0f6b0f6241eabfffeb153ffffb9feffffffffaaaa",
    # wrong length for compresed point
    "9a0111ea397fe69a4b1ba7b6434bacd764774b84f38512bf6730d2a0f6b0f6241eabfffeb153ffffb9feffffffffaa",
    "9a0111ea397fe69a4b1ba7b6434bacd764774b84f38512bf6730d2a0f6b0f6241eabfffeb153ffffb9feffffffffaaaaaa",
    # invalid x-coord
    "9a0111ea397fe69a4b1ba7b6434bacd764774b84f38512bf6730d2a0f6b0f6241eabfffeb153ffffb9feffffffffaaaa",
    # invalid elm of Fp --- equal to p (must be strictly less)
    "9a0111ea397fe69a4b1ba7b6434bacd764774b84f38512bf6730d2a0f6b0f6241eabfffeb153ffffb9feffffffffaaab",
]
INVALID_G2_VECTORS = [
    # infinity points: too short
    "c000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
    # infinity points: not all zeros
    "c00000000000000000000000000001000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
    "c00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000010000000000000000000000000000000000000000000",
    # bad tags
    "3a0111ea397fe69a4b1ba7b6434bacd764774b84f38512bf6730d2a0f6b0f6241eabfffeb153ffffb9feffffffffaaaa000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
    "7a0111ea397fe69a4b1ba7b6434bacd764774b84f38512bf6730d2a0f6b0f6241eabfffeb153ffffb9feffffffffaaaa000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
    "fa0111ea397fe69a4b1ba7b6434bacd764774b84f38512bf6730d2a0f6b0f6241eabfffeb153ffffb9feffffffffaaaa000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
    # wrong length for compressed point
    "9a0111ea397fe69a4b1ba7b6434bacd764774b84f38512bf6730d2a0f6b0f6241eabfffeb153ffffb9feffffffffaaaa0000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
    "9a0111ea397fe69a4b1ba7b6434bacd764774b84f38512bf6730d2a0f6b0f6241eabfffeb153ffffb9feffffffffaaaa00000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
    # invalid x-coord
    "9a0111ea397fe69a4b1ba7b6434bacd764774b84f38512bf6730d2a0f6b0f6241eabfffeb153ffffb9feffffffffaaaa1a0111ea397fe69a4b1ba7b6434bacd764774b84f38512bf6730d2a0f6b0f6241eabfffeb153ffffb9feffffffffaaa7",
    # invalid elm of Fp --- equal to p (must be strictly less)
    "9a0111ea397fe69a4b1ba7b6434bacd764774b84f38512bf6730d2a0f6b0f6241eabfffeb153ffffb9feffffffffaaab000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000000",
    "9a0111ea397fe69a4b1ba7b6434bacd764774b84f38512bf6730d2a0f6b0f6241eabfffeb153ffffb9feffffffffaaaa1a0111ea397fe69a4b1ba7b6434bacd764774b84f38512bf6730d2a0f6b0f6241eabfffeb153ffffb9feffffffffaaab",
]


@pytest.mark.parametrize("hex_str", INVALID_G1_VECTORS)
def test_invalid_g1_vector_rejected(hex_str: str) -> None:
    with pytest.raises(ValueError):
        G1Element(binascii.unhexlify(hex_str))


@pytest.mark.parametrize("hex_str", INVALID_G2_VECTORS)
def test_invalid_g2_vector_rejected(hex_str: str) -> None:
    with pytest.raises(ValueError):
        G2Element(binascii.unhexlify(hex_str))


# The following code was used to generate the reference vectors below
"""
from py_ecc.bls import (
    G2Basic,
    G2MessageAugmentation as G2MA,
    G2ProofOfPossession as G2Pop,
)

secret1 = bytes([1] * 32)
secret2 = bytes([x * 314159 % 256 for x in range(32)])
sk1 = int.from_bytes(secret1, 'big')
sk2 = int.from_bytes(secret2, 'big')
msg = bytes([3, 1, 4, 1, 5, 9])
pk1 = G2Basic.SkToPk(sk1)
pk2 = G2Basic.SkToPk(sk2)

for Scheme in (G2Basic, G2MA, G2Pop):
    sig1 = Scheme.Sign(sk1, msg)
    sig2 = Scheme.Sign(sk2, msg)
    sig_agg = Scheme.Aggregate([sig1, sig2])
    print(sig1)
    print(sig2)
    print(sig_agg)
"""

REFERENCE_VECTORS = {
    BasicSchemeMPL: (
        b"\x96\xba4\xfa\xc3<\x7f\x12\x9d`*\x0b\xc8\xa3\xd4?\x9a\xbc\x01N\xce\xaa\xb75\x91F\xb4\xb1P\xe5{\x80\x86Es\x8f5g\x1e\x9e\x10\xe0\xd8b\xa3\x0c\xabp\x07N\xb5\x83\x1d\x13\xe6\xa5\xb1b\xd0\x1e\xeb\xe6\x87\xd0\x16J\xdb\xd0\xa8d7\n|\"*'h\xd7pM\xa2T\xf1\xbf\x18#f[\xc26\x1f\x9d\xd8\xc0\x0e\x99",
        b'\xa4\x02y\t2\x13\x0fvj\xf1\x1b\xa7\x16Sf\x83\xd8\xc4\xcf\xa5\x19G\xe4\xf9\x08\x1f\xed\xd6\x92\xd6\xdc\x0c\xac[\x90K\xee^\xa6\xe2Ui\xe3m{\xe4\xcaY\x06\x9a\x96\xe3K\x7fp\x07X\xb7\x16\xf9IJ\xaaY\xa9nt\xd1J;U*\x9ak\xc1)\xe7\x17\x19[\x9d`\x06\xfdm\\\xefGh\xc0"\xe0\xf71j\xbf',
        b"\x98|\xfd;\xcdb(\x02\x87\x02t\x83\xf2\x9cU$^\xd81\xf5\x1d\xd6\xbd\x99\x9ao\xf1\xa1\xf1\xf1\xf0\xb6Gw\x8b\x01g5\x9cqPUX\xa7n\x15\x8ef\x18\x1e\xe5\x12Y\x05\xa6B$k\x01\xe7\xfa^\xe5=h\xa4\xfe\x9b\xfb)\xa8\xe2f\x01\xf0\xb9\xadW}\xdd\x18\x87js1|!n\xa6\x1fC\x04\x14\xecQ\xc5",
    ),
    AugSchemeMPL: (
        b'\x81\x80\xf0,\xcbr\xe9"\xb1R\xfc\xed\xbe\x0e\x1d\x19R\x105Opp6X\xe8\xe0\x8c\xbe\xbf\x11\xd4\x97\x0e\xabj\xc3\xcc\xf7\x15\xf3\xfb\x87m\xf9\xa9yz\xbd\x0c\x1a\xf6\x1a\xae\xad\xc9,,\xfe\\\nV\xc1F\xcc\x8c?qQ\xa0s\xcf_\x16\xdf8$g$\xc4\xae\xd7?\xf3\x0e\xf5\xda\xa6\xaa\xca\xed\x1a&\xec\xaa3k',
        b'\x99\x11\x1e\xea\xfbA-\xa6\x1eL7\xd3\xe8\x06\xc6\xfdj\xc9\xf3\x87\x0eT\xda\x92"\xbaNIH"\xc5\xb7eg1\xfazdY4\xd0KU\x9e\x92a\xb8b\x01\xbb\xeeW\x05RP\xa4Y\xa2\xda\x10\xe5\x1f\x9c\x1aiA)\x7f\xfc]\x97\nUr6\xd0\xbd\xeb|\xf8\xff\x18\x80\x0b\x08c8q\xa0\xf0\xa7\xeaB\xf4t\x80',
        b"\x8c]\x03\xf9\xda\xe7~\x19\xa5\x94Z\x06\xa2\x14\x83n\xdb\x8e\x03\xb8QR]\x84\xb9\xded@\xe6\x8f\xc0\xcas\x03\xee\xed9\r\x86<\x9bU\xa8\xcfmY\x14\n\x01\xb5\x88G\x88\x1e\xb5\xafgsMD\xb2UVF\xc6al9\xab\x88\xd2S)\x9a\xcc\x1e\xb1\xb1\x9d\xdb\x9b\xfc\xbev\xe2\x8a\xdd\xf6q\xd1\x16\xc0R\xbb\x18G",
    ),
    PopSchemeMPL: (
        b"\x95P\xfbN\x7f~\x8c\xc4\xa9\x0b\xe8V\n\xb5\xa7\x98\xb0\xb20\x00\xb6\xa5J!\x17R\x02\x10\xf9\x86\xf3\xf2\x81\xb3v\xf2Y\xc0\xb7\x80b\xd1\xeb1\x92\xb3\xd9\xbb\x04\x9fY\xec\xc1\xb0:pI\xebf^\r\xf3d\x94\xaeL\xb5\xf1\x13l\xca\xee\xfc\x99X\xcb0\xc33==C\xf0qH\xc3\x86)\x9a{\x1b\xfc\r\xc5\xcf|",
        b"\xa6\x906\xbc\x11\xae^\xfc\xbfa\x80\xaf\xe3\x9a\xdd\xde~'s\x1e\xc4\x02W\xbf\xdc<7\xf1{\x8d\xf6\x83\x06\xa3N\xbd\x10\xe9\xe3*5%7P\xdf\\\x87\xc2\x14/\x82\x07\xe8\xd5eG\x12\xb4\xe5T\xf5\x85\xfbhF\xff8\x04\xe4)\xa9\xf8\xa1\xb4\xc5ku\xd0\x86\x9e\xd6u\x80\xd7\x89\x87\x0b\xab\xe2\xc7\xc8\xa9\xd5\x1e{*",
        b"\xa4\xeat+\xcd\xc1U>\x9c\xa4\xe5`\xbe~^ln\xfajd\xdd\xdf\x9c\xa3\xbb(T#=\x85\xa6\xaa\xc1\xb7n\xc7\xd1\x03\xdbN3\x14\x8b\x82\xaf\x99#\xdb\x05\x93Jn\xce\x9aq\x01\xcd\x8a\x9dG\xce'\x97\x80V\xb0\xf5\x90\x00!\x81\x8cEi\x8a\xfd\xd6\xcf\x8ako\x7f\xee\x1f\x0bCqoU\xe4\x13\xd4\xb8z`9",
    ),
}


@pytest.mark.parametrize("scheme", SCHEMES, ids=SCHEME_IDS)
def test_sign_matches_reference_vectors(scheme: type) -> None:
    secret1 = bytes([1] * 32)
    secret2 = bytes([x * 314159 % 256 for x in range(32)])
    sk1 = PrivateKey.from_bytes(secret1)
    sk2 = PrivateKey.from_bytes(secret2)
    msg = bytes([3, 1, 4, 1, 5, 9])

    sig1 = scheme.sign(sk1, msg)
    sig2 = scheme.sign(sk2, msg)
    sig_agg = scheme.aggregate([sig1, sig2])

    ref_sig1, ref_sig2, ref_sig_agg = REFERENCE_VECTORS[scheme]
    assert bytes(sig1) == ref_sig1
    assert bytes(sig2) == ref_sig2
    assert bytes(sig_agg) == ref_sig_agg


SAMPLES = [
    "aggregate_signatures",
    "creating_keys_and_signatures",
    "hd_keys",
    "loading_from_bytes",
    "proof_of_possession",
    "serializing_to_bytes",
    "tree_aggregates",
]


@pytest.mark.parametrize("sample", SAMPLES)
def test_samples(sample: str) -> None:
    module = importlib.import_module(sample)
    getattr(module, sample)()


def test_aggregate_verify_zero_items() -> None:
    assert AugSchemeMPL.aggregate_verify([], [], G2Element())


def test_from_message() -> None:
    msg = bytes([10]) * 32
    assert G2Element.from_message(msg, G2_DST) == BasicSchemeMPL.g2_from_message(msg)
    assert G1Element.from_message(msg, G1_DST) != G1Element()


def test_from_message_is_domain_separated() -> None:
    """The tag keeps one scheme's hashes off another's, so a different tag over
    the same message must land elsewhere."""
    msg = bytes([10]) * 32
    assert G2Element.from_message(msg, G2_DST) != G2Element.from_message(msg, G1_DST)
    assert G1Element.from_message(msg, G1_DST) != G1Element.from_message(msg, G2_DST)


@pytest.mark.parametrize("element", [G1Element, G2Element])
@pytest.mark.parametrize("size", [256, 1000])
def test_from_message_rejects_oversized_dst(element: type, size: int) -> None:
    """md_xmd caps the tag at 255 bytes but compares signed, so the length is
    checked here rather than reaching relic."""
    with pytest.raises(ValueError):
        element.from_message(bytes([10]) * 32, b"x" * size)


def test_from_message_accepts_maximum_dst() -> None:
    assert G2Element.from_message(bytes([10]) * 32, b"x" * 255) != G2Element()


class _LyingBytes(bytes):
    """A bytes subclass whose __len__ disagrees with its payload."""

    def __init__(self, payload: bytes, claimed_len: int) -> None:
        del payload
        self._claimed_len = claimed_len

    def __new__(cls, payload: bytes, claimed_len: int) -> "_LyingBytes":
        del claimed_len
        return super().__new__(cls, payload)

    def __len__(self) -> int:
        return self._claimed_len


@pytest.mark.parametrize("element", [G1Element, G2Element], ids=["G1Element", "G2Element"])
def test_from_message_measures_the_dst_payload_not_its_len(element: type) -> None:
    """The length check has to read the same bytes the copy takes, or a subclass
    that understates __len__ walks an oversized tag straight into relic."""
    dst = _LyingBytes(b"x" * 256, 1)
    assert len(dst) == 1
    with pytest.raises(ValueError):
        element.from_message(bytes([10]) * 32, dst)

    # and the mirror case: a small tag must not be rejected for lying upwards
    honest = G1_DST if element is G1Element else G2_DST
    assert element.from_message(bytes([10]) * 32, _LyingBytes(honest, 300)) == element.from_message(
        bytes([10]) * 32, honest
    )


def test_length_checked_paths_use_the_payload_of_a_bytes_subclass() -> None:
    """Every entry point that measures a length before copying must agree with
    the payload it ends up hashing, whatever __len__ claims."""
    assert Util.hash256(_LyingBytes(b"abc", 1 << 40)) == Util.hash256(b"abc")

    seed = bytes([9]) * 32
    for scheme in (BasicSchemeMPL, AugSchemeMPL, PopSchemeMPL):
        sk = scheme.key_gen(_LyingBytes(seed, 1))
        assert sk == scheme.key_gen(seed)
        msg = bytes([1, 2, 3])
        assert scheme.sign(sk, _LyingBytes(msg, 1 << 40)) == scheme.sign(sk, msg)
        assert scheme.verify(sk.get_g1(), _LyingBytes(msg, 1), scheme.sign(sk, msg))


@pytest.mark.parametrize("scheme", SCHEMES, ids=SCHEME_IDS)
def test_key_gen_rejects_oversized_seeds(scheme: type) -> None:
    """relic is built ALLOC=AUTO, so md_hmac stages the seed with alloca and a
    seed of a few MiB would take the process down instead of raising."""
    with pytest.raises(ValueError):
        scheme.key_gen(b"x" * (SEED_LIMIT + 1))
    # the cap itself has to keep working
    assert scheme.key_gen(b"x" * SEED_LIMIT) == scheme.key_gen(b"x" * SEED_LIMIT)


def test_from_bytes_and_from_bytes_unchecked_agree_on_valid_point() -> None:
    sk1 = BasicSchemeMPL.key_gen(b"1" * 32)
    good_point_bytes = bytes(sk1.get_g1())
    assert G1Element.from_bytes(good_point_bytes) == G1Element.from_bytes_unchecked(
        good_point_bytes
    )


def test_from_bytes_rejects_invalid_g1_point() -> None:
    bad_point_hex = "8d5d0fb73b9c92df4eab4216e48c3e358578b4cc30f82c268bd6fef3bd34b558628daf1afef798d4c3b0fcd8b28c8973"  # noqa: E501
    with pytest.raises(ValueError):
        G1Element.from_bytes(bytes.fromhex(bad_point_hex))
    # from_bytes_unchecked skips subgroup validation and must not raise
    G1Element.from_bytes_unchecked(bytes.fromhex(bad_point_hex))


def test_from_bytes_rejects_invalid_g2_point() -> None:
    bad_g2_point_hex = "8f2886c94eaeac335c8414cbf14c16681b225380cfee3293becc4531d5b415984b4ea4050d9ecda11fbc21c60627e9d212dfcb17d2b5ae399aa3fbcb099e05baa496b852ad976fb633cc6766b02fca4da549dc063908463b2906ad64e8b310ad"  # noqa: E501
    with pytest.raises(ValueError):
        G2Element.from_bytes(bytes.fromhex(bad_g2_point_hex))


@pytest.mark.parametrize(
    ("cls", "size"),
    [
        (PrivateKey, PrivateKey.PRIVATE_KEY_SIZE),
        (G1Element, G1Element.SIZE),
        (G2Element, G2Element.SIZE),
        (GTElement, GTElement.SIZE),
    ],
    ids=["PrivateKey", "G1Element", "G2Element", "GTElement"],
)
def test_from_bytes_rejects_non_contiguous_buffers(cls: type, size: int) -> None:
    # A reversed view points at the last backing byte with a stride of -1, so
    # reading it as if it were contiguous runs off the end of the allocation
    with pytest.raises(BufferError):
        cls.from_bytes(memoryview(bytearray(size))[::-1])
    # A strided view stays in bounds but is not the bytes the caller passed
    with pytest.raises(BufferError):
        cls.from_bytes(memoryview(bytearray(size * 2))[::2])


@pytest.mark.parametrize(
    ("cls", "size"),
    [
        (G1Element, G1Element.SIZE),
        (G2Element, G2Element.SIZE),
        (GTElement, GTElement.SIZE),
    ],
    ids=["G1Element", "G2Element", "GTElement"],
)
def test_from_bytes_unchecked_rejects_non_contiguous_buffers(cls: type, size: int) -> None:
    with pytest.raises(BufferError):
        cls.from_bytes_unchecked(memoryview(bytearray(size))[::-1])


@pytest.mark.parametrize("element", [G1Element, G2Element], ids=["G1Element", "G2Element"])
def test_from_message_accepts_a_large_message(element: type) -> None:
    # ep_map_dst takes the message length as an int and relic checks nothing on
    # its side, so the binds guard it. Anything under the limit has to keep
    # working; the limit itself is not testable without ~2 GiB of memory.
    assert element.from_message(b"x" * (1 << 20), G1_DST) != element()
