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

//! Merk benches

use criterion::{criterion_group, criterion_main, BatchSize, Criterion};
use grovedb_costs::storage_cost::removal::StorageRemovedBytes::BasicStorageRemoval;
use grovedb_merk::{
    proofs,
    test_utils::{make_batch_rand, make_batch_seq, make_del_batch_rand, TempMerk},
    tree::kv::ValueDefinedCostType,
    Merk,
};
use grovedb_path::SubtreePath;
use grovedb_storage::{rocksdb_storage::test_utils::TempStorage, Storage};
use grovedb_version::version::GroveVersion;
use rand::prelude::*;

/// 1 million gets in 2k batches
pub fn get(c: &mut Criterion) {
    let grove_version = GroveVersion::latest();
    let initial_size = 1_000_000;
    let batch_size = 2_000;
    let num_batches = initial_size / batch_size;

    let mut merk = TempMerk::new(grove_version);

    let mut batches = vec![];
    for i in 0..num_batches {
        let batch = make_batch_rand(batch_size, i);
        merk.apply_unchecked::<_, Vec<u8>, _, _, _, _>(
            &batch,
            &[],
            None,
            &|_k, _v| Ok(0),
            None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
            &mut |_costs, _old_value, _value| Ok((false, None)),
            &mut |_a, key_bytes_to_remove, value_bytes_to_remove| {
                Ok((
                    BasicStorageRemoval(key_bytes_to_remove),
                    BasicStorageRemoval(value_bytes_to_remove),
                ))
            },
            grove_version,
        )
        .unwrap()
        .expect("apply failed");
        batches.push(batch);
    }

    c.bench_function("get", |b| {
        let mut i = 0;

        b.iter(|| {
            let batch_index = (i % num_batches) as usize;
            let key_index = (i / num_batches) as usize;

            let key = &batches[batch_index][key_index].0;
            merk.get(
                key,
                true,
                None::<fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                grove_version,
            )
            .unwrap()
            .expect("get failed");

            i = (i + 1) % initial_size;
        })
    });
}

/// 1 million sequential inserts in 2k batches
pub fn insert_1m_2k_seq(c: &mut Criterion) {
    let grove_version = GroveVersion::latest();
    let initial_size = 1_000_000;
    let batch_size = 2_000;

    let n_batches: usize = initial_size / batch_size;
    let mut batches = Vec::with_capacity(n_batches);

    for i in 0..n_batches {
        let batch = make_batch_seq(((i * batch_size) as u64)..((i + 1) * batch_size) as u64);
        batches.push(batch);
    }

    c.bench_function("insert_1m_2k_seq", |b| {
        let mut merk = TempMerk::new(grove_version);
        let mut i = 0;

        b.iter_with_large_drop(|| {
            let batch = &batches[i % n_batches];
            merk.apply_unchecked::<_, Vec<u8>, _, _, _, _>(
                batch,
                &[],
                None,
                &|_k, _v| Ok(0),
                None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                &mut |_costs, _old_value, _value| Ok((false, None)),
                &mut |_a, key_bytes_to_remove, value_bytes_to_remove| {
                    Ok((
                        BasicStorageRemoval(key_bytes_to_remove),
                        BasicStorageRemoval(value_bytes_to_remove),
                    ))
                },
                grove_version,
            )
            .unwrap()
            .expect("apply failed");
            i += 1;
        });
    });
}

/// 1 million random inserts in 2k batches
pub fn insert_1m_2k_rand(c: &mut Criterion) {
    let grove_version = GroveVersion::latest();
    let initial_size = 1_000_000;
    let batch_size = 2_000;

    let n_batches: usize = initial_size / batch_size;
    let mut batches = Vec::with_capacity(n_batches);

    for i in 0..n_batches {
        let batch = make_batch_rand(batch_size as u64, i as u64);
        batches.push(batch);
    }

    c.bench_function("insert_1m_2k_rand", |b| {
        let mut merk = TempMerk::new(grove_version);
        let mut i = 0;

        b.iter_with_large_drop(|| {
            let batch = &batches[i % n_batches];
            merk.apply_unchecked::<_, Vec<u8>, _, _, _, _>(
                batch,
                &[],
                None,
                &|_k, _v| Ok(0),
                None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                &mut |_costs, _old_value, _value| Ok((false, None)),
                &mut |_a, key_bytes_to_remove, value_bytes_to_remove| {
                    Ok((
                        BasicStorageRemoval(key_bytes_to_remove),
                        BasicStorageRemoval(value_bytes_to_remove),
                    ))
                },
                grove_version,
            )
            .unwrap()
            .expect("apply failed");
            i += 1;
        });
    });
}

/// 1 million sequential updates in 2k batches
pub fn update_1m_2k_seq(c: &mut Criterion) {
    let grove_version = GroveVersion::latest();
    let initial_size = 1_000_000;
    let batch_size = 2_000;

    let n_batches: usize = initial_size / batch_size;
    let mut batches = Vec::with_capacity(n_batches);

    let mut merk = TempMerk::new(grove_version);

    for i in 0..n_batches {
        let batch = make_batch_seq(((i * batch_size) as u64)..((i + 1) * batch_size) as u64);
        merk.apply_unchecked::<_, Vec<u8>, _, _, _, _>(
            &batch,
            &[],
            None,
            &|_k, _v| Ok(0),
            None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
            &mut |_costs, _old_value, _value| Ok((false, None)),
            &mut |_a, key_bytes_to_remove, value_bytes_to_remove| {
                Ok((
                    BasicStorageRemoval(key_bytes_to_remove),
                    BasicStorageRemoval(value_bytes_to_remove),
                ))
            },
            grove_version,
        )
        .unwrap()
        .expect("apply failed");

        batches.push(batch);
    }

    c.bench_function("update_1m_2k_seq", |b| {
        let mut i = 0;

        b.iter_with_large_drop(|| {
            let batch = &batches[i % n_batches];
            merk.apply_unchecked::<_, Vec<u8>, _, _, _, _>(
                batch,
                &[],
                None,
                &|_k, _v| Ok(0),
                None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                &mut |_costs, _old_value, _value| Ok((false, None)),
                &mut |_a, key_bytes_to_remove, value_bytes_to_remove| {
                    Ok((
                        BasicStorageRemoval(key_bytes_to_remove),
                        BasicStorageRemoval(value_bytes_to_remove),
                    ))
                },
                grove_version,
            )
            .unwrap()
            .expect("apply failed");
            i += 1;
        });
    });
}

/// 1 million random updates in 2k batches
pub fn update_1m_2k_rand(c: &mut Criterion) {
    let grove_version = GroveVersion::latest();
    let initial_size = 1_000_000;
    let batch_size = 2_000;

    let n_batches: usize = initial_size / batch_size;
    let mut batches = Vec::with_capacity(n_batches);

    let mut merk = TempMerk::new(grove_version);

    for i in 0..n_batches {
        let batch = make_batch_rand(batch_size as u64, i as u64);
        merk.apply_unchecked::<_, Vec<u8>, _, _, _, _>(
            &batch,
            &[],
            None,
            &|_k, _v| Ok(0),
            None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
            &mut |_costs, _old_value, _value| Ok((false, None)),
            &mut |_a, key_bytes_to_remove, value_bytes_to_remove| {
                Ok((
                    BasicStorageRemoval(key_bytes_to_remove),
                    BasicStorageRemoval(value_bytes_to_remove),
                ))
            },
            grove_version,
        )
        .unwrap()
        .expect("apply failed");

        batches.push(batch);
    }

    c.bench_function("update_1m_2k_rand", |b| {
        let mut i = 0;

        b.iter_with_large_drop(|| {
            let batch = &batches[i % n_batches];
            merk.apply_unchecked::<_, Vec<u8>, _, _, _, _>(
                batch,
                &[],
                None,
                &|_k, _v| Ok(0),
                None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                &mut |_costs, _old_value, _value| Ok((false, None)),
                &mut |_a, key_bytes_to_remove, value_bytes_to_remove| {
                    Ok((
                        BasicStorageRemoval(key_bytes_to_remove),
                        BasicStorageRemoval(value_bytes_to_remove),
                    ))
                },
                grove_version,
            )
            .unwrap()
            .expect("apply failed");
            i += 1;
        });
    });
}

/// 1 million random deletes in 2k batches
pub fn delete_1m_2k_rand(c: &mut Criterion) {
    let grove_version = GroveVersion::latest();
    let initial_size = 1_000_000;
    let batch_size = 2_000;

    let n_batches: usize = initial_size / batch_size;
    let mut batches = Vec::with_capacity(n_batches);
    let mut delete_batches = Vec::with_capacity(n_batches);

    let mut merk = TempMerk::new(grove_version);

    for i in 0..n_batches {
        let batch = make_batch_rand(batch_size as u64, i as u64);
        let delete_batch = make_del_batch_rand(batch_size as u64, i as u64);
        merk.apply_unchecked::<_, Vec<u8>, _, _, _, _>(
            &batch,
            &[],
            None,
            &|_k, _v| Ok(0),
            None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
            &mut |_costs, _old_value, _value| Ok((false, None)),
            &mut |_a, key_bytes_to_remove, value_bytes_to_remove| {
                Ok((
                    BasicStorageRemoval(key_bytes_to_remove),
                    BasicStorageRemoval(value_bytes_to_remove),
                ))
            },
            grove_version,
        )
        .unwrap()
        .expect("apply failed");

        batches.push(batch);
        delete_batches.push(delete_batch);
    }

    c.bench_function("delete_1m_2k_rand", |b| {
        let mut i = 0;

        let delete_batch = &delete_batches[i % n_batches];
        let insert_batch = &batches[i % n_batches];

        // Merk tree is kept with 1m elements before each bench iteration for more or
        // less same inputs.
        merk.apply_unchecked::<_, Vec<u8>, _, _, _, _>(
            insert_batch,
            &[],
            None,
            &|_k, _v| Ok(0),
            None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
            &mut |_costs, _old_value, _value| Ok((false, None)),
            &mut |_a, key_bytes_to_remove, value_bytes_to_remove| {
                Ok((
                    BasicStorageRemoval(key_bytes_to_remove),
                    BasicStorageRemoval(value_bytes_to_remove),
                ))
            },
            grove_version,
        )
        .unwrap()
        .expect("apply failed");

        b.iter_with_large_drop(|| {
            merk.apply_unchecked::<_, Vec<u8>, _, _, _, _>(
                delete_batch,
                &[],
                None,
                &|_k, _v| Ok(0),
                None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                &mut |_costs, _old_value, _value| Ok((false, None)),
                &mut |_a, key_bytes_to_remove, value_bytes_to_remove| {
                    Ok((
                        BasicStorageRemoval(key_bytes_to_remove),
                        BasicStorageRemoval(value_bytes_to_remove),
                    ))
                },
                grove_version,
            )
            .unwrap()
            .expect("apply failed");
            i += 1;
        });
    });
}

/// 1 million random proofs in 2k batches
pub fn prove_1m_2k_rand(c: &mut Criterion) {
    let grove_version = GroveVersion::latest();
    let initial_size = 1_000_000;
    let batch_size = 2_000;

    let n_batches: usize = initial_size / batch_size;
    let mut batches = Vec::with_capacity(n_batches);
    let mut prove_keys_per_batch = Vec::with_capacity(n_batches);

    let mut merk = TempMerk::new(grove_version);

    for i in 0..n_batches {
        let batch = make_batch_rand(batch_size as u64, i as u64);
        merk.apply_unchecked::<_, Vec<u8>, _, _, _, _>(
            &batch,
            &[],
            None,
            &|_k, _v| Ok(0),
            None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
            &mut |_costs, _old_value, _value| Ok((false, None)),
            &mut |_a, key_bytes_to_remove, value_bytes_to_remove| {
                Ok((
                    BasicStorageRemoval(key_bytes_to_remove),
                    BasicStorageRemoval(value_bytes_to_remove),
                ))
            },
            grove_version,
        )
        .unwrap()
        .expect("apply failed");
        let mut prove_keys = Vec::with_capacity(batch_size);
        for (key, _) in batch.iter() {
            prove_keys.push(proofs::query::query_item::QueryItem::Key(key.clone()));
        }
        prove_keys_per_batch.push(prove_keys);
        batches.push(batch);
    }

    c.bench_function("prove_1m_2k_rand", |b| {
        let mut i = 0;

        b.iter_with_large_drop(|| {
            let keys = prove_keys_per_batch[i % n_batches].clone();

            merk.prove_unchecked(keys, None, true, grove_version)
                .unwrap()
                .expect("prove failed");
            i += 1;
        });
    });
}

/// Build 1 million trunk chunks in 2k batches, random
pub fn build_trunk_chunk_1m_2k_rand(c: &mut Criterion) {
    let grove_version = GroveVersion::latest();
    let initial_size = 1_000_000;
    let batch_size = 2_000;

    let n_batches: usize = initial_size / batch_size;

    let mut merk = TempMerk::new(grove_version);

    for i in 0..n_batches {
        let batch = make_batch_rand(batch_size as u64, i as u64);
        merk.apply_unchecked::<_, Vec<u8>, _, _, _, _>(
            &batch,
            &[],
            None,
            &|_k, _v| Ok(0),
            None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
            &mut |_costs, _old_value, _value| Ok((false, None)),
            &mut |_a, key_bytes_to_remove, value_bytes_to_remove| {
                Ok((
                    BasicStorageRemoval(key_bytes_to_remove),
                    BasicStorageRemoval(value_bytes_to_remove),
                ))
            },
            grove_version,
        )
        .unwrap()
        .expect("apply failed")
    }

    c.bench_function("build_trunk_chunk_1m_2k_rand", |b| {
        let mut bytes = Vec::new();

        b.iter(|| {
            bytes.clear();

            let (ops, _) =
                merk.walk(|walker| walker.unwrap().create_trunk_proof().unwrap().unwrap());
            proofs::encode_into(ops.iter(), &mut bytes);
        });
    });
}

/// Chunk producer random 1 million
pub fn chunkproducer_rand_1m_1_rand(c: &mut Criterion) {
    let grove_version = GroveVersion::latest();
    let initial_size = 1_000_000;
    let batch_size = 2_000;

    let n_batches: usize = initial_size / batch_size;

    let mut merk = TempMerk::new(grove_version);

    for i in 0..n_batches {
        let batch = make_batch_rand(batch_size as u64, i as u64);
        merk.apply_unchecked::<_, Vec<u8>, _, _, _, _>(
            &batch,
            &[],
            None,
            &|_k, _v| Ok(0),
            None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
            &mut |_costs, _old_value, _value| Ok((false, None)),
            &mut |_a, key_bytes_to_remove, value_bytes_to_remove| {
                Ok((
                    BasicStorageRemoval(key_bytes_to_remove),
                    BasicStorageRemoval(value_bytes_to_remove),
                ))
            },
            grove_version,
        )
        .unwrap()
        .expect("apply failed")
    }

    let mut rng = rand::thread_rng();
    let mut chunks = merk.chunks().unwrap();

    c.bench_function("chunkproducer_rand_1m_1_rand", |b| {
        b.iter_with_large_drop(|| {
            let i = rng.gen::<usize>() % chunks.len();
            let _chunk = chunks.chunk(i, grove_version).unwrap();
        });
    });
}

/// Chunk iter 1 million
pub fn chunk_iter_1m_1(c: &mut Criterion) {
    let grove_version = GroveVersion::latest();
    let initial_size = 1_000_000;
    let batch_size = 2_000;

    let n_batches: usize = initial_size / batch_size;

    let mut merk = TempMerk::new(grove_version);

    for i in 0..n_batches {
        let batch = make_batch_rand(batch_size as u64, i as u64);
        merk.apply_unchecked::<_, Vec<u8>, _, _, _, _>(
            &batch,
            &[],
            None,
            &|_k, _v| Ok(0),
            None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
            &mut |_costs, _old_value, _value| Ok((false, None)),
            &mut |_a, key_bytes_to_remove, value_bytes_to_remove| {
                Ok((
                    BasicStorageRemoval(key_bytes_to_remove),
                    BasicStorageRemoval(value_bytes_to_remove),
                ))
            },
            grove_version,
        )
        .unwrap()
        .expect("apply failed")
    }

    let mut chunks = merk.chunks().unwrap().into_iter();

    let mut next = || match chunks.next(grove_version) {
        Some(chunk) => chunk,
        None => {
            chunks = merk.chunks().unwrap().into_iter();
            chunks.next(grove_version).unwrap()
        }
    };

    c.bench_function("chunk_iter_1m_1", |b| {
        b.iter_with_large_drop(|| {
            let _chunk = next();
        });
    });
}

/// Restore merk of size 500
pub fn restore_500_1(c: &mut Criterion) {
    let grove_version = GroveVersion::latest();
    let merk_size = 500;

    let mut merk = TempMerk::new(grove_version);

    let batch = make_batch_rand(merk_size as u64, 0_u64);
    merk.apply_unchecked::<_, Vec<u8>, _, _, _, _>(
        &batch,
        &[],
        None,
        &|_k, _v| Ok(0),
        None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
        &mut |_costs, _old_value, _value| Ok((false, None)),
        &mut |_a, key_bytes_to_remove, value_bytes_to_remove| {
            Ok((
                BasicStorageRemoval(key_bytes_to_remove),
                BasicStorageRemoval(value_bytes_to_remove),
            ))
        },
        grove_version,
    )
    .unwrap()
    .expect("apply failed");

    let root_hash = merk.root_hash().unwrap();

    c.bench_function("restore_500_1", |b| {
        b.iter_batched(
            || {
                let storage = TempStorage::new();
                (storage, merk.chunks().unwrap().into_iter())
            },
            |data| {
                let tx = data.0.start_transaction();
                let ctx = data
                    .0
                    .get_immediate_storage_context(SubtreePath::empty(), &tx)
                    .unwrap();
                let m = Merk::open_standalone(
                    ctx,
                    false,
                    None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                    grove_version,
                )
                .unwrap()
                .unwrap();
                let mut restorer = Merk::restore(m, root_hash);

                for chunk in data.1 {
                    restorer.process_chunk(chunk.unwrap()).unwrap();
                }
                data.0.commit_transaction(tx).unwrap().unwrap();
            },
            BatchSize::SmallInput,
        );
    });
}

criterion_group!(
    benches,
    get,
    insert_1m_2k_seq,
    insert_1m_2k_rand,
    update_1m_2k_seq,
    update_1m_2k_rand,
    delete_1m_2k_rand,
    prove_1m_2k_rand,
    build_trunk_chunk_1m_2k_rand,
    chunkproducer_rand_1m_1_rand,
    chunk_iter_1m_1,
    restore_500_1,
);
criterion_main!(benches);
