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

//! Insertion Benchmark

#[cfg(feature = "minimal")]
use criterion::{criterion_group, criterion_main, Criterion};
#[cfg(feature = "minimal")]
use grovedb::{Element, GroveDb};
use grovedb_path::SubtreePath;
#[cfg(feature = "minimal")]
use rand::Rng;
#[cfg(feature = "minimal")]
use tempfile::TempDir;

#[cfg(feature = "minimal")]
const N_ITEMS: usize = 10_000;

const EMPTY_PATH: SubtreePath<'static, [u8; 0]> = SubtreePath::empty();

/// Benchmark function to insert '''N_ITEMS''' key-values into an empty tree
/// without a transaction
#[cfg(feature = "minimal")]
pub fn insertion_benchmark_without_transaction(c: &mut Criterion) {
    let dir = TempDir::new().unwrap();
    let db = GroveDb::open(dir.path()).unwrap();
    let test_leaf: &[u8] = b"leaf1";
    db.insert(
        EMPTY_PATH,
        test_leaf,
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .unwrap();
    let keys = std::iter::repeat_with(|| rand::thread_rng().gen::<[u8; 32]>()).take(N_ITEMS);

    c.bench_function("scalars insertion without transaction", |b| {
        b.iter(|| {
            for k in keys.clone() {
                db.insert(
                    [test_leaf].as_ref(),
                    &k,
                    Element::new_item(k.to_vec()),
                    None,
                    None,
                    grove_version,
                )
                .unwrap()
                .unwrap();
            }
        })
    });
}

/// Benchmark function to insert '''N_ITEMS''' key-values into an empty tree
/// with a transaction
#[cfg(feature = "minimal")]
pub fn insertion_benchmark_with_transaction(c: &mut Criterion) {
    let dir = TempDir::new().unwrap();
    let db = GroveDb::open(dir.path()).unwrap();
    let test_leaf: &[u8] = b"leaf1";
    db.insert(
        EMPTY_PATH,
        test_leaf,
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .unwrap();
    let keys = std::iter::repeat_with(|| rand::thread_rng().gen::<[u8; 32]>()).take(N_ITEMS);

    c.bench_function("scalars insertion with transaction", |b| {
        b.iter(|| {
            let tx = db.start_transaction();
            for k in keys.clone() {
                db.insert(
                    [test_leaf].as_ref(),
                    &k,
                    Element::new_item(k.to_vec()),
                    None,
                    Some(&tx),
                    grove_version,
                )
                .unwrap()
                .unwrap();
            }
            db.commit_transaction(tx).unwrap().unwrap();
        })
    });
}

/// Benchmark function to insert 10 root leaves without a transaction
#[cfg(feature = "minimal")]
pub fn root_leaf_insertion_benchmark_without_transaction(c: &mut Criterion) {
    let dir = TempDir::new().unwrap();
    let db = GroveDb::open(dir.path()).unwrap();
    let keys = std::iter::repeat_with(|| rand::thread_rng().gen::<[u8; 32]>()).take(10);

    c.bench_function("root leaves insertion without transaction", |b| {
        b.iter(|| {
            for k in keys.clone() {
                db.insert(
                    EMPTY_PATH,
                    &k,
                    Element::empty_tree(),
                    None,
                    None,
                    grove_version,
                )
                .unwrap()
                .unwrap();
            }
        })
    });
}

/// Benchmark function to insert 10 root leaves with a transaction
#[cfg(feature = "minimal")]
pub fn root_leaf_insertion_benchmark_with_transaction(c: &mut Criterion) {
    let dir = TempDir::new().unwrap();
    let db = GroveDb::open(dir.path()).unwrap();
    let keys = std::iter::repeat_with(|| rand::thread_rng().gen::<[u8; 32]>()).take(10);

    c.bench_function("root leaves insertion with transaction", |b| {
        b.iter(|| {
            let tx = db.start_transaction();
            for k in keys.clone() {
                db.insert(
                    EMPTY_PATH,
                    &k,
                    Element::empty_tree(),
                    None,
                    Some(&tx),
                    grove_version,
                )
                .unwrap()
                .unwrap();
            }
            db.commit_transaction(tx).unwrap().unwrap();
        })
    });
}

/// Benchmark function to insert a subtree nested within 10 higher subtrees
/// and insert key-values into it without a transaction
#[cfg(feature = "minimal")]
pub fn deeply_nested_insertion_benchmark_without_transaction(c: &mut Criterion) {
    let dir = TempDir::new().unwrap();
    let db = GroveDb::open(dir.path()).unwrap();
    let mut nested_subtrees: Vec<[u8; 32]> = Vec::new();
    for s in std::iter::repeat_with(|| rand::thread_rng().gen::<[u8; 32]>()).take(10) {
        db.insert(
            nested_subtrees.as_slice(),
            &s,
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .unwrap();
        nested_subtrees.push(s);
    }

    let keys = std::iter::repeat_with(|| rand::thread_rng().gen::<[u8; 32]>()).take(N_ITEMS);

    c.bench_function("deeply nested scalars insertion without transaction", |b| {
        b.iter(|| {
            for k in keys.clone() {
                db.insert(
                    nested_subtrees.as_slice(),
                    &k,
                    Element::new_item(k.to_vec()),
                    None,
                    None,
                    grove_version,
                )
                .unwrap()
                .unwrap();
            }
        })
    });
}

/// Benchmark function to insert a subtree nested within 10 higher subtrees
/// and insert key-values into it with a transaction
#[cfg(feature = "minimal")]
pub fn deeply_nested_insertion_benchmark_with_transaction(c: &mut Criterion) {
    let dir = TempDir::new().unwrap();
    let db = GroveDb::open(dir.path()).unwrap();
    let mut nested_subtrees: Vec<[u8; 32]> = Vec::new();
    for s in std::iter::repeat_with(|| rand::thread_rng().gen::<[u8; 32]>()).take(10) {
        db.insert(
            nested_subtrees.as_slice(),
            &s,
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .unwrap();
        nested_subtrees.push(s);
    }

    let keys = std::iter::repeat_with(|| rand::thread_rng().gen::<[u8; 32]>()).take(N_ITEMS);

    c.bench_function("deeply nested scalars insertion with transaction", |b| {
        b.iter(|| {
            let tx = db.start_transaction();
            for k in keys.clone() {
                db.insert(
                    nested_subtrees.as_slice(),
                    &k,
                    Element::new_item(k.to_vec()),
                    None,
                    Some(&tx),
                    grove_version,
                )
                .unwrap()
                .unwrap();
            }
            db.commit_transaction(tx).unwrap().unwrap();
        })
    });
}

#[cfg(feature = "minimal")]
criterion_group!(
    benches,
    insertion_benchmark_without_transaction,
    insertion_benchmark_with_transaction,
    root_leaf_insertion_benchmark_without_transaction,
    root_leaf_insertion_benchmark_with_transaction,
    deeply_nested_insertion_benchmark_without_transaction,
    deeply_nested_insertion_benchmark_with_transaction,
);
#[cfg(feature = "minimal")]
criterion_main!(benches);
