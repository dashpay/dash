//! Tests

#[cfg(feature = "minimal")]
mod tests {
    use grovedb_costs::{
        storage_cost::{removal::StorageRemovedBytes::NoStorageRemoval, StorageCost},
        OperationCost,
    };
    use grovedb_version::version::GroveVersion;

    use crate::{
        batch::QualifiedGroveDbOp,
        tests::{common::EMPTY_PATH, make_empty_grovedb},
        Element,
    };

    #[test]
    fn test_batch_one_sum_item_insert_costs_match_non_batch() {
        let grove_version = GroveVersion::latest();
        let db = make_empty_grovedb();
        let tx = db.start_transaction();

        db.insert(
            EMPTY_PATH,
            b"sum_tree",
            Element::empty_sum_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("expected to insert sum tree");

        let non_batch_cost = db
            .insert(
                [b"sum_tree".as_slice()].as_ref(),
                b"key1",
                Element::new_sum_item(150),
                None,
                Some(&tx),
                grove_version,
            )
            .cost;
        tx.rollback().expect("expected to rollback");
        let ops = vec![QualifiedGroveDbOp::insert_or_replace_op(
            vec![b"sum_tree".to_vec()],
            b"key1".to_vec(),
            Element::new_sum_item(150),
        )];
        let cost = db.apply_batch(ops, None, Some(&tx), grove_version).cost;
        assert_eq!(non_batch_cost.storage_cost, cost.storage_cost);
    }

    #[test]
    fn test_batch_one_insert_sum_tree_cost() {
        let grove_version = GroveVersion::latest();
        let db = make_empty_grovedb();
        let tx = db.start_transaction();

        let ops = vec![QualifiedGroveDbOp::insert_or_replace_op(
            vec![],
            b"key1".to_vec(),
            Element::empty_sum_tree(),
        )];
        let cost_result = db.apply_batch(ops, None, Some(&tx), grove_version);
        cost_result.value.expect("expected to execute batch");
        let cost = cost_result.cost;
        // Explanation for 124 storage_written_bytes

        // Key -> 37 bytes
        // 32 bytes for the key prefix
        // 4 bytes for the key
        // 1 byte for key_size (required space for 36)

        // Value -> 47
        //   1 for the flag option (but no flags)
        //   1 for the enum type
        //   1 for empty tree value
        //   9 for sum tree value
        //   1 for Basic Merk
        // 32 for node hash
        // 0 for value hash
        // 2 byte for the value_size (required space for 98 + up to 256 for child key)

        // Parent Hook -> 40
        // Key Bytes 4
        // Hash Size 32
        // Key Length 1
        // Child Heights 2
        // Sum 1
        // Total 37 + 38 + 40 = 115

        // Hash node calls
        // 1 for the tree insert
        // 2 for the node hash
        // 1 for the value hash
        // 1 for the combine hash
        // 1 kv_digest_to_kv_hash

        // Seek Count
        // 1 to load from root tree
        // 1 to insert
        // 1 to update root tree

        assert_eq!(
            cost,
            OperationCost {
                seek_count: 3,
                storage_cost: StorageCost {
                    added_bytes: 124,
                    replaced_bytes: 0,
                    removed_bytes: NoStorageRemoval,
                },
                storage_loaded_bytes: 0,
                hash_node_calls: 6,
            }
        );
    }

    #[test]
    fn test_batch_one_insert_sum_tree_under_parent_tree_in_same_merk_cost() {
        let grove_version = GroveVersion::latest();
        let db = make_empty_grovedb();
        let tx = db.start_transaction();

        db.insert(
            EMPTY_PATH,
            b"0",
            Element::empty_tree(),
            None,
            Some(&tx),
            grove_version,
        )
        .unwrap()
        .expect("successful root tree leaf insert");

        let ops = vec![QualifiedGroveDbOp::insert_or_replace_op(
            vec![],
            b"key1".to_vec(),
            Element::empty_sum_tree(),
        )];
        let cost_result = db.apply_batch(ops, None, Some(&tx), grove_version);
        cost_result.value.expect("expected to execute batch");
        let cost = cost_result.cost;
        // Explanation for 124 storage_written_bytes

        // Key -> 37 bytes
        // 32 bytes for the key prefix
        // 4 bytes for the key
        // 1 byte for key_size (required space for 36)

        // Value -> 47
        //   1 for the flag option (but no flags)
        //   1 for the enum type
        //   1 for empty tree value
        //   9 for sum tree value
        //   1 for Basic merk
        // 32 for node hash
        // 0 for value hash
        // 2 byte for the value_size (required space for 98 + up to 256 for child key)

        // Parent Hook -> 40
        // Key Bytes 4
        // Hash Size 32
        // Key Length 1
        // Child Heights 2
        // Sum 1
        // Total 37 + 38 + 40 = 115

        // Replaced bytes

        // 37 + 36 = 74 (key is not replaced) //needs update
        // We instead are getting 106, because we are paying for (+ hash - key byte
        // size) this means 31 extra bytes.
        // In reality though we really are replacing 106 bytes. TBD what to do.

        // Hash node calls 8
        // 1 to get tree hash
        // 2 for the node hash
        // 1 for the value hash
        // 1 for the kv_digest_to_kv_hash
        // 1 for the combine hash
        // 2 for the node hash above

        // Seek Count explanation
        // 1 to get root merk
        // 1 to load root tree
        // 1 to insert new item
        // 1 to replace parent tree
        // 1 to update root
        assert_eq!(
            cost,
            OperationCost {
                seek_count: 5,
                storage_cost: StorageCost {
                    added_bytes: 124,
                    replaced_bytes: 75,
                    removed_bytes: NoStorageRemoval,
                },
                storage_loaded_bytes: 71, // todo: verify and explain
                hash_node_calls: 8,
            }
        );
    }

    #[test]
    fn test_batch_one_insert_sum_tree_under_parent_sum_tree_in_same_merk_cost() {
        let grove_version = GroveVersion::latest();
        let db = make_empty_grovedb();
        let tx = db.start_transaction();

        db.insert(
            EMPTY_PATH,
            b"0",
            Element::empty_sum_tree(),
            None,
            Some(&tx),
            grove_version,
        )
        .unwrap()
        .expect("successful root tree leaf insert");

        let ops = vec![QualifiedGroveDbOp::insert_or_replace_op(
            vec![],
            b"key1".to_vec(),
            Element::empty_sum_tree(),
        )];
        let cost_result = db.apply_batch(ops, None, Some(&tx), grove_version);
        cost_result.value.expect("expected to execute batch");
        let cost = cost_result.cost;
        // Explanation for 124 storage_written_bytes

        // Key -> 37 bytes
        // 32 bytes for the key prefix
        // 4 bytes for the key
        // 1 byte for key_size (required space for 36)

        // Value -> 47
        //   1 for the flag option (but no flags)
        //   1 for the enum type
        //   1 for empty tree value
        //   9 for sum tree value
        //   1 for Basic merk
        // 32 for node hash
        // 0 for value hash
        // 2 byte for the value_size (required space for 98 + up to 256 for child key)

        // Parent Hook -> 40
        // Key Bytes 4
        // Hash Size 32
        // Key Length 1
        // Child Heights 2
        // Sum 1
        // Total 37 + 38 + 40 = 115

        // Replaced bytes

        // 37 + 36 = 74 (key is not replaced) //needs update
        // We instead are getting 107, because we are paying for (+ hash - key byte
        // size) this means 31 extra bytes.
        // In reality though we really are replacing 107 bytes. TBD what to do.

        // Hash node calls 8
        // 1 to get tree hash
        // 2 for the node hash
        // 1 for the value hash
        // 1 for the kv_digest_to_kv_hash
        // 1 for the combine hash
        // 2 for the node hash above

        // Seek Count explanation
        // 1 to get root merk
        // 1 to load root tree
        // 1 to insert new item
        // 1 to replace parent tree
        // 1 to update root
        assert_eq!(
            cost,
            OperationCost {
                seek_count: 5,
                storage_cost: StorageCost {
                    added_bytes: 124,
                    replaced_bytes: 84,
                    removed_bytes: NoStorageRemoval,
                },
                storage_loaded_bytes: 72, // todo: verify and explain
                hash_node_calls: 8,
            }
        );
    }

    #[test]
    fn test_batch_one_insert_sum_tree_under_parent_tree_in_different_merk_cost() {
        let grove_version = GroveVersion::latest();
        let db = make_empty_grovedb();
        let tx = db.start_transaction();

        db.insert(
            EMPTY_PATH,
            b"0",
            Element::empty_tree(),
            None,
            Some(&tx),
            grove_version,
        )
        .unwrap()
        .expect("successful root tree leaf insert");

        let ops = vec![QualifiedGroveDbOp::insert_or_replace_op(
            vec![b"0".to_vec()],
            b"key1".to_vec(),
            Element::empty_sum_tree(),
        )];
        let cost_result = db.apply_batch(ops, None, Some(&tx), grove_version);
        cost_result.value.expect("expected to execute batch");
        let cost = cost_result.cost;
        // Explanation for 124 storage_written_bytes

        // Key -> 37 bytes
        // 32 bytes for the key prefix
        // 4 bytes for the key
        // 1 byte for key_size (required space for 36)

        // Value -> 38
        //   1 for the flag option (but no flags)
        //   1 for the enum type
        //   1 for empty tree value
        //   9 for Sum value
        //   1 for BasicMerk
        // 32 for node hash
        // 0 for value hash
        // 2 byte for the value_size (required space for 98 + up to 256 for child key)

        // Parent Hook -> 40
        // Key Bytes 4
        // Hash Size 32
        // Key Length 1
        // Child Heights 2
        // Sum 1

        // Total 37 + 38 + 40 = 115

        // Replaced bytes

        // 37 + 38 = 75 (key is not replaced)

        //// Hash node calls 10
        // 1 to get the lowest merk
        //
        // 1 to get the middle merk
        // 2 for the node hash
        // 1 for the value hash
        // 1 for the combine hash
        // 1 for the kv_digest_to_kv_hash

        // On the layer above the root key did change
        // meaning we get another 5 hashes 2 + 1 + 1 + 1

        //// Seek Count explanation

        // 1 to get merk at lower level
        // 1 to insert new item
        // 1 to get root merk
        // 1 to load root tree
        // 1 to replace parent tree
        // 1 to update root
        assert_eq!(
            cost,
            OperationCost {
                seek_count: 6,
                storage_cost: StorageCost {
                    added_bytes: 124,
                    replaced_bytes: 75,
                    removed_bytes: NoStorageRemoval,
                },
                storage_loaded_bytes: 146, // todo: verify and explain
                hash_node_calls: 12,
            }
        );
    }

    #[test]
    fn test_batch_one_insert_sum_tree_under_parent_sum_tree_in_different_merk_cost() {
        let grove_version = GroveVersion::latest();
        let db = make_empty_grovedb();
        let tx = db.start_transaction();

        db.insert(
            EMPTY_PATH,
            b"0",
            Element::empty_sum_tree(),
            None,
            Some(&tx),
            grove_version,
        )
        .unwrap()
        .expect("successful root tree leaf insert");

        let ops = vec![QualifiedGroveDbOp::insert_or_replace_op(
            vec![b"0".to_vec()],
            b"key1".to_vec(),
            Element::empty_sum_tree(),
        )];
        let cost_result = db.apply_batch(ops, None, Some(&tx), grove_version);
        cost_result.value.expect("expected to execute batch");
        let cost = cost_result.cost;
        // Explanation for 124 storage_written_bytes

        // Key -> 37 bytes
        // 32 bytes for the key prefix
        // 4 bytes for the key
        // 1 byte for key_size (required space for 36)

        // Value -> 55
        //   1 for the flag option (but no flags)
        //   1 for the enum type
        //   1 for empty tree value
        //   9 for Sum value
        //   1 for BasicMerk
        // 32 for node hash
        // 0 for value hash
        // 8 for Feature type
        // 2 byte for the value_size (required space for 98 + up to 256 for child key)

        // Parent Hook -> 48
        // Key Bytes 4
        // Hash Size 32
        // Key Length 1
        // Child Heights 2
        // Sum 9

        // Total 37 + 55 + 48 = 140

        // Replaced bytes

        // 37 + 38 = 75 (key is not replaced)

        //// Hash node calls 10
        // 1 to get the lowest merk
        //
        // 1 to get the middle merk
        // 2 for the node hash
        // 1 for the value hash
        // 1 for the combine hash
        // 1 for the kv_digest_to_kv_hash

        // On the layer above the root key did change
        // meaning we get another 5 hashes 2 + 1 + 1 + 1

        //// Seek Count explanation

        // 1 to get merk at lower level
        // 1 to insert new item
        // 1 to get root merk
        // 1 to load root tree
        // 1 to replace parent tree
        // 1 to update root
        assert_eq!(
            cost,
            OperationCost {
                seek_count: 6,
                storage_cost: StorageCost {
                    added_bytes: 140,
                    replaced_bytes: 84,
                    removed_bytes: NoStorageRemoval,
                },
                storage_loaded_bytes: 156, // todo: verify and explain
                hash_node_calls: 12,
            }
        );
    }

    #[test]
    fn test_batch_one_insert_sum_item_cost_right_below_value_required_cost_of_2() {
        let grove_version = GroveVersion::latest();
        let db = make_empty_grovedb();
        let tx = db.start_transaction();

        db.insert(
            EMPTY_PATH,
            b"sum_tree".as_slice(),
            Element::empty_sum_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("expected to insert sum tree");

        let ops = vec![QualifiedGroveDbOp::insert_or_replace_op(
            vec![b"sum_tree".to_vec()],
            b"key1".to_vec(),
            Element::new_sum_item_with_flags(15, Some([0; 42].to_vec())),
        )];
        let cost_result = db.apply_batch(ops, None, Some(&tx), grove_version);
        cost_result.value.expect("expected to execute batch");
        let cost = cost_result.cost;
        // Explanation for 243 storage_written_bytes

        // Key -> 37 bytes
        // 32 bytes for the key prefix
        // 4 bytes for the key
        // 1 byte for key_size (required space for 36)

        // Value -> 128
        //   1 for the flag option
        //   1 for the enum type
        //   9 for the value size
        //   1 for flags size
        //   41 flags size
        // 32 for node hash
        // 32 for value hash
        // 9 for basic merk
        // 1 byte for the value_size (required space for 127)

        // Parent Hook -> 48
        // Key Bytes 4
        // Hash Size 32
        // Key Length 1
        // Child Heights 2
        // Sum 9
        // Total 37 + 128 + 48 = 213

        // Hash node calls
        // 2 for the node hash
        // 1 for the value hash
        // 1 kv_digest_to_kv_hash

        // Seek Count
        // 1 to load from root tree
        // 1 to insert
        // 1 to update root tree
        assert_eq!(
            cost,
            OperationCost {
                seek_count: 6,
                storage_cost: StorageCost {
                    added_bytes: 213,
                    replaced_bytes: 91,
                    removed_bytes: NoStorageRemoval,
                },
                storage_loaded_bytes: 170,
                hash_node_calls: 10,
            }
        );
    }

    #[test]
    fn test_batch_one_insert_sum_item_cost_right_above_value_required_cost_of_2() {
        let grove_version = GroveVersion::latest();
        let db = make_empty_grovedb();
        let tx = db.start_transaction();

        db.insert(
            EMPTY_PATH,
            b"sum_tree".as_slice(),
            Element::empty_sum_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("expected to insert sum tree");

        let ops = vec![QualifiedGroveDbOp::insert_or_replace_op(
            vec![b"sum_tree".to_vec()],
            b"key1".to_vec(),
            Element::new_sum_item_with_flags(15, Some([0; 43].to_vec())),
        )];
        let cost_result = db.apply_batch(ops, None, Some(&tx), grove_version);
        cost_result.value.expect("expected to execute batch");
        let cost = cost_result.cost;
        // Explanation for 243 storage_written_bytes

        // Key -> 37 bytes
        // 32 bytes for the key prefix
        // 4 bytes for the key
        // 1 byte for key_size (required space for 36)

        // Value -> 128
        //   1 for the flag option
        //   1 for the enum type
        //   9 for the value size
        //   1 for flags size
        //   42 flags size
        // 32 for node hash
        // 32 for value hash
        // 9 for basic merk
        // 2 byte for the value_size (required space for 128)

        // Parent Hook -> 48
        // Key Bytes 4
        // Hash Size 32
        // Key Length 1
        // Child Heights 2
        // Sum 9
        // Total 37 + 128 + 48 = 215

        // Hash node calls
        // 2 for the node hash
        // 1 for the value hash
        // 1 kv_digest_to_kv_hash

        // Seek Count
        // 1 to load from root tree
        // 1 to insert
        // 1 to update root tree
        assert_eq!(
            cost,
            OperationCost {
                seek_count: 6,
                storage_cost: StorageCost {
                    added_bytes: 215,
                    replaced_bytes: 91,
                    removed_bytes: NoStorageRemoval,
                },
                storage_loaded_bytes: 170,
                hash_node_calls: 10,
            }
        );
    }

    #[test]
    fn test_batch_one_update_sum_item_bigger_no_flags() {
        let grove_version = GroveVersion::latest();
        let db = make_empty_grovedb();
        let tx = db.start_transaction();
        db.insert(
            EMPTY_PATH,
            b"tree",
            Element::empty_sum_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("expected to insert tree");

        db.insert(
            [b"tree".as_slice()].as_ref(),
            b"key1",
            Element::new_sum_item_with_flags(100, None),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("expected to insert item");

        // We are adding 2 bytes
        let ops = vec![QualifiedGroveDbOp::insert_or_replace_op(
            vec![b"tree".to_vec()],
            b"key1".to_vec(),
            Element::new_sum_item_with_flags(100000, None),
        )];

        let cost = db
            .apply_batch_with_element_flags_update(
                ops,
                None,
                |_cost, _old_flags, _new_flags| Ok(false),
                |_flags, _removed_key_bytes, _removed_value_bytes| {
                    Ok((NoStorageRemoval, NoStorageRemoval))
                },
                Some(&tx),
                grove_version,
            )
            .cost;

        // Hash node calls

        // Seek Count

        assert_eq!(
            cost,
            OperationCost {
                seek_count: 7, // todo: verify this
                storage_cost: StorageCost {
                    added_bytes: 0,
                    replaced_bytes: 220, // todo: verify this
                    removed_bytes: NoStorageRemoval
                },
                storage_loaded_bytes: 239, // todo: verify this
                hash_node_calls: 10,       // todo: verify this
            }
        );
    }

    #[test]
    fn test_batch_one_update_sum_item_bigger_with_flags() {
        let grove_version = GroveVersion::latest();
        let db = make_empty_grovedb();
        let tx = db.start_transaction();
        db.insert(
            EMPTY_PATH,
            b"tree",
            Element::empty_sum_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("expected to insert tree");

        db.insert(
            [b"tree".as_slice()].as_ref(),
            b"key1",
            Element::new_sum_item_with_flags(100, Some(vec![0])),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("expected to insert item");

        // We are adding 2 bytes
        let ops = vec![QualifiedGroveDbOp::insert_or_replace_op(
            vec![b"tree".to_vec()],
            b"key1".to_vec(),
            Element::new_sum_item_with_flags(100000, Some(vec![1])),
        )];

        let cost = db
            .apply_batch_with_element_flags_update(
                ops,
                None,
                |_cost, _old_flags, _new_flags| Ok(false),
                |_flags, _removed_key_bytes, _removed_value_bytes| {
                    Ok((NoStorageRemoval, NoStorageRemoval))
                },
                Some(&tx),
                grove_version,
            )
            .cost;

        // Hash node calls

        // Seek Count

        assert_eq!(
            cost,
            OperationCost {
                seek_count: 7, // todo: verify this
                storage_cost: StorageCost {
                    added_bytes: 0,
                    replaced_bytes: 222, // todo: verify this
                    removed_bytes: NoStorageRemoval
                },
                storage_loaded_bytes: 241, // todo: verify this
                hash_node_calls: 10,       // todo: verify this
            }
        );
    }

    #[test]
    fn test_batch_one_update_sum_item_smaller_no_flags() {
        let grove_version = GroveVersion::latest();
        let db = make_empty_grovedb();
        let tx = db.start_transaction();
        db.insert(
            EMPTY_PATH,
            b"tree",
            Element::empty_sum_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("expected to insert tree");

        db.insert(
            [b"tree".as_slice()].as_ref(),
            b"key1",
            Element::new_sum_item_with_flags(1000000, None),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("expected to insert item");

        // We are adding 2 bytes
        let ops = vec![QualifiedGroveDbOp::insert_or_replace_op(
            vec![b"tree".to_vec()],
            b"key1".to_vec(),
            Element::new_sum_item_with_flags(5, None),
        )];

        let cost = db
            .apply_batch_with_element_flags_update(
                ops,
                None,
                |_cost, _old_flags, _new_flags| Ok(false),
                |_flags, _removed_key_bytes, _removed_value_bytes| {
                    Ok((NoStorageRemoval, NoStorageRemoval))
                },
                Some(&tx),
                grove_version,
            )
            .cost;

        // Hash node calls

        // Seek Count

        assert_eq!(
            cost,
            OperationCost {
                seek_count: 7, // todo: verify this
                storage_cost: StorageCost {
                    added_bytes: 0,
                    replaced_bytes: 220, // todo: verify this
                    removed_bytes: NoStorageRemoval
                },
                storage_loaded_bytes: 248, // todo: verify this
                hash_node_calls: 10,       // todo: verify this
            }
        );
    }

    #[test]
    fn test_batch_one_update_sum_item_smaller_with_flags() {
        let grove_version = GroveVersion::latest();
        let db = make_empty_grovedb();
        let tx = db.start_transaction();
        db.insert(
            EMPTY_PATH,
            b"tree",
            Element::empty_sum_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("expected to insert tree");

        db.insert(
            [b"tree".as_slice()].as_ref(),
            b"key1",
            Element::new_sum_item_with_flags(10000000, Some(vec![0])),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("expected to insert item");

        // We are adding 2 bytes
        let ops = vec![QualifiedGroveDbOp::insert_or_replace_op(
            vec![b"tree".to_vec()],
            b"key1".to_vec(),
            Element::new_sum_item_with_flags(5, Some(vec![1])),
        )];

        let cost = db
            .apply_batch_with_element_flags_update(
                ops,
                None,
                |_cost, _old_flags, _new_flags| Ok(false),
                |_flags, _removed_key_bytes, _removed_value_bytes| {
                    Ok((NoStorageRemoval, NoStorageRemoval))
                },
                Some(&tx),
                grove_version,
            )
            .cost;

        // Hash node calls

        // Seek Count

        assert_eq!(
            cost,
            OperationCost {
                seek_count: 7, // todo: verify this
                storage_cost: StorageCost {
                    added_bytes: 0,
                    replaced_bytes: 222, // todo: verify this
                    removed_bytes: NoStorageRemoval
                },
                storage_loaded_bytes: 251, // todo: verify this
                hash_node_calls: 10,       // todo: verify this
            }
        );
    }
}
