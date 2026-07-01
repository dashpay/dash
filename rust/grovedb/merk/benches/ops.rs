// MIT LICENSE
//
// Copyright (c) 2021 Dash Core Group
//
// Permission is hereby granted, free of charge, to any
// person obtaining a copy of this software and associated
// documentation files (the "Software"), to deal in the
// Software without restriction, including without
// limitation the rights to use, copy, modify, merge,
// publish, distribute, sublicense, and/or sell copies of
// the Software, and to permit persons to whom the Software
// is furnished to do so, subject to the following
// conditions:
//
// The above copyright notice and this permission notice
// shall be included in all copies or substantial portions
// of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF
// ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED
// TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
// PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT
// SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
// CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
// OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR
// IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.

//! Merk benches ops

use criterion::{criterion_group, criterion_main, Criterion};
use grovedb_merk::{
    owner::Owner,
    test_utils::{
        apply_memonly_unchecked, make_batch_rand, make_batch_seq, make_tree_rand, make_tree_seq,
    },
};

/// 1m sequential inserts in 10k batches, memonly
fn insert_1m_10k_seq_memonly(c: &mut Criterion) {
    let initial_size = 1_000_000;
    let batch_size = 10_000;
    let n_batches = initial_size / batch_size;

    let mut tree = Owner::new(make_tree_seq(initial_size, grove_version));

    let mut batches = Vec::new();
    for i in 0..n_batches {
        batches.push(make_batch_seq((i * batch_size)..((i + 1) * batch_size)));
    }

    c.bench_function("insert_1m_10k_seq_memonly", |b| {
        let mut i = 0;

        b.iter(|| {
            let batch = &batches[i % n_batches as usize];
            tree.own(|tree| apply_memonly_unchecked(tree, batch, grove_version));
            i += 1;
        });
    });
}

/// 1m random inserts in 10k batches, memonly
fn insert_1m_10k_rand_memonly(c: &mut Criterion) {
    let initial_size = 1_000_000;
    let batch_size = 10_000;
    let n_batches = initial_size / batch_size;

    let mut tree = Owner::new(make_tree_rand(
        initial_size,
        batch_size,
        0,
        false,
        grove_version,
    ));

    let mut batches = Vec::new();
    for i in 0..n_batches {
        batches.push(make_batch_rand(batch_size, i));
    }

    c.bench_function("insert_1m_10k_rand_memonly", |b| {
        let mut i = 0;

        b.iter(|| {
            let batch = &batches[i % n_batches as usize];
            tree.own(|tree| apply_memonly_unchecked(tree, batch, grove_version));
            i += 1;
        });
    });
}

/// 1m sequential updates in 10k batches, memonly
fn update_1m_10k_seq_memonly(c: &mut Criterion) {
    let initial_size = 1_000_000;
    let batch_size = 10_000;
    let n_batches = initial_size / batch_size;

    let mut tree = Owner::new(make_tree_seq(initial_size, grove_version));

    let mut batches = Vec::new();
    for i in 0..n_batches {
        let batch = make_batch_seq((i * batch_size)..((i + 1) * batch_size));
        tree.own(|tree| apply_memonly_unchecked(tree, &batch, grove_version));
        batches.push(batch);
    }

    c.bench_function("update_1m_10k_seq_memonly", |b| {
        let mut i = 0;

        b.iter(|| {
            let batch = &batches[i % n_batches as usize];
            tree.own(|tree| apply_memonly_unchecked(tree, batch, grove_version));
            i += 1;
        });
    });
}

/// 1m random updates in 10k batches, memonly
fn update_1m_10k_rand_memonly(c: &mut Criterion) {
    let initial_size = 1_000_000;
    let batch_size = 10_000;
    let n_batches = initial_size / batch_size;

    let mut tree = Owner::new(make_tree_rand(
        initial_size,
        batch_size,
        0,
        false,
        grove_version,
    ));

    let mut batches = Vec::new();
    for i in 0..n_batches {
        let batch = make_batch_rand(batch_size, i);
        tree.own(|tree| apply_memonly_unchecked(tree, &batch, grove_version));
        batches.push(batch);
    }

    c.bench_function("update_1m_10k_rand_memonly", |b| {
        let mut i = 0;

        b.iter(|| {
            let batch = &batches[i % n_batches as usize];
            tree.own(|tree| apply_memonly_unchecked(tree, batch, grove_version));
            i += 1;
        });
    });
}

criterion_group!(
    benches,
    insert_1m_10k_seq_memonly,
    insert_1m_10k_rand_memonly,
    update_1m_10k_seq_memonly,
    update_1m_10k_rand_memonly
);
criterion_main!(benches);
