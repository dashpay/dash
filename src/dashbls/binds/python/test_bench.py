#!/usr/bin/env python3
# coding: latin-1

#
# Copyright (c) 2020-present, Chia Network Inc.
# Copyright (c) 2026-present, The Dash Core developers
# SPDX-License-Identifier: Apache-2.0
# See the accompanying file LICENSE or https://opensource.org/licenses/Apache-2.0
#

"""Benchmarks for the dashbls package."""

import secrets

import pytest
from dashbls import (
    AugSchemeMPL,
    G1Element,
    G2Element,
    PrivateKey,
)
from pytest_benchmark.fixture import BenchmarkFixture

NUM_ITEMS = 1000
SignedMessages = tuple[list[bytes], list[bytes], list[bytes]]


@pytest.fixture(scope="module")
def signed_messages() -> SignedMessages:
    pk_bytes = []
    sig_bytes = []
    messages = []
    for i in range(NUM_ITEMS):
        message = b"%d" % i
        sk: PrivateKey = AugSchemeMPL.key_gen(secrets.token_bytes(32))
        pk: G1Element = sk.get_g1()
        sig: G2Element = AugSchemeMPL.sign(sk, message)

        pk_bytes.append(bytes(pk))
        sig_bytes.append(bytes(sig))
        messages.append(message)
    return pk_bytes, sig_bytes, messages


def test_public_key_validation(
    benchmark: BenchmarkFixture, signed_messages: SignedMessages
) -> None:
    pk_bytes, _, _ = signed_messages
    pks = benchmark(lambda: [G1Element.from_bytes(pk) for pk in pk_bytes])
    assert len(pks) == NUM_ITEMS


def test_signature_validation(benchmark: BenchmarkFixture, signed_messages: SignedMessages) -> None:
    _, sig_bytes, _ = signed_messages
    sigs = benchmark(lambda: [G2Element.from_bytes(sig) for sig in sig_bytes])
    assert len(sigs) == NUM_ITEMS


def test_signature_aggregation(
    benchmark: BenchmarkFixture, signed_messages: SignedMessages
) -> None:
    _, sig_bytes, _ = signed_messages
    sigs = [G2Element.from_bytes(sig) for sig in sig_bytes]
    agg_sig = benchmark(lambda: AugSchemeMPL.aggregate(sigs))
    assert agg_sig is not None


def test_batch_verification(benchmark: BenchmarkFixture, signed_messages: SignedMessages) -> None:
    pk_bytes, sig_bytes, messages = signed_messages
    pks = [G1Element.from_bytes(pk) for pk in pk_bytes]
    sigs = [G2Element.from_bytes(sig) for sig in sig_bytes]
    agg_sig = AugSchemeMPL.aggregate(sigs)

    ok = benchmark(lambda: AugSchemeMPL.aggregate_verify(pks, messages, agg_sig))
    assert ok
