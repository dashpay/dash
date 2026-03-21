//! This tests just in time costs
//! Just in time costs modify the tree in the same batch

#[cfg(feature = "minimal")]
mod tests {
    use std::{collections::BTreeMap, option::Option::None};

    use grovedb_costs::{
        storage_cost::removal::{StorageRemovalPerEpochByIdentifier, StorageRemovedBytes},
        OperationCost,
    };
    use grovedb_epoch_based_storage_flags::StorageFlags;
    use grovedb_version::version::GroveVersion;
    use intmap::IntMap;

    use crate::{
        batch::QualifiedGroveDbOp,
        reference_path::{
            ReferencePathType, ReferencePathType::UpstreamFromElementHeightReference,
        },
        tests::{common::EMPTY_PATH, make_empty_grovedb, TempGroveDb},
        Element, Error, Transaction,
    };

    fn single_epoch_removed_bytes_map(
        owner_id: [u8; 32],
        epoch_index: u16,
        bytes_removed: u32,
    ) -> StorageRemovalPerEpochByIdentifier {
        let mut removed_bytes = StorageRemovalPerEpochByIdentifier::default();
        let mut removed_bytes_for_identity = IntMap::new();
        removed_bytes_for_identity.insert(epoch_index, bytes_removed);
        removed_bytes.insert(owner_id, removed_bytes_for_identity);
        removed_bytes
    }

    fn apply_batch(
        grove_db: &TempGroveDb,
        ops: Vec<QualifiedGroveDbOp>,
        tx: &Transaction,
        grove_version: &GroveVersion,
    ) -> OperationCost {
        grove_db
            .apply_batch_with_element_flags_update(
                ops,
                None,
                |cost, old_flags, new_flags| {
                    StorageFlags::update_element_flags(cost, old_flags, new_flags)
                        .map_err(|e| Error::JustInTimeElementFlagsClientError(e.to_string()))
                },
                |flags, removed_key_bytes, removed_value_bytes| {
                    StorageFlags::split_removal_bytes(flags, removed_key_bytes, removed_value_bytes)
                        .map_err(|e| Error::SplitRemovalBytesClientError(e.to_string()))
                },
                Some(&tx),
                grove_version,
            )
            .cost_as_result()
            .expect("expected to not error")
    }

    fn expect_storage_flags(
        grove_db: &TempGroveDb,
        tx: &Transaction,
        expected_storage_flags: StorageFlags,
        grove_version: &GroveVersion,
    ) {
        let element = grove_db
            .get(
                [b"tree".as_slice()].as_ref(),
                b"key1",
                Some(tx),
                grove_version,
            )
            .unwrap()
            .expect("expected element");
        let storage_flags = StorageFlags::from_element_flags_ref(
            element.get_flags().as_ref().expect("expected flags"),
        )
        .expect("expected to get storage flags")
        .expect("expected storage flags");
        assert_eq!(storage_flags, expected_storage_flags);
    }

    fn verify_references(grove_db: &TempGroveDb, tx: &Transaction) {
        let issues = grove_db
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

    fn create_epoch_map(epoch: u16, bytes: u32) -> BTreeMap<u16, u32> {
        let mut map = BTreeMap::new();
        map.insert(epoch, bytes);
        map
    }

    fn create_two_epoch_map(
        first_epoch: u16,
        first_epoch_bytes: u32,
        second_epoch: u16,
        second_epoch_bytes: u32,
    ) -> BTreeMap<u16, u32> {
        let mut map = BTreeMap::new();
        map.insert(first_epoch, first_epoch_bytes);
        map.insert(second_epoch, second_epoch_bytes);
        map
    }

    #[test]
    fn test_partial_costs_with_no_new_operations_are_same_as_apply_batch() {
        let grove_version = GroveVersion::latest();
        let db = make_empty_grovedb();
        let tx = db.start_transaction();

        db.insert(
            EMPTY_PATH,
            b"documents",
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .cost_as_result()
        .expect("expected to insert successfully");
        db.insert(
            EMPTY_PATH,
            b"balances",
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .cost_as_result()
        .expect("expected to insert successfully");
        let ops = vec![
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![b"documents".to_vec()],
                b"key2".to_vec(),
                Element::new_item_with_flags(b"pizza".to_vec(), Some([0, 1].to_vec())),
            ),
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![b"documents".to_vec()],
                b"key3".to_vec(),
                Element::empty_tree(),
            ),
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![b"documents".to_vec(), b"key3".to_vec()],
                b"key4".to_vec(),
                Element::new_reference(UpstreamFromElementHeightReference(
                    1,
                    vec![b"key2".to_vec()],
                )),
            ),
        ];

        let full_cost = db
            .apply_batch(ops.clone(), None, Some(&tx), grove_version)
            .cost_as_result()
            .expect("expected to apply batch");

        let apply_root_hash = db
            .root_hash(Some(&tx), grove_version)
            .unwrap()
            .expect("expected to get root hash");

        db.get(
            [b"documents".as_slice()].as_ref(),
            b"key2",
            Some(&tx),
            grove_version,
        )
        .unwrap()
        .expect("cannot get element");

        db.get(
            [b"documents".as_slice()].as_ref(),
            b"key3",
            Some(&tx),
            grove_version,
        )
        .unwrap()
        .expect("cannot get element");

        db.get(
            [b"documents".as_slice(), b"key3".as_slice()].as_ref(),
            b"key4",
            Some(&tx),
            grove_version,
        )
        .unwrap()
        .expect("cannot get element");

        tx.rollback().expect("expected to rollback");

        let cost = db
            .apply_partial_batch(
                ops,
                None,
                |_cost, _left_over_ops| Ok(vec![]),
                Some(&tx),
                grove_version,
            )
            .cost_as_result()
            .expect("expected to apply batch");

        let apply_partial_root_hash = db
            .root_hash(Some(&tx), grove_version)
            .unwrap()
            .expect("expected to get root hash");

        db.get(
            [b"documents".as_slice()].as_ref(),
            b"key2",
            Some(&tx),
            grove_version,
        )
        .unwrap()
        .expect("cannot get element");

        db.get(
            [b"documents".as_slice()].as_ref(),
            b"key3",
            Some(&tx),
            grove_version,
        )
        .unwrap()
        .expect("cannot get element");

        db.get(
            [b"documents".as_slice(), b"key3".as_slice()].as_ref(),
            b"key4",
            Some(&tx),
            grove_version,
        )
        .unwrap()
        .expect("cannot get element");

        assert_eq!(full_cost, cost);

        assert_eq!(apply_root_hash, apply_partial_root_hash);
    }

    #[test]
    fn test_partial_costs_with_add_balance_operations() {
        let grove_version = GroveVersion::latest();
        let db = make_empty_grovedb();
        let tx = db.start_transaction();

        db.insert(
            EMPTY_PATH,
            b"documents",
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .cost_as_result()
        .expect("expected to insert successfully");
        db.insert(
            EMPTY_PATH,
            b"balances",
            Element::empty_sum_tree(),
            None,
            None,
            grove_version,
        )
        .cost_as_result()
        .expect("expected to insert successfully");
        let ops = vec![
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![b"documents".to_vec()],
                b"key2".to_vec(),
                Element::new_item_with_flags(b"pizza".to_vec(), Some([0, 1].to_vec())),
            ),
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![b"documents".to_vec()],
                b"key3".to_vec(),
                Element::empty_tree(),
            ),
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![b"documents".to_vec(), b"key3".to_vec()],
                b"key4".to_vec(),
                Element::new_reference(UpstreamFromElementHeightReference(
                    1,
                    vec![b"key2".to_vec()],
                )),
            ),
        ];

        let full_cost = db
            .apply_batch(ops.clone(), None, Some(&tx), grove_version)
            .cost_as_result()
            .expect("expected to apply batch");

        let apply_root_hash = db
            .root_hash(Some(&tx), grove_version)
            .unwrap()
            .expect("expected to get root hash");

        db.get(
            [b"documents".as_slice()].as_ref(),
            b"key2",
            Some(&tx),
            grove_version,
        )
        .unwrap()
        .expect("cannot get element");

        db.get(
            [b"documents".as_slice()].as_ref(),
            b"key3",
            Some(&tx),
            grove_version,
        )
        .unwrap()
        .expect("cannot get element");

        db.get(
            [b"documents".as_slice(), b"key3".as_slice()].as_ref(),
            b"key4",
            Some(&tx),
            grove_version,
        )
        .unwrap()
        .expect("cannot get element");

        tx.rollback().expect("expected to rollback");

        let cost = db
            .apply_partial_batch(
                ops,
                None,
                |_cost, left_over_ops| {
                    assert!(left_over_ops.is_some());
                    assert_eq!(left_over_ops.as_ref().unwrap().len(), 1);
                    let ops_by_root_path = left_over_ops
                        .as_ref()
                        .unwrap()
                        .get(0)
                        .expect("expected to have root path");
                    assert_eq!(ops_by_root_path.len(), 1);
                    let new_ops = vec![QualifiedGroveDbOp::insert_or_replace_op(
                        vec![b"balances".to_vec()],
                        b"person".to_vec(),
                        Element::new_sum_item_with_flags(1000, Some([0, 1].to_vec())),
                    )];
                    Ok(new_ops)
                },
                Some(&tx),
                grove_version,
            )
            .cost_as_result()
            .expect("expected to apply batch");

        let apply_partial_root_hash = db
            .root_hash(Some(&tx), grove_version)
            .unwrap()
            .expect("expected to get root hash");

        db.get(
            [b"documents".as_slice()].as_ref(),
            b"key2",
            Some(&tx),
            grove_version,
        )
        .unwrap()
        .expect("cannot get element");

        db.get(
            [b"documents".as_slice()].as_ref(),
            b"key3",
            Some(&tx),
            grove_version,
        )
        .unwrap()
        .expect("cannot get element");

        db.get(
            [b"documents".as_slice(), b"key3".as_slice()].as_ref(),
            b"key4",
            Some(&tx),
            grove_version,
        )
        .unwrap()
        .expect("cannot get element");

        let balance = db
            .get(
                [b"balances".as_slice()].as_ref(),
                b"person",
                Some(&tx),
                grove_version,
            )
            .unwrap()
            .expect("cannot get element");

        assert_eq!(
            balance.as_sum_item_value().expect("expected sum item"),
            1000
        );

        assert!(full_cost.storage_cost.added_bytes < cost.storage_cost.added_bytes);

        assert_ne!(apply_root_hash, apply_partial_root_hash);
    }

    #[test]
    fn test_one_update_bigger_item_same_epoch_with_reference() {
        let grove_version = GroveVersion::latest();
        let db = make_empty_grovedb();
        let tx = db.start_transaction();

        let owner_id = [1; 32];
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
            EMPTY_PATH,
            b"refs",
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
            Element::new_item_with_flags(
                b"value1".to_vec(),
                Some(StorageFlags::new_single_epoch(0, Some(owner_id)).to_element_flags()),
            ),
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
                Element::new_item_with_flags(
                    b"value100".to_vec(),
                    Some(StorageFlags::new_single_epoch(0, Some(owner_id)).to_element_flags()),
                ),
            ),
            QualifiedGroveDbOp::insert_only_op(
                vec![b"refs".to_vec()],
                b"ref_key".to_vec(),
                Element::new_reference_with_hops(
                    ReferencePathType::AbsolutePathReference(vec![
                        b"tree".to_vec(),
                        b"key1".to_vec(),
                    ]),
                    Some(1),
                ),
            ),
        ];

        apply_batch(&db, ops, &tx, grove_version);

        expect_storage_flags(
            &db,
            &tx,
            StorageFlags::new_single_epoch(0, Some(owner_id)),
            grove_version,
        );

        verify_references(&db, &tx);
    }

    #[test]
    fn test_one_update_bigger_item_different_epoch_with_reference() {
        let grove_version = GroveVersion::latest();
        let db = make_empty_grovedb();

        let owner_id = [1; 32];

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
            EMPTY_PATH,
            b"refs",
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
            Element::new_item_with_flags(
                b"value1".to_vec(),
                Some(StorageFlags::new_single_epoch(0, Some(owner_id)).to_element_flags()),
            ),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("expected to insert item");

        let base_item = b"value1".to_vec();

        for n in 1..150 {
            let tx = db.start_transaction();
            let mut item = base_item.clone();
            item.extend(std::iter::repeat(0).take(n));
            // We are adding n bytes
            let ops = vec![
                QualifiedGroveDbOp::insert_or_replace_op(
                    vec![b"tree".to_vec()],
                    b"key1".to_vec(),
                    Element::new_item_with_flags(
                        item, // value100 if n was 2
                        Some(StorageFlags::new_single_epoch(1, Some(owner_id)).to_element_flags()),
                    ),
                ),
                QualifiedGroveDbOp::insert_only_op(
                    vec![b"refs".to_vec()],
                    b"ref_key".to_vec(),
                    Element::new_reference_with_hops(
                        ReferencePathType::AbsolutePathReference(vec![
                            b"tree".to_vec(),
                            b"key1".to_vec(),
                        ]),
                        None,
                    ),
                ),
            ];

            apply_batch(&db, ops, &tx, grove_version);

            let expected_added_bytes = if n < 15 {
                n as u32 + 3
            } else if n < 124 {
                n as u32 + 4 // the varint requires an extra byte
            } else {
                n as u32 + 5 // the varint requires an extra byte
            };
            expect_storage_flags(
                &db,
                &tx,
                StorageFlags::MultiEpochOwned(
                    0,
                    create_epoch_map(1, expected_added_bytes),
                    owner_id,
                ),
                grove_version,
            );

            verify_references(&db, &tx);
        }
    }

    #[test]
    fn test_one_update_bigger_item_different_base_epoch_with_bytes_in_last_epoch_with_reference() {
        let grove_version = GroveVersion::latest();
        let db = make_empty_grovedb();

        let owner_id = [1; 32];

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
            EMPTY_PATH,
            b"refs",
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
            Element::new_item_with_flags(
                b"value1".to_vec(),
                Some(
                    StorageFlags::MultiEpochOwned(0, create_epoch_map(1, 4), owner_id)
                        .to_element_flags(),
                ),
            ),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("expected to insert item");

        let base_item = b"value1".to_vec();

        for n in 1..150 {
            let tx = db.start_transaction();
            let mut item = base_item.clone();
            item.extend(std::iter::repeat(0).take(n));
            // We are adding n bytes
            let ops = vec![
                QualifiedGroveDbOp::insert_or_replace_op(
                    vec![b"tree".to_vec()],
                    b"key1".to_vec(),
                    Element::new_item_with_flags(
                        item, // value100 if n was 2
                        Some(StorageFlags::new_single_epoch(1, Some(owner_id)).to_element_flags()),
                    ),
                ),
                QualifiedGroveDbOp::insert_only_op(
                    vec![b"refs".to_vec()],
                    b"ref_key".to_vec(),
                    Element::new_reference_with_hops(
                        ReferencePathType::AbsolutePathReference(vec![
                            b"tree".to_vec(),
                            b"key1".to_vec(),
                        ]),
                        None,
                    ),
                ),
            ];

            apply_batch(&db, ops, &tx, grove_version);

            let expected_added_bytes = if n < 15 {
                n as u32 + 4
            } else if n < 123 {
                n as u32 + 5 // the varint requires an extra byte
            } else {
                n as u32 + 6 // the varint requires an extra byte
            };
            expect_storage_flags(
                &db,
                &tx,
                StorageFlags::MultiEpochOwned(
                    0,
                    create_epoch_map(1, expected_added_bytes),
                    owner_id,
                ),
                grove_version,
            );

            verify_references(&db, &tx);
        }
    }

    #[test]
    fn test_one_update_bigger_item_different_base_epoch_with_bytes_in_future_epoch_with_reference()
    {
        let grove_version = GroveVersion::latest();
        let db = make_empty_grovedb();

        let owner_id = [1; 32];

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
            EMPTY_PATH,
            b"refs",
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
            Element::new_item_with_flags(
                b"value1".to_vec(),
                Some(
                    StorageFlags::MultiEpochOwned(0, create_epoch_map(1, 4), owner_id)
                        .to_element_flags(),
                ),
            ),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("expected to insert item");

        let base_item = b"value1".to_vec();

        for n in 1..150 {
            let tx = db.start_transaction();
            let mut item = base_item.clone();
            item.extend(std::iter::repeat(0).take(n));
            // We are adding n bytes
            let ops = vec![
                QualifiedGroveDbOp::insert_or_replace_op(
                    vec![b"tree".to_vec()],
                    b"key1".to_vec(),
                    Element::new_item_with_flags(
                        item, // value100 if n was 2
                        Some(StorageFlags::new_single_epoch(2, Some(owner_id)).to_element_flags()),
                    ),
                ),
                QualifiedGroveDbOp::insert_only_op(
                    vec![b"refs".to_vec()],
                    b"ref_key".to_vec(),
                    Element::new_reference_with_hops(
                        ReferencePathType::AbsolutePathReference(vec![
                            b"tree".to_vec(),
                            b"key1".to_vec(),
                        ]),
                        None,
                    ),
                ),
            ];

            apply_batch(&db, ops, &tx, grove_version);

            let expected_added_bytes = if n < 12 {
                n as u32 + 3
            } else if n < 124 {
                n as u32 + 4 // the varint requires an extra byte
            } else {
                n as u32 + 5 // the varint requires an extra byte
            };
            expect_storage_flags(
                &db,
                &tx,
                StorageFlags::MultiEpochOwned(
                    0,
                    create_two_epoch_map(1, 4, 2, expected_added_bytes),
                    owner_id,
                ),
                grove_version,
            );

            verify_references(&db, &tx);
        }
    }

    #[test]
    fn test_one_update_smaller_item_same_base_epoch_with_reference() {
        let grove_version = GroveVersion::latest();
        let db = make_empty_grovedb();

        let owner_id = [1; 32];

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
            EMPTY_PATH,
            b"refs",
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("expected to insert tree");

        let base_item = b"value1".to_vec();
        let mut original_item = base_item.clone();
        original_item.extend(std::iter::repeat(0).take(150));

        db.insert(
            [b"tree".as_slice()].as_ref(),
            b"key1",
            Element::new_item_with_flags(
                original_item,
                Some(StorageFlags::new_single_epoch(0, Some(owner_id)).to_element_flags()),
            ),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("expected to insert item");

        let to = 150usize;

        for n in (0..to).rev() {
            let tx = db.start_transaction();
            let mut item = base_item.clone();
            item.extend(std::iter::repeat(0).take(n));
            // We are adding n bytes
            let ops = vec![
                QualifiedGroveDbOp::insert_or_replace_op(
                    vec![b"tree".to_vec()],
                    b"key1".to_vec(),
                    Element::new_item_with_flags(
                        item, // value1 if n was 1
                        Some(StorageFlags::new_single_epoch(0, Some(owner_id)).to_element_flags()),
                    ),
                ),
                QualifiedGroveDbOp::insert_only_op(
                    vec![b"refs".to_vec()],
                    b"ref_key".to_vec(),
                    Element::new_reference_with_hops(
                        ReferencePathType::AbsolutePathReference(vec![
                            b"tree".to_vec(),
                            b"key1".to_vec(),
                        ]),
                        None,
                    ),
                ),
            ];

            let removed_bytes = if n > 17 {
                to as u32 - n as u32
            } else {
                to as u32 - n as u32 + 1 // we remove an extra byte
            };

            let storage_removed_bytes = apply_batch(&db, ops, &tx, grove_version)
                .storage_cost
                .removed_bytes;

            let expected_storage_removed_bytes =
                single_epoch_removed_bytes_map(owner_id, 0, removed_bytes);

            assert_eq!(
                storage_removed_bytes,
                StorageRemovedBytes::SectionedStorageRemoval(expected_storage_removed_bytes)
            );

            expect_storage_flags(
                &db,
                &tx,
                StorageFlags::SingleEpochOwned(0, owner_id),
                grove_version,
            );

            verify_references(&db, &tx);
        }
    }

    #[test]
    fn test_one_update_smaller_item_different_base_epoch_with_reference() {
        let grove_version = GroveVersion::latest();
        let db = make_empty_grovedb();

        let owner_id = [1; 32];

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
            EMPTY_PATH,
            b"refs",
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("expected to insert tree");

        let base_item = b"value1".to_vec();
        let mut original_item = base_item.clone();
        original_item.extend(std::iter::repeat(0).take(150));

        db.insert(
            [b"tree".as_slice()].as_ref(),
            b"key1",
            Element::new_item_with_flags(
                original_item,
                Some(StorageFlags::new_single_epoch(0, Some(owner_id)).to_element_flags()),
            ),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("expected to insert item");

        for n in 0..150 {
            let tx = db.start_transaction();
            let mut item = base_item.clone();
            item.extend(std::iter::repeat(0).take(n));
            // We are adding n bytes
            let ops = vec![
                QualifiedGroveDbOp::insert_or_replace_op(
                    vec![b"tree".to_vec()],
                    b"key1".to_vec(),
                    Element::new_item_with_flags(
                        item, // value1 if n was 1
                        Some(StorageFlags::new_single_epoch(1, Some(owner_id)).to_element_flags()),
                    ),
                ),
                QualifiedGroveDbOp::insert_only_op(
                    vec![b"refs".to_vec()],
                    b"ref_key".to_vec(),
                    Element::new_reference_with_hops(
                        ReferencePathType::AbsolutePathReference(vec![
                            b"tree".to_vec(),
                            b"key1".to_vec(),
                        ]),
                        None,
                    ),
                ),
            ];

            apply_batch(&db, ops, &tx, grove_version);

            expect_storage_flags(
                &db,
                &tx,
                StorageFlags::SingleEpochOwned(0, owner_id),
                grove_version,
            );

            verify_references(&db, &tx);
        }
    }

    #[test]
    fn test_one_update_smaller_item_different_base_epoch_with_previous_flags() {
        let grove_version = GroveVersion::latest();
        let db = make_empty_grovedb();
        let tx = db.start_transaction();

        let owner_id = [1; 32];

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
            EMPTY_PATH,
            b"refs",
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
            Element::new_item_with_flags(
                b"value1500".to_vec(), // the 1500 is 4 bytes
                Some(
                    StorageFlags::MultiEpochOwned(0, create_epoch_map(1, 7), owner_id)
                        .to_element_flags(),
                ),
            ),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("expected to insert item");

        // We are removing 2 bytes
        let ops = vec![
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![b"tree".to_vec()],
                b"key1".to_vec(),
                Element::new_item_with_flags(
                    b"value15".to_vec(),
                    Some(StorageFlags::new_single_epoch(1, Some(owner_id)).to_element_flags()),
                ),
            ),
            QualifiedGroveDbOp::insert_only_op(
                vec![b"refs".to_vec()],
                b"ref_key".to_vec(),
                Element::new_reference_with_hops(
                    ReferencePathType::AbsolutePathReference(vec![
                        b"tree".to_vec(),
                        b"key1".to_vec(),
                    ]),
                    None,
                ),
            ),
        ];

        apply_batch(&db, ops, &tx, grove_version);

        expect_storage_flags(
            &db,
            &tx,
            StorageFlags::MultiEpochOwned(0, create_epoch_map(1, 5), owner_id),
            grove_version,
        );

        verify_references(&db, &tx);
    }

    #[test]
    fn test_one_update_smaller_item_different_base_epoch_with_previous_flags_all_multi_epoch() {
        let grove_version = GroveVersion::latest();
        let db = make_empty_grovedb();
        let tx = db.start_transaction();

        let owner_id = [1; 32];

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
            EMPTY_PATH,
            b"refs",
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
            Element::new_item_with_flags(
                b"value1500".to_vec(), // the 1500 is 4 bytes
                Some(
                    StorageFlags::MultiEpochOwned(0, create_epoch_map(1, 7), owner_id)
                        .to_element_flags(),
                ),
            ),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("expected to insert item");

        // We are removing 2 bytes
        let ops = vec![
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![b"tree".to_vec()],
                b"key1".to_vec(),
                Element::new_item_with_flags(
                    b"value".to_vec(),
                    Some(StorageFlags::new_single_epoch(1, Some(owner_id)).to_element_flags()),
                ),
            ),
            QualifiedGroveDbOp::insert_only_op(
                vec![b"refs".to_vec()],
                b"ref_key".to_vec(),
                Element::new_reference_with_hops(
                    ReferencePathType::AbsolutePathReference(vec![
                        b"tree".to_vec(),
                        b"key1".to_vec(),
                    ]),
                    None,
                ),
            ),
        ];

        apply_batch(&db, ops, &tx, grove_version);

        expect_storage_flags(
            &db,
            &tx,
            StorageFlags::SingleEpochOwned(0, owner_id),
            grove_version,
        );

        verify_references(&db, &tx);
    }

    #[test]
    fn test_one_update_bigger_sum_item_same_epoch_with_reference() {
        let grove_version = GroveVersion::latest();
        let db = make_empty_grovedb();
        let tx = db.start_transaction();

        let owner_id = [1; 32];
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
            EMPTY_PATH,
            b"refs",
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
            Element::new_sum_item_with_flags(
                1,
                Some(StorageFlags::new_single_epoch(0, Some(owner_id)).to_element_flags()),
            ),
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
                Element::new_sum_item_with_flags(
                    100000000000,
                    Some(StorageFlags::new_single_epoch(0, Some(owner_id)).to_element_flags()),
                ),
            ),
            QualifiedGroveDbOp::insert_only_op(
                vec![b"refs".to_vec()],
                b"ref_key".to_vec(),
                Element::new_reference_with_hops(
                    ReferencePathType::AbsolutePathReference(vec![
                        b"tree".to_vec(),
                        b"key1".to_vec(),
                    ]),
                    Some(1),
                ),
            ),
        ];

        apply_batch(&db, ops, &tx, grove_version);

        expect_storage_flags(
            &db,
            &tx,
            StorageFlags::new_single_epoch(0, Some(owner_id)),
            grove_version,
        );

        verify_references(&db, &tx);
    }

    #[test]
    fn test_one_update_bigger_sum_item_different_epoch_with_reference() {
        let grove_version = GroveVersion::latest();
        let db = make_empty_grovedb();

        let owner_id = [1; 32];

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
            EMPTY_PATH,
            b"refs",
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
            Element::new_sum_item_with_flags(
                1,
                Some(StorageFlags::new_single_epoch(0, Some(owner_id)).to_element_flags()),
            ),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("expected to insert item");

        let tx = db.start_transaction();
        // We are adding n bytes
        let ops = vec![
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![b"tree".to_vec()],
                b"key1".to_vec(),
                Element::new_sum_item_with_flags(
                    10000000000,
                    Some(StorageFlags::new_single_epoch(1, Some(owner_id)).to_element_flags()),
                ),
            ),
            QualifiedGroveDbOp::insert_only_op(
                vec![b"refs".to_vec()],
                b"ref_key".to_vec(),
                Element::new_reference_with_hops(
                    ReferencePathType::AbsolutePathReference(vec![
                        b"tree".to_vec(),
                        b"key1".to_vec(),
                    ]),
                    None,
                ),
            ),
        ];

        apply_batch(&db, ops, &tx, grove_version);

        expect_storage_flags(
            &db,
            &tx,
            StorageFlags::SingleEpochOwned(0, owner_id), // no change
            grove_version,
        );

        verify_references(&db, &tx);
    }

    #[test]
    fn test_one_update_bigger_item_add_flags() {
        let grove_version = GroveVersion::latest();
        let db = make_empty_grovedb();

        let owner_id = [1; 32];

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
            EMPTY_PATH,
            b"refs",
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
            Element::new_item_with_flags(b"value1".to_vec(), None),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("expected to insert item");

        let base_item = b"value1".to_vec();

        for n in 1..150 {
            let tx = db.start_transaction();
            let mut item = base_item.clone();
            item.extend(std::iter::repeat(0).take(n));
            // We are adding n bytes
            let ops = vec![
                QualifiedGroveDbOp::insert_or_replace_op(
                    vec![b"tree".to_vec()],
                    b"key1".to_vec(),
                    Element::new_item_with_flags(
                        item, // value100 if n was 2
                        Some(StorageFlags::new_single_epoch(1, Some(owner_id)).to_element_flags()),
                    ),
                ),
                QualifiedGroveDbOp::insert_only_op(
                    vec![b"refs".to_vec()],
                    b"ref_key".to_vec(),
                    Element::new_reference_with_hops(
                        ReferencePathType::AbsolutePathReference(vec![
                            b"tree".to_vec(),
                            b"key1".to_vec(),
                        ]),
                        None,
                    ),
                ),
            ];

            apply_batch(&db, ops, &tx, grove_version);

            let _expected_added_bytes = if n < 15 {
                n as u32 + 3
            } else if n < 124 {
                n as u32 + 4 // the varint requires an extra byte
            } else {
                n as u32 + 5 // the varint requires an extra byte
            };
            expect_storage_flags(
                &db,
                &tx,
                StorageFlags::SingleEpochOwned(1, owner_id),
                grove_version,
            );

            verify_references(&db, &tx);
        }
    }
    #[test]
    fn test_one_update_smaller_item_add_flags() {
        let grove_version = GroveVersion::latest();
        let db = make_empty_grovedb();

        let owner_id = [1; 32];

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
            EMPTY_PATH,
            b"refs",
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("expected to insert tree");

        let base_item = b"value1".to_vec();
        let mut original_item = base_item.clone();
        original_item.extend(std::iter::repeat(0).take(150));

        db.insert(
            [b"tree".as_slice()].as_ref(),
            b"key1",
            Element::new_item_with_flags(original_item, None),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("expected to insert item");

        let to = 150usize;

        for n in (0..to).rev() {
            let tx = db.start_transaction();
            let mut item = base_item.clone();
            item.extend(std::iter::repeat(0).take(n));
            // We are adding n bytes
            let ops = vec![
                QualifiedGroveDbOp::insert_or_replace_op(
                    vec![b"tree".to_vec()],
                    b"key1".to_vec(),
                    Element::new_item_with_flags(
                        item, // value1 if n was 1
                        Some(StorageFlags::new_single_epoch(0, Some(owner_id)).to_element_flags()),
                    ),
                ),
                QualifiedGroveDbOp::insert_only_op(
                    vec![b"refs".to_vec()],
                    b"ref_key".to_vec(),
                    Element::new_reference_with_hops(
                        ReferencePathType::AbsolutePathReference(vec![
                            b"tree".to_vec(),
                            b"key1".to_vec(),
                        ]),
                        None,
                    ),
                ),
            ];

            let storage_removed_bytes = apply_batch(&db, ops, &tx, grove_version)
                .storage_cost
                .removed_bytes;

            if n > 113 {
                assert_eq!(storage_removed_bytes, StorageRemovedBytes::NoStorageRemoval);
            } else if n > 17 {
                let removed_bytes = 114 - n as u32;
                assert_eq!(
                    storage_removed_bytes,
                    StorageRemovedBytes::BasicStorageRemoval(removed_bytes)
                );
            } else {
                let removed_bytes = 114 - n as u32 + 1; // because of varint
                assert_eq!(
                    storage_removed_bytes,
                    StorageRemovedBytes::BasicStorageRemoval(removed_bytes)
                );
            };

            expect_storage_flags(
                &db,
                &tx,
                StorageFlags::SingleEpochOwned(0, owner_id),
                grove_version,
            );

            verify_references(&db, &tx);
        }
    }
}
