//! Tests

#[cfg(feature = "minimal")]
mod tests {
    use grovedb_costs::{
        storage_cost::{
            removal::{
                Identifier, StorageRemovalPerEpochByIdentifier,
                StorageRemovedBytes::{
                    BasicStorageRemoval, NoStorageRemoval, SectionedStorageRemoval,
                },
            },
            transition::OperationStorageTransitionType,
            StorageCost,
        },
        OperationCost,
    };
    use grovedb_version::version::GroveVersion;
    use integer_encoding::VarInt;
    use intmap::IntMap;

    use crate::{
        batch::QualifiedGroveDbOp,
        reference_path::ReferencePathType::SiblingReference,
        tests::{common::EMPTY_PATH, make_empty_grovedb},
        Element,
    };

    #[test]
    fn test_batch_one_insert_costs_match_non_batch() {
        let grove_version = GroveVersion::latest();
        let db = make_empty_grovedb();
        let tx = db.start_transaction();

        let non_batch_cost = db
            .insert(
                EMPTY_PATH,
                b"key1",
                Element::empty_tree(),
                None,
                Some(&tx),
                grove_version,
            )
            .cost;
        tx.rollback().expect("expected to rollback");
        let ops = vec![QualifiedGroveDbOp::insert_or_replace_op(
            vec![],
            b"key1".to_vec(),
            Element::empty_tree(),
        )];
        let cost = db.apply_batch(ops, None, Some(&tx), grove_version).cost;
        assert_eq!(non_batch_cost.storage_cost, cost.storage_cost);
    }

    #[test]
    fn test_batch_root_one_insert_tree_cost() {
        let grove_version = GroveVersion::latest();
        let db = make_empty_grovedb();
        let tx = db.start_transaction();

        let ops = vec![QualifiedGroveDbOp::insert_or_replace_op(
            vec![],
            b"key1".to_vec(),
            Element::empty_tree(),
        )];
        let cost_result = db.apply_batch(ops, None, Some(&tx), grove_version);
        cost_result.value.expect("expected to execute batch");
        let cost = cost_result.cost;
        // Explanation for 113 storage_written_bytes

        // Key -> 37 bytes
        // 32 bytes for the key prefix
        // 4 bytes for the key
        // 1 byte for key_size (required space for 36)

        // Value -> 38
        //   1 for the flag option (but no flags)
        //   1 for the enum type
        //   1 for empty tree value
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
                    added_bytes: 115,
                    replaced_bytes: 0,
                    removed_bytes: NoStorageRemoval,
                },
                storage_loaded_bytes: 0,
                hash_node_calls: 6,
            }
        );
    }

    #[test]
    fn test_batch_root_one_insert_item_cost() {
        let grove_version = GroveVersion::latest();
        let db = make_empty_grovedb();
        let tx = db.start_transaction();

        let ops = vec![QualifiedGroveDbOp::insert_or_replace_op(
            vec![],
            b"key1".to_vec(),
            Element::new_item(b"cat".to_vec()),
        )];
        let cost_result = db.apply_batch(ops, None, Some(&tx), grove_version);
        cost_result.value.expect("expected to execute batch");
        let cost = cost_result.cost;
        // Explanation for 214 storage_written_bytes

        // Key -> 37 bytes
        // 32 bytes for the key prefix
        // 4 bytes for the key
        // 1 byte for key_size (required space for 36)

        // Value -> 72
        //   1 for the flag option (but no flags)
        //   1 for the enum type
        //   1 for required space for bytes
        //   3 for item bytes
        // 32 for node hash
        // 32 for value hash
        // 1 for basic merk
        // 1 byte for the value_size (required space for 71)

        // Parent Hook -> 40
        // Key Bytes 4
        // Hash Size 32
        // Key Length 1
        // Child Heights 2
        // Sum 1

        // Total 37 + 72 + 40 = 149

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
                seek_count: 3,
                storage_cost: StorageCost {
                    added_bytes: 149,
                    replaced_bytes: 0,
                    removed_bytes: NoStorageRemoval,
                },
                storage_loaded_bytes: 0,
                hash_node_calls: 4,
            }
        );
    }

    #[test]
    fn test_batch_root_one_insert_tree_under_parent_item_in_same_merk_cost() {
        let grove_version = GroveVersion::latest();
        let db = make_empty_grovedb();
        let tx = db.start_transaction();

        let cost = db
            .insert(
                EMPTY_PATH,
                b"0",
                Element::new_item(b"cat".to_vec()),
                None,
                Some(&tx),
                grove_version,
            )
            .cost_as_result()
            .expect("successful root tree leaf insert");

        assert_eq!(cost.storage_cost.added_bytes, 143);

        let ops = vec![QualifiedGroveDbOp::insert_or_replace_op(
            vec![],
            b"key1".to_vec(),
            Element::empty_tree(),
        )];
        let cost_result = db.apply_batch(ops, None, Some(&tx), grove_version);
        cost_result.value.expect("expected to execute batch");
        let cost = cost_result.cost;
        // Explanation for 115 storage_written_bytes

        // Key -> 37 bytes
        // 32 bytes for the key prefix
        // 4 bytes for the key
        // 1 byte for key_size (required space for 36)

        // Value -> 38
        //   1 for the flag option (but no flags)
        //   1 for the enum type
        //   1 for empty tree value
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

        // Value -> 80
        //   1 for the flag option (but no flags)
        //   1 for the enum type
        //   1 for required space for bytes
        //   3 for item bytes
        //   1 for basic Merk
        // 32 for node hash
        // 40 for parent hook
        // 1 byte for the value_size (required space for 99)

        // 80 + ? = 108

        // Hash node calls 8
        // 1 for the inserted tree hash
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
                    added_bytes: 115,
                    replaced_bytes: 109, // todo verify
                    removed_bytes: NoStorageRemoval,
                },
                storage_loaded_bytes: 74, // todo: verify and explain
                hash_node_calls: 8,
            }
        );
    }

    #[test]
    fn test_batch_root_one_insert_tree_under_parent_tree_in_same_merk_cost() {
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
            Element::empty_tree(),
        )];
        let cost_result = db.apply_batch(ops, None, Some(&tx), grove_version);
        cost_result.value.expect("expected to execute batch");
        let cost = cost_result.cost;
        // Explanation for 113 storage_written_bytes

        // Key -> 37 bytes
        // 32 bytes for the key prefix
        // 4 bytes for the key
        // 1 byte for key_size (required space for 36)

        // Value -> 38
        //   1 for the flag option (but no flags)
        //   1 for the enum type
        //   1 for empty tree value
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
                    added_bytes: 115,
                    replaced_bytes: 75,
                    removed_bytes: NoStorageRemoval,
                },
                storage_loaded_bytes: 71, // todo: verify and explain
                hash_node_calls: 8,
            }
        );
    }

    #[test]
    fn test_batch_root_one_insert_tree_under_parent_tree_in_different_merk_cost() {
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
            Element::empty_tree(),
        )];
        let cost_result = db.apply_batch(ops, None, Some(&tx), grove_version);
        cost_result.value.expect("expected to execute batch");
        let cost = cost_result.cost;
        // Explanation for 113 storage_written_bytes

        // Key -> 37 bytes
        // 32 bytes for the key prefix
        // 4 bytes for the key
        // 1 byte for key_size (required space for 36)

        // Value -> 38
        //   1 for the flag option (but no flags)
        //   1 for the enum type
        //   1 for empty tree value
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
                    added_bytes: 115,
                    replaced_bytes: 75,
                    removed_bytes: NoStorageRemoval,
                },
                storage_loaded_bytes: 146, // todo: verify and explain
                hash_node_calls: 12,
            }
        );
    }

    #[test]
    fn test_batch_root_one_insert_cost_right_below_value_required_cost_of_2() {
        let grove_version = GroveVersion::latest();
        let db = make_empty_grovedb();
        let tx = db.start_transaction();

        let ops = vec![QualifiedGroveDbOp::insert_or_replace_op(
            vec![],
            b"key1".to_vec(),
            Element::new_item([0u8; 59].to_vec()),
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
        //   1 for the flag option (but no flags)
        //   1 for the enum type
        //   1 for the value size
        //   59 bytes
        // 32 for node hash
        // 32 for value hash
        // 1 for basic merk
        // 1 byte for the value_size (required space for 127)

        // Parent Hook -> 40
        // Key Bytes 4
        // Hash Size 32
        // Key Length 1
        // Child Heights 2
        // Sum 1
        // Total 37 + 128 + 40 = 205

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
                seek_count: 3,
                storage_cost: StorageCost {
                    added_bytes: 205,
                    replaced_bytes: 0,
                    removed_bytes: NoStorageRemoval,
                },
                storage_loaded_bytes: 0,
                hash_node_calls: 4,
            }
        );
    }

    #[test]
    fn test_batch_root_one_insert_cost_right_above_value_required_cost_of_2() {
        let grove_version = GroveVersion::latest();
        let db = make_empty_grovedb();
        let tx = db.start_transaction();

        let ops = vec![QualifiedGroveDbOp::insert_or_replace_op(
            vec![],
            b"key1".to_vec(),
            Element::new_item([0u8; 60].to_vec()),
        )];
        let cost_result = db.apply_batch(ops, None, Some(&tx), grove_version);
        cost_result.value.expect("expected to execute batch");
        let cost = cost_result.cost;
        // Explanation for 243 storage_written_bytes

        // Key -> 37 bytes
        // 32 bytes for the key prefix
        // 4 bytes for the key
        // 1 byte for key_size (required space for 36)

        // Value -> 130
        //   1 for the flag option (but no flags)
        //   1 for the enum type
        //   1 for the value size
        //   60 bytes
        // 32 for node hash
        // 32 for value hash
        // 1 for basic merk
        // 2 byte for the value_size (required space for 128)

        // Parent Hook -> 40
        // Key Bytes 4
        // Hash Size 32
        // Key Length 1
        // Child Heights 2
        // Sum 1
        // Total 37 + 130 + 40 = 207

        // Hash node calls
        // 2 for the node hash
        // 1 for the value hash (just under)
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
                    added_bytes: 207,
                    replaced_bytes: 0,
                    removed_bytes: NoStorageRemoval,
                },
                storage_loaded_bytes: 0,
                hash_node_calls: 4,
            }
        );
    }

    #[test]
    fn test_batch_root_one_insert_with_flags_cost_right_below_value_required_cost_of_2() {
        let grove_version = GroveVersion::latest();
        let db = make_empty_grovedb();
        let tx = db.start_transaction();

        let ops = vec![QualifiedGroveDbOp::insert_or_replace_op(
            vec![],
            b"key1".to_vec(),
            Element::new_item_with_flags([0u8; 56].to_vec(), Some(vec![0, 0])),
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
        //   1 for the flag options
        //   1 for flags size
        //   2 for flag bytes
        //   1 for the enum type
        //   1 for the value size
        //   56 bytes
        // 32 for node hash
        // 32 for value hash
        // 1 for basic merk
        // 1 byte for the value_size (required space for 127)

        // Parent Hook -> 40
        // Key Bytes 4
        // Hash Size 32
        // Key Length 1
        // Child Heights 2
        // Sum 1
        // Total 37 + 128 + 40 = 205

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
                seek_count: 3,
                storage_cost: StorageCost {
                    added_bytes: 205,
                    replaced_bytes: 0,
                    removed_bytes: NoStorageRemoval,
                },
                storage_loaded_bytes: 0,
                hash_node_calls: 4,
            }
        );
    }

    #[test]
    fn test_batch_root_one_insert_with_flags_cost_right_above_value_required_cost_of_2() {
        let grove_version = GroveVersion::latest();
        let db = make_empty_grovedb();
        let tx = db.start_transaction();

        let ops = vec![QualifiedGroveDbOp::insert_or_replace_op(
            vec![],
            b"key1".to_vec(),
            Element::new_item_with_flags([0u8; 57].to_vec(), Some(vec![0, 0])),
        )];
        let cost_result = db.apply_batch(ops, None, Some(&tx), grove_version);
        cost_result.value.expect("expected to execute batch");
        let cost = cost_result.cost;
        // Explanation for 243 storage_written_bytes

        // Key -> 37 bytes
        // 32 bytes for the key prefix
        // 4 bytes for the key
        // 1 byte for key_size (required space for 36)

        // Value -> 130
        //   1 for the flag option
        //   1 for flags size
        //   2 for flag bytes
        //   1 for the enum type
        //   1 for the value size
        //   60 bytes
        // 32 for node hash
        // 32 for value hash
        // 1 for basic merk
        // 2 byte for the value_size (required space for 128)

        // Parent Hook -> 40
        // Key Bytes 4
        // Hash Size 32
        // Key Length 1
        // Child Heights 2
        // Sum 1
        // Total 37 + 130 + 40 = 207

        // Hash node calls
        // 2 for the node hash
        // 1 for the value hash (just under)
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
                    added_bytes: 207,
                    replaced_bytes: 0,
                    removed_bytes: NoStorageRemoval,
                },
                storage_loaded_bytes: 0,
                hash_node_calls: 4,
            }
        );
    }

    #[test]
    fn test_batch_root_one_update_item_bigger_cost_no_flags() {
        let grove_version = GroveVersion::latest();
        let db = make_empty_grovedb();
        let tx = db.start_transaction();
        db.insert(
            EMPTY_PATH,
            b"tree",
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("expected to insert tree");

        db.insert(
            [b"tree".as_slice()].as_ref(),
            b"key1",
            Element::new_item_with_flags(b"value1".to_vec(), Some(vec![0])),
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
            Element::new_item_with_flags(b"value100".to_vec(), Some(vec![1])),
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
                    added_bytes: 2,
                    replaced_bytes: 195, // todo: verify this
                    removed_bytes: NoStorageRemoval
                },
                storage_loaded_bytes: 235, // todo: verify this
                hash_node_calls: 10,       // todo: verify this
            }
        );
    }

    #[test]
    fn test_batch_root_one_update_item_bigger_cost() {
        let grove_version = GroveVersion::latest();
        let db = make_empty_grovedb();
        let tx = db.start_transaction();
        db.insert(
            EMPTY_PATH,
            b"tree",
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("expected to insert tree");

        db.insert(
            [b"tree".as_slice()].as_ref(),
            b"key1",
            Element::new_item_with_flags(b"value1".to_vec(), Some(vec![0, 0])),
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
            Element::new_item_with_flags(b"value100".to_vec(), Some(vec![0, 1])),
        )];

        let cost = db
            .apply_batch_with_element_flags_update(
                ops,
                None,
                |cost, old_flags, new_flags| match cost.transition_type() {
                    OperationStorageTransitionType::OperationUpdateBiggerSize => {
                        if new_flags[0] == 0 {
                            new_flags[0] = 1;
                            let new_flags_epoch = new_flags[1];
                            new_flags[1] = old_flags.unwrap()[1];
                            new_flags.push(new_flags_epoch);
                            new_flags.extend(cost.added_bytes.encode_var_vec());
                            // first pass will be vec![1u8, 0, 1, 2], second pass will be vec![1u8,
                            // 0, 1, 4]
                            assert!(
                                new_flags == &vec![1u8, 0, 1, 2]
                                    || new_flags == &vec![1u8, 0, 1, 4]
                            );
                            Ok(true)
                        } else {
                            assert_eq!(new_flags[0], 1);
                            Ok(false)
                        }
                    }
                    OperationStorageTransitionType::OperationUpdateSmallerSize => {
                        new_flags.extend(vec![1, 2]);
                        Ok(true)
                    }
                    _ => Ok(false),
                },
                |_flags, removed_key_bytes, removed_value_bytes| {
                    Ok((
                        BasicStorageRemoval(removed_key_bytes),
                        BasicStorageRemoval(removed_value_bytes),
                    ))
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
                    added_bytes: 4,
                    replaced_bytes: 196, // todo: verify this
                    removed_bytes: NoStorageRemoval
                },
                storage_loaded_bytes: 236, // todo: verify this
                hash_node_calls: 10,       // todo: verify this
            }
        );
    }

    #[test]
    fn test_batch_root_one_update_item_bigger_cost_with_refresh_reference() {
        let grove_version = GroveVersion::latest();
        let db = make_empty_grovedb();
        let tx = db.start_transaction();
        db.insert(
            EMPTY_PATH,
            b"tree",
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("expected to insert tree");

        db.insert(
            [b"tree".as_slice()].as_ref(),
            b"key1",
            Element::new_item_with_flags(b"value1".to_vec(), Some(vec![0, 0])),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("expected to insert item");

        db.insert(
            [b"tree".as_slice()].as_ref(),
            b"keyref",
            Element::new_reference_with_flags(SiblingReference(b"key1".to_vec()), Some(vec![0, 0])),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("expected to insert item");

        // We are adding 2 bytes
        let ops = vec![
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![b"tree".to_vec()],
                b"key1".to_vec(),
                Element::new_item_with_flags(b"value100".to_vec(), Some(vec![0, 1])),
            ),
            QualifiedGroveDbOp::replace_op(
                vec![b"tree".to_vec()],
                b"keyref".to_vec(),
                Element::new_reference_with_flags(
                    SiblingReference(b"key1".to_vec()),
                    Some(vec![0, 1]),
                ),
            ),
        ];

        let cost = db
            .apply_batch_with_element_flags_update(
                ops,
                None,
                |cost, old_flags, new_flags| match cost.transition_type() {
                    OperationStorageTransitionType::OperationUpdateBiggerSize => {
                        if new_flags[0] == 0 {
                            new_flags[0] = 1;
                            let new_flags_epoch = new_flags[1];
                            new_flags[1] = old_flags.unwrap()[1];
                            new_flags.push(new_flags_epoch);
                            new_flags.extend(cost.added_bytes.encode_var_vec());
                            // first pass will be vec![1u8, 0, 1, 2], second pass will be vec![1u8,
                            // 0, 1, 4]
                            assert!(
                                new_flags == &vec![1u8, 0, 1, 2]
                                    || new_flags == &vec![1u8, 0, 1, 4]
                            );
                            Ok(true)
                        } else {
                            assert_eq!(new_flags[0], 1);
                            Ok(false)
                        }
                    }
                    OperationStorageTransitionType::OperationUpdateSmallerSize => {
                        new_flags.extend(vec![1, 2]);
                        Ok(true)
                    }
                    _ => Ok(false),
                },
                |_flags, removed_key_bytes, removed_value_bytes| {
                    Ok((
                        BasicStorageRemoval(removed_key_bytes),
                        BasicStorageRemoval(removed_value_bytes),
                    ))
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
                seek_count: 9, // todo: verify this
                storage_cost: StorageCost {
                    added_bytes: 4,
                    replaced_bytes: 316, // todo: verify this
                    removed_bytes: NoStorageRemoval
                },
                storage_loaded_bytes: 357, // todo: verify this
                hash_node_calls: 16,       // todo: verify this
            }
        );

        let issues = db
            .visualize_verify_grovedb(Some(&tx), true, false, &Default::default())
            .unwrap();
        assert_eq!(
            issues.len(),
            0,
            "reference issue: {}",
            issues
                .iter()
                .map(|(hash, (a, b, c))| format!("{}: {} {} {}", hash, a, b, c))
                .collect::<Vec<_>>()
                .join(" | ")
        );
    }

    #[test]
    fn test_batch_root_one_update_item_bigger_cost_with_insert_reference() {
        let grove_version = GroveVersion::latest();
        let db = make_empty_grovedb();
        let tx = db.start_transaction();
        db.insert(
            EMPTY_PATH,
            b"tree",
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("expected to insert tree");

        db.insert(
            [b"tree".as_slice()].as_ref(),
            b"key1",
            Element::new_item_with_flags(b"value1".to_vec(), Some(vec![0, 0])),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("expected to insert item");

        // We are adding 2 bytes
        let ops = vec![
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![b"tree".to_vec()],
                b"key1".to_vec(),
                Element::new_item_with_flags(b"value100".to_vec(), Some(vec![0, 1])),
            ),
            QualifiedGroveDbOp::insert_only_op(
                vec![b"tree".to_vec()],
                b"keyref".to_vec(),
                Element::new_reference_with_flags(
                    SiblingReference(b"key1".to_vec()),
                    Some(vec![0, 1]),
                ),
            ),
        ];

        let cost = db
            .apply_batch_with_element_flags_update(
                ops,
                None,
                |cost, old_flags, new_flags| match cost.transition_type() {
                    OperationStorageTransitionType::OperationUpdateBiggerSize => {
                        if new_flags[0] == 0 {
                            new_flags[0] = 1;
                            let new_flags_epoch = new_flags[1];
                            new_flags[1] = old_flags.unwrap()[1];
                            new_flags.push(new_flags_epoch);
                            new_flags.extend(cost.added_bytes.encode_var_vec());
                            // first pass will be vec![1u8, 0, 1, 2], second pass will be vec![1u8,
                            // 0, 1, 4]
                            assert!(
                                new_flags == &vec![1u8, 0, 1, 2]
                                    || new_flags == &vec![1u8, 0, 1, 4]
                            );
                            Ok(true)
                        } else {
                            assert_eq!(new_flags[0], 1);
                            Ok(false)
                        }
                    }
                    OperationStorageTransitionType::OperationUpdateSmallerSize => {
                        new_flags.extend(vec![1, 2]);
                        Ok(true)
                    }
                    _ => Ok(false),
                },
                |_flags, removed_key_bytes, removed_value_bytes| {
                    Ok((
                        BasicStorageRemoval(removed_key_bytes),
                        BasicStorageRemoval(removed_value_bytes),
                    ))
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
                seek_count: 8, // todo: verify this
                storage_cost: StorageCost {
                    added_bytes: 163,
                    replaced_bytes: 196, // todo: verify this
                    removed_bytes: NoStorageRemoval
                },
                storage_loaded_bytes: 236, // todo: verify this
                hash_node_calls: 16,       // todo: verify this
            }
        );

        let issues = db
            .visualize_verify_grovedb(Some(&tx), true, false, &Default::default())
            .unwrap();
        assert_eq!(
            issues.len(),
            0,
            "reference issue: {}",
            issues
                .iter()
                .map(|(hash, (a, b, c))| format!("{}: {} {} {}", hash, a, b, c))
                .collect::<Vec<_>>()
                .join(" | ")
        );
    }

    #[test]
    fn test_batch_root_one_update_item_smaller_cost_no_flags() {
        let grove_version = GroveVersion::latest();
        let db = make_empty_grovedb();
        let tx = db.start_transaction();
        db.insert(
            EMPTY_PATH,
            b"tree",
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("expected to insert tree");

        db.insert(
            [b"tree".as_slice()].as_ref(),
            b"key1",
            Element::new_item_with_flags(b"value1".to_vec(), Some(vec![0])),
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
            Element::new_item_with_flags(b"value".to_vec(), Some(vec![1])),
        )];

        let cost = db
            .apply_batch_with_element_flags_update(
                ops,
                None,
                |_cost, _old_flags, _new_flags| Ok(false),
                |_flags, removed_key_bytes, removed_value_bytes| {
                    Ok((
                        BasicStorageRemoval(removed_key_bytes),
                        BasicStorageRemoval(removed_value_bytes),
                    ))
                },
                Some(&tx),
                grove_version,
            )
            .cost;

        assert_eq!(
            cost,
            OperationCost {
                seek_count: 7, // todo: verify this
                storage_cost: StorageCost {
                    added_bytes: 0,
                    replaced_bytes: 194, // todo: verify this
                    removed_bytes: BasicStorageRemoval(1)
                },
                storage_loaded_bytes: 235, // todo: verify this
                hash_node_calls: 10,       // todo: verify this
            }
        );
    }

    #[test]
    fn test_batch_root_one_update_item_smaller_cost() {
        let grove_version = GroveVersion::latest();
        let db = make_empty_grovedb();
        let tx = db.start_transaction();
        db.insert(
            EMPTY_PATH,
            b"tree",
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("expected to insert tree");

        db.insert(
            [b"tree".as_slice()].as_ref(),
            b"key1",
            Element::new_item_with_flags(b"value1".to_vec(), Some(vec![0, 0])),
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
            Element::new_item_with_flags(b"value".to_vec(), Some(vec![0, 1])),
        )];

        let cost = db
            .apply_batch_with_element_flags_update(
                ops,
                None,
                |cost, old_flags, new_flags| match cost.transition_type() {
                    OperationStorageTransitionType::OperationUpdateBiggerSize => {
                        if new_flags[0] == 0 {
                            new_flags[0] = 1;
                            let new_flags_epoch = new_flags[1];
                            new_flags[1] = old_flags.unwrap()[1];
                            new_flags.push(new_flags_epoch);
                            new_flags.extend(cost.added_bytes.encode_var_vec());
                            // first pass will be vec![1u8, 0, 1, 2], second pass will be vec![1u8,
                            // 0, 1, 4]
                            assert!(
                                new_flags == &vec![1u8, 0, 1, 2]
                                    || new_flags == &vec![1u8, 0, 1, 4]
                            );
                            Ok(true)
                        } else {
                            assert_eq!(new_flags[0], 1);
                            Ok(false)
                        }
                    }
                    OperationStorageTransitionType::OperationUpdateSmallerSize => Ok(true),
                    _ => Ok(false),
                },
                |_flags, _removed_key_bytes, removed_value_bytes| {
                    let mut removed_bytes = StorageRemovalPerEpochByIdentifier::default();
                    // we are removing 1 byte from epoch 0 for an identity
                    let mut removed_bytes_for_identity = IntMap::new();
                    removed_bytes_for_identity.insert(0, removed_value_bytes);
                    removed_bytes.insert(Identifier::default(), removed_bytes_for_identity);
                    Ok((NoStorageRemoval, SectionedStorageRemoval(removed_bytes)))
                },
                Some(&tx),
                grove_version,
            )
            .cost;

        let mut removed_bytes = StorageRemovalPerEpochByIdentifier::default();
        // we are removing 1 byte from epoch 0 for an identity
        let mut removed_bytes_for_identity = IntMap::new();
        removed_bytes_for_identity.insert(0, 1);
        removed_bytes.insert(Identifier::default(), removed_bytes_for_identity);

        assert_eq!(
            cost,
            OperationCost {
                seek_count: 7, // todo: verify this
                storage_cost: StorageCost {
                    added_bytes: 0,
                    replaced_bytes: 195, // todo: verify this
                    removed_bytes: SectionedStorageRemoval(removed_bytes)
                },
                storage_loaded_bytes: 236, // todo: verify this
                hash_node_calls: 10,       // todo: verify this
            }
        );
    }

    #[test]
    fn test_batch_root_one_update_item_smaller_cost_with_refresh_reference() {
        let grove_version = GroveVersion::latest();
        let db = make_empty_grovedb();
        let tx = db.start_transaction();
        db.insert(
            EMPTY_PATH,
            b"tree",
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("expected to insert tree");

        db.insert(
            [b"tree".as_slice()].as_ref(),
            b"key1",
            Element::new_item_with_flags(b"value1".to_vec(), Some(vec![0, 0])),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("expected to insert item");

        db.insert(
            [b"tree".as_slice()].as_ref(),
            b"keyref",
            Element::new_reference_with_flags(SiblingReference(b"key1".to_vec()), Some(vec![0, 0])),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("expected to insert item");

        // We are adding 2 bytes
        let ops = vec![
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![b"tree".to_vec()],
                b"key1".to_vec(),
                Element::new_item_with_flags(b"value".to_vec(), Some(vec![0, 1])),
            ),
            QualifiedGroveDbOp::replace_op(
                vec![b"tree".to_vec()],
                b"keyref".to_vec(),
                Element::new_reference_with_flags(
                    SiblingReference(b"key1".to_vec()),
                    Some(vec![0, 1]),
                ),
            ),
        ];

        let cost = db
            .apply_batch_with_element_flags_update(
                ops,
                None,
                |cost, old_flags, new_flags| match cost.transition_type() {
                    OperationStorageTransitionType::OperationUpdateBiggerSize => {
                        if new_flags[0] == 0 {
                            new_flags[0] = 1;
                            let new_flags_epoch = new_flags[1];
                            new_flags[1] = old_flags.unwrap()[1];
                            new_flags.push(new_flags_epoch);
                            new_flags.extend(cost.added_bytes.encode_var_vec());
                            // first pass will be vec![1u8, 0, 1, 2], second pass will be vec![1u8,
                            // 0, 1, 4]
                            assert!(
                                new_flags == &vec![1u8, 0, 1, 2]
                                    || new_flags == &vec![1u8, 0, 1, 4]
                            );
                            Ok(true)
                        } else {
                            assert_eq!(new_flags[0], 1);
                            Ok(false)
                        }
                    }
                    OperationStorageTransitionType::OperationUpdateSmallerSize => Ok(true),
                    _ => Ok(false),
                },
                |_flags, _removed_key_bytes, removed_value_bytes| {
                    let mut removed_bytes = StorageRemovalPerEpochByIdentifier::default();
                    // we are removing 1 byte from epoch 0 for an identity
                    let mut removed_bytes_for_identity = IntMap::new();
                    removed_bytes_for_identity.insert(0, removed_value_bytes);
                    removed_bytes.insert(Identifier::default(), removed_bytes_for_identity);
                    Ok((NoStorageRemoval, SectionedStorageRemoval(removed_bytes)))
                },
                Some(&tx),
                grove_version,
            )
            .cost;

        let mut removed_bytes = StorageRemovalPerEpochByIdentifier::default();
        // we are removing 1 byte from epoch 0 for an identity
        let mut removed_bytes_for_identity = IntMap::new();
        removed_bytes_for_identity.insert(0, 1);
        removed_bytes.insert(Identifier::default(), removed_bytes_for_identity);

        assert_eq!(
            cost,
            OperationCost {
                seek_count: 9, // todo: verify this
                storage_cost: StorageCost {
                    added_bytes: 0,
                    replaced_bytes: 315, // todo: verify this
                    removed_bytes: SectionedStorageRemoval(removed_bytes)
                },
                storage_loaded_bytes: 357, // todo: verify this
                hash_node_calls: 16,       // todo: verify this
            }
        );

        let issues = db
            .visualize_verify_grovedb(Some(&tx), true, false, &Default::default())
            .unwrap();
        assert_eq!(
            issues.len(),
            0,
            "reference issue: {}",
            issues
                .iter()
                .map(|(hash, (a, b, c))| format!("{}: {} {} {}", hash, a, b, c))
                .collect::<Vec<_>>()
                .join(" | ")
        );
    }

    #[test]
    fn test_batch_root_one_update_tree_bigger_flags_cost() {
        let grove_version = GroveVersion::latest();
        let db = make_empty_grovedb();
        let tx = db.start_transaction();
        db.insert(
            EMPTY_PATH,
            b"tree",
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("expected to insert tree");

        db.insert(
            [b"tree".as_slice()].as_ref(),
            b"key1",
            Element::new_tree_with_flags(None, Some(vec![0, 0])),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("expected to insert item");

        // We are adding 1 byte to the flags
        let ops = vec![QualifiedGroveDbOp::insert_or_replace_op(
            vec![b"tree".to_vec()],
            b"key1".to_vec(),
            Element::new_tree_with_flags(None, Some(vec![0, 1, 1])),
        )];

        let cost = db
            .apply_batch_with_element_flags_update(
                ops,
                None,
                |cost, old_flags, new_flags| match cost.transition_type() {
                    OperationStorageTransitionType::OperationUpdateBiggerSize => {
                        if new_flags[0] == 0 {
                            new_flags[0] = 1;
                            let new_flags_epoch = new_flags[1];
                            let old_flags = old_flags.unwrap();
                            new_flags[1] = old_flags[1];
                            new_flags.push(new_flags_epoch);
                            new_flags.extend(cost.added_bytes.encode_var_vec());
                            assert!(
                                new_flags == &vec![1u8, 0, 1, 1, 1]
                                    || new_flags == &vec![1u8, 0, 1, 1, 3]
                            );
                            Ok(true)
                        } else {
                            assert_eq!(new_flags[0], 1);
                            Ok(false)
                        }
                    }
                    OperationStorageTransitionType::OperationUpdateSmallerSize => {
                        new_flags.extend(vec![1, 2]);
                        Ok(true)
                    }
                    _ => Ok(false),
                },
                |_flags, _removed_key_bytes, removed_value_bytes| {
                    Ok((NoStorageRemoval, BasicStorageRemoval(removed_value_bytes)))
                },
                Some(&tx),
                grove_version,
            )
            .cost;

        assert_eq!(
            cost,
            OperationCost {
                seek_count: 7, // todo: verify this
                storage_cost: StorageCost {
                    added_bytes: 3,
                    replaced_bytes: 159, // todo: verify this
                    removed_bytes: NoStorageRemoval
                },
                storage_loaded_bytes: 230, // todo: verify this
                hash_node_calls: 12,       // todo: verify this
            }
        );
    }

    #[test]
    fn test_batch_root_one_update_cost_right_above_value_required_cost_of_2_with_refresh_reference()
    {
        let grove_version = GroveVersion::latest();
        let db = make_empty_grovedb();
        let tx = db.start_transaction();

        let ops = vec![
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![],
                b"key1".to_vec(),
                Element::new_item_with_flags([0u8; 56].to_vec(), Some(vec![0, 0])),
            ),
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![],
                b"keyref".to_vec(),
                Element::new_reference(SiblingReference(b"key1".to_vec())),
            ),
        ];

        let cost_result = db.apply_batch(ops, None, Some(&tx), grove_version);
        cost_result.value.expect("expected to execute batch");
        // We are adding 2 bytes
        let ops = vec![
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![],
                b"key1".to_vec(),
                Element::new_item_with_flags([0u8; 57].to_vec(), Some(vec![0, 1])),
            ),
            QualifiedGroveDbOp::replace_op(
                vec![],
                b"keyref".to_vec(),
                Element::new_reference(SiblingReference(b"key1".to_vec())),
            ),
        ];

        let cost = db
            .apply_batch_with_element_flags_update(
                ops,
                None,
                |cost, old_flags, new_flags| match cost.transition_type() {
                    OperationStorageTransitionType::OperationUpdateBiggerSize => {
                        if new_flags[0] == 0 {
                            new_flags[0] = 1;
                            let new_flags_epoch = new_flags[1];
                            new_flags[1] = old_flags.unwrap()[1];
                            new_flags.push(new_flags_epoch);
                            new_flags.extend(cost.added_bytes.encode_var_vec());
                            // first pass will be vec![1u8, 0, 1, 2], second pass will be vec![1u8,
                            // 0, 1, 4]
                            assert!(
                                new_flags == &vec![1u8, 0, 1, 2]
                                    || new_flags == &vec![1u8, 0, 1, 4]
                            );
                            Ok(true)
                        } else {
                            assert_eq!(new_flags[0], 1);
                            Ok(false)
                        }
                    }
                    OperationStorageTransitionType::OperationUpdateSmallerSize => {
                        new_flags.extend(vec![1, 2]);
                        Ok(true)
                    }
                    _ => Ok(false),
                },
                |_flags, removed_key_bytes, removed_value_bytes| {
                    Ok((
                        BasicStorageRemoval(removed_key_bytes),
                        BasicStorageRemoval(removed_value_bytes),
                    ))
                },
                Some(&tx),
                grove_version,
            )
            .cost;

        assert_eq!(
            cost,
            OperationCost {
                seek_count: 7, // todo: verify this
                storage_cost: StorageCost {
                    added_bytes: 4,
                    replaced_bytes: 285, // todo: verify this
                    removed_bytes: NoStorageRemoval
                },
                storage_loaded_bytes: 380, // todo: verify this
                hash_node_calls: 12,       // todo: verify this
            }
        );

        let issues = db
            .visualize_verify_grovedb(Some(&tx), true, false, &Default::default())
            .unwrap();
        assert_eq!(
            issues.len(),
            0,
            "reference issue: {}",
            issues
                .iter()
                .map(|(hash, (a, b, c))| format!("{}: {} {} {}", hash, a, b, c))
                .collect::<Vec<_>>()
                .join(" | ")
        );
    }

    #[test]
    fn test_batch_root_one_update_cost_right_above_value_required_cost_of_2_with_insert_reference()
    {
        let grove_version = GroveVersion::latest();
        let db = make_empty_grovedb();
        let tx = db.start_transaction();

        let ops = vec![QualifiedGroveDbOp::insert_or_replace_op(
            vec![],
            b"key1".to_vec(),
            Element::new_item_with_flags([0u8; 56].to_vec(), Some(vec![0, 0])),
        )];

        let cost_result = db.apply_batch(ops, None, Some(&tx), grove_version);
        cost_result.value.expect("expected to execute batch");
        // We are adding 2 bytes
        let ops = vec![
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![],
                b"key1".to_vec(),
                Element::new_item_with_flags([0u8; 57].to_vec(), Some(vec![0, 1])),
            ),
            QualifiedGroveDbOp::insert_only_op(
                vec![],
                b"keyref".to_vec(),
                Element::new_reference(SiblingReference(b"key1".to_vec())),
            ),
        ];

        let cost = db
            .apply_batch_with_element_flags_update(
                ops,
                None,
                |cost, old_flags, new_flags| match cost.transition_type() {
                    OperationStorageTransitionType::OperationUpdateBiggerSize => {
                        if new_flags[0] == 0 {
                            new_flags[0] = 1;
                            let new_flags_epoch = new_flags[1];
                            new_flags[1] = old_flags.unwrap()[1];
                            new_flags.push(new_flags_epoch);
                            new_flags.extend(cost.added_bytes.encode_var_vec());
                            // first pass will be vec![1u8, 0, 1, 2], second pass will be vec![1u8,
                            // 0, 1, 4]
                            assert!(
                                new_flags == &vec![1u8, 0, 1, 2]
                                    || new_flags == &vec![1u8, 0, 1, 4]
                            );
                            Ok(true)
                        } else {
                            assert_eq!(new_flags[0], 1);
                            Ok(false)
                        }
                    }
                    OperationStorageTransitionType::OperationUpdateSmallerSize => {
                        new_flags.extend(vec![1, 2]);
                        Ok(true)
                    }
                    _ => Ok(false),
                },
                |_flags, removed_key_bytes, removed_value_bytes| {
                    Ok((
                        BasicStorageRemoval(removed_key_bytes),
                        BasicStorageRemoval(removed_value_bytes),
                    ))
                },
                Some(&tx),
                grove_version,
            )
            .cost;

        assert_eq!(
            cost,
            OperationCost {
                seek_count: 5, // todo: verify this
                storage_cost: StorageCost {
                    added_bytes: 160,
                    replaced_bytes: 168, // todo: verify this
                    removed_bytes: NoStorageRemoval
                },
                storage_loaded_bytes: 133, // todo: verify this
                hash_node_calls: 12,       // todo: verify this
            }
        );

        let issues = db
            .visualize_verify_grovedb(Some(&tx), true, false, &Default::default())
            .unwrap();
        assert_eq!(
            issues.len(),
            0,
            "reference issue: {}",
            issues
                .iter()
                .map(|(hash, (a, b, c))| format!("{}: {} {} {}", hash, a, b, c))
                .collect::<Vec<_>>()
                .join(" | ")
        );
    }
}
