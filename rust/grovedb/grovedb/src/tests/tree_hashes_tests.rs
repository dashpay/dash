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

//! Tree hashes tests

use grovedb_merk::tree::{
    combine_hash, kv::ValueDefinedCostType, kv_digest_to_kv_hash, node_hash, value_hash, NULL_HASH,
};
use grovedb_storage::StorageBatch;
use grovedb_version::version::GroveVersion;

use crate::{
    tests::{make_test_grovedb, TEST_LEAF},
    Element,
};

#[test]
fn test_node_hashes_when_inserting_item() {
    let grove_version = GroveVersion::latest();
    let db = make_test_grovedb(grove_version);

    db.insert(
        [TEST_LEAF].as_ref(),
        b"key1",
        Element::new_item(b"baguette".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("successful subtree insert");

    let batch = StorageBatch::new();
    let transaction = db.start_transaction();

    let test_leaf_merk = db
        .open_transactional_merk_at_path(
            [TEST_LEAF].as_ref().into(),
            &transaction,
            Some(&batch),
            grove_version,
        )
        .unwrap()
        .expect("should open merk");

    let (elem_value, elem_value_hash) = test_leaf_merk
        .get_value_and_value_hash(
            b"key1",
            true,
            None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
            grove_version,
        )
        .unwrap()
        .expect("should get value hash")
        .expect("value hash should be some");

    let elem_kv_hash = test_leaf_merk
        .get_kv_hash(
            b"key1",
            true,
            None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
            grove_version,
        )
        .unwrap()
        .expect("should get value hash")
        .expect("value hash should be some");

    let elem_node_hash = test_leaf_merk
        .get_hash(
            b"key1",
            true,
            None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
            grove_version,
        )
        .unwrap()
        .expect("should get value hash")
        .expect("value hash should be some");

    let actual_value_hash = value_hash(&elem_value).unwrap();

    assert_eq!(elem_value_hash, actual_value_hash);

    let kv_hash = kv_digest_to_kv_hash(b"key1", &elem_value_hash).unwrap();

    assert_eq!(elem_kv_hash, kv_hash);

    let node_hash = node_hash(&kv_hash, &NULL_HASH, &NULL_HASH).unwrap();

    assert_eq!(elem_node_hash, node_hash);
}

#[test]
fn test_tree_hashes_when_inserting_empty_tree() {
    let grove_version = GroveVersion::latest();
    let db = make_test_grovedb(grove_version);

    db.insert(
        [TEST_LEAF].as_ref(),
        b"key1",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("successful subtree insert");

    let batch = StorageBatch::new();
    let transaction = db.start_transaction();

    let test_leaf_merk = db
        .open_transactional_merk_at_path(
            [TEST_LEAF].as_ref().into(),
            &transaction,
            Some(&batch),
            grove_version,
        )
        .unwrap()
        .expect("should open merk");

    let (elem_value, elem_value_hash) = test_leaf_merk
        .get_value_and_value_hash(
            b"key1",
            true,
            None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
            grove_version,
        )
        .unwrap()
        .expect("should get value hash")
        .expect("value hash should be some");

    let elem_kv_hash = test_leaf_merk
        .get_kv_hash(
            b"key1",
            true,
            None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
            grove_version,
        )
        .unwrap()
        .expect("should get value hash")
        .expect("value hash should be some");

    let elem_node_hash = test_leaf_merk
        .get_hash(
            b"key1",
            true,
            None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
            grove_version,
        )
        .unwrap()
        .expect("should get value hash")
        .expect("value hash should be some");

    let underlying_merk = db
        .open_transactional_merk_at_path(
            [TEST_LEAF, b"key1"].as_ref().into(),
            &transaction,
            Some(&batch),
            grove_version,
        )
        .unwrap()
        .expect("should open merk");

    let root_hash = underlying_merk.root_hash().unwrap();

    let actual_value_hash = value_hash(&elem_value).unwrap();
    let combined_value_hash = combine_hash(&actual_value_hash, &root_hash).unwrap();

    assert_eq!(elem_value_hash, combined_value_hash);

    let kv_hash = kv_digest_to_kv_hash(b"key1", &elem_value_hash).unwrap();

    assert_eq!(elem_kv_hash, kv_hash);

    let node_hash = node_hash(&kv_hash, &NULL_HASH, &NULL_HASH).unwrap();

    assert_eq!(elem_node_hash, node_hash);
}

#[test]
fn test_tree_hashes_when_inserting_empty_trees_twice_under_each_other() {
    let grove_version = GroveVersion::latest();
    let db = make_test_grovedb(grove_version);

    db.insert(
        [TEST_LEAF].as_ref(),
        b"key1",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("successful subtree insert");

    db.insert(
        [TEST_LEAF, b"key1"].as_ref(),
        b"key2",
        Element::empty_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("successful subtree insert");

    let batch = StorageBatch::new();
    let transaction = db.start_transaction();

    let under_top_merk = db
        .open_transactional_merk_at_path(
            [TEST_LEAF].as_ref().into(),
            &transaction,
            Some(&batch),
            grove_version,
        )
        .unwrap()
        .expect("should open merk");

    let middle_merk_key1 = db
        .open_transactional_merk_at_path(
            [TEST_LEAF, b"key1"].as_ref().into(),
            &transaction,
            Some(&batch),
            grove_version,
        )
        .unwrap()
        .expect("should open merk");

    // Let's first verify that the lowest nodes hashes are as we expect

    let (elem_value, elem_value_hash) = middle_merk_key1
        .get_value_and_value_hash(
            b"key2",
            true,
            None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
            grove_version,
        )
        .unwrap()
        .expect("should get value hash")
        .expect("value hash should be some");

    let bottom_merk = db
        .open_transactional_merk_at_path(
            [TEST_LEAF, b"key1", b"key2"].as_ref().into(),
            &transaction,
            Some(&batch),
            grove_version,
        )
        .unwrap()
        .expect("should open merk");

    let root_hash = bottom_merk.root_hash().unwrap();

    assert_eq!(root_hash, NULL_HASH);

    let actual_value_hash_key2 = value_hash(&elem_value).unwrap();
    let combined_value_hash_key2 = combine_hash(&actual_value_hash_key2, &root_hash).unwrap();

    assert_eq!(elem_value_hash, combined_value_hash_key2);

    let elem_kv_hash = middle_merk_key1
        .get_kv_hash(
            b"key2",
            true,
            None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
            grove_version,
        )
        .unwrap()
        .expect("should get kv hash")
        .expect("value hash should be some");

    let kv_hash_key2 = kv_digest_to_kv_hash(b"key2", &elem_value_hash).unwrap();

    assert_eq!(elem_kv_hash, kv_hash_key2);

    let elem_node_hash = middle_merk_key1
        .get_hash(
            b"key2",
            true,
            None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
            grove_version,
        )
        .unwrap()
        .expect("should get kv hash")
        .expect("value hash should be some");

    let node_hash_key2 = node_hash(&kv_hash_key2, &NULL_HASH, &NULL_HASH).unwrap();

    assert_eq!(elem_node_hash, node_hash_key2);

    // now lets verify the middle node

    let root_hash = middle_merk_key1.root_hash().unwrap();

    // the root hash should equal to the node_hash previously calculated

    assert_eq!(root_hash, node_hash_key2);

    let (middle_elem_value_key1, middle_elem_value_hash_key1) = under_top_merk
        .get_value_and_value_hash(
            b"key1",
            true,
            None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
            grove_version,
        )
        .unwrap()
        .expect("should get value hash")
        .expect("value hash should be some");

    assert_eq!(
        hex::encode(middle_elem_value_key1.as_slice()),
        "0201046b65793200"
    );

    let element = Element::deserialize(middle_elem_value_key1.as_slice(), grove_version)
        .expect("expected to deserialize element");

    assert_eq!(element, Element::new_tree(Some(b"key2".to_vec())));

    let actual_value_hash = value_hash(&middle_elem_value_key1).unwrap();

    assert_eq!(
        hex::encode(actual_value_hash),
        "06df974f1ea519344393e681f40dcb1b366b042416e663e0ba942ee2fd4b81f4"
    );
    assert_eq!(
        hex::encode(root_hash),
        "f0ba6963b5280da600c89b9684e6ab386ff6146fadfc21a98d52d5bb524c1bd9"
    );

    let combined_value_hash_key1 = combine_hash(&actual_value_hash, &root_hash).unwrap();

    assert_eq!(
        hex::encode(middle_elem_value_hash_key1),
        hex::encode(combined_value_hash_key1)
    );
    assert_eq!(
        hex::encode(middle_elem_value_hash_key1),
        "1e15cd574fd55c2471a864a6ea855e126ac0610ad99d96088c34438e2b22490b"
    );

    let middle_elem_kv_hash_key1 = under_top_merk
        .get_kv_hash(
            b"key1",
            true,
            None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
            grove_version,
        )
        .unwrap()
        .expect("should get value hash")
        .expect("value hash should be some");

    let kv_hash_key2 = kv_digest_to_kv_hash(b"key1", &combined_value_hash_key1).unwrap();

    assert_eq!(
        hex::encode(middle_elem_kv_hash_key1),
        hex::encode(kv_hash_key2),
        "middle kv hashes don't match"
    );

    let middle_elem_node_hash_key1 = under_top_merk
        .get_hash(
            b"key1",
            true,
            None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
            grove_version,
        )
        .unwrap()
        .expect("should get value hash")
        .expect("value hash should be some");

    let node_hash_key1 = node_hash(&kv_hash_key2, &NULL_HASH, &NULL_HASH).unwrap();

    assert_eq!(node_hash_key1, middle_elem_node_hash_key1);

    // now lets verify the middle node

    let root_hash = under_top_merk.root_hash().unwrap();

    // the root hash should equal to the node_hash previously calculated

    assert_eq!(root_hash, node_hash_key1);
}
