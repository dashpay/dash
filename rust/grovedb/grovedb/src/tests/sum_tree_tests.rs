//! Sum tree tests

#[cfg(test)]
mod tests {
    use grovedb_merk::{
        element::costs::ElementCostExtensions,
        proofs::Query,
        tree::{kv::ValueDefinedCostType, AggregateData},
        TreeFeatureType::{BasicMerkNode, BigSummedMerkNode, SummedMerkNode},
    };
    use grovedb_storage::StorageBatch;
    use grovedb_version::version::GroveVersion;

    use crate::{
        batch::QualifiedGroveDbOp,
        element::SumValue,
        reference_path::ReferencePathType,
        tests::{make_test_grovedb, TEST_LEAF},
        Element, Error, GroveDb, PathQuery,
    };

    #[test]
    fn test_sum_tree_behaves_like_regular_tree() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);
        db.insert(
            [TEST_LEAF].as_ref(),
            b"key",
            Element::empty_sum_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert tree");

        // Can fetch sum tree
        let sum_tree = db
            .get([TEST_LEAF].as_ref(), b"key", None, grove_version)
            .unwrap()
            .expect("should get tree");
        assert!(matches!(sum_tree, Element::SumTree(..)));

        db.insert(
            [TEST_LEAF, b"key"].as_ref(),
            b"innerkey",
            Element::new_item(vec![1]),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item");
        db.insert(
            [TEST_LEAF, b"key"].as_ref(),
            b"innerkey2",
            Element::new_item(vec![3]),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item");
        db.insert(
            [TEST_LEAF, b"key"].as_ref(),
            b"innerkey3",
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item");

        // Test proper item retrieval
        let item = db
            .get(
                [TEST_LEAF, b"key"].as_ref(),
                b"innerkey",
                None,
                grove_version,
            )
            .unwrap()
            .expect("should get item");
        assert_eq!(item, Element::new_item(vec![1]));

        // Test proof generation
        let mut query = Query::new();
        query.insert_key(b"innerkey2".to_vec());

        let path_query = PathQuery::new_unsized(vec![TEST_LEAF.to_vec(), b"key".to_vec()], query);
        let proof = db
            .prove_query(&path_query, None, grove_version)
            .unwrap()
            .expect("should generate proof");
        let (root_hash, result_set) = GroveDb::verify_query_raw(&proof, &path_query, grove_version)
            .expect("should verify proof");
        assert_eq!(
            root_hash,
            db.grove_db.root_hash(None, grove_version).unwrap().unwrap()
        );
        assert_eq!(result_set.len(), 1);
        assert_eq!(
            Element::deserialize(&result_set[0].value, grove_version)
                .expect("should deserialize element"),
            Element::new_item(vec![3])
        );

        let (root_hash, parent, result_set) =
            GroveDb::verify_query_get_parent_tree_info(&proof, &path_query, grove_version)
                .expect("should verify proof");
        assert_eq!(
            root_hash,
            db.grove_db.root_hash(None, grove_version).unwrap().unwrap()
        );
        assert_eq!(result_set.len(), 1);
        assert_eq!(
            parent,
            SummedMerkNode(0), // because no sum items
        );
    }

    #[test]
    fn test_sum_item_behaves_like_regular_item() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);
        db.insert(
            [TEST_LEAF].as_ref(),
            b"sumkey",
            Element::empty_sum_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert tree");
        db.insert(
            [TEST_LEAF, b"sumkey"].as_ref(),
            b"k1",
            Element::new_item(vec![1]),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert tree");
        db.insert(
            [TEST_LEAF, b"sumkey"].as_ref(),
            b"k2",
            Element::new_sum_item(5),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert tree");
        db.insert(
            [TEST_LEAF, b"sumkey"].as_ref(),
            b"k3",
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert tree");

        // Test proper item retrieval
        let item = db
            .get([TEST_LEAF, b"sumkey"].as_ref(), b"k2", None, grove_version)
            .unwrap()
            .expect("should get item");
        assert_eq!(item, Element::new_sum_item(5));

        // Test proof generation
        let mut query = Query::new();
        query.insert_key(b"k2".to_vec());

        let path_query =
            PathQuery::new_unsized(vec![TEST_LEAF.to_vec(), b"sumkey".to_vec()], query);
        let proof = db
            .prove_query(&path_query, None, grove_version)
            .unwrap()
            .expect("should generate proof");
        let (root_hash, result_set) = GroveDb::verify_query_raw(&proof, &path_query, grove_version)
            .expect("should verify proof");
        assert_eq!(
            root_hash,
            db.grove_db.root_hash(None, grove_version).unwrap().unwrap()
        );
        assert_eq!(result_set.len(), 1);
        let element_from_proof = Element::deserialize(&result_set[0].value, grove_version)
            .expect("should deserialize element");
        assert_eq!(element_from_proof, Element::new_sum_item(5));
        assert_eq!(element_from_proof.sum_value_or_default(), 5);

        let (root_hash, parent, result_set) =
            GroveDb::verify_query_get_parent_tree_info(&proof, &path_query, grove_version)
                .expect("should verify proof");
        assert_eq!(
            root_hash,
            db.grove_db.root_hash(None, grove_version).unwrap().unwrap()
        );
        assert_eq!(result_set.len(), 1);
        assert_eq!(
            parent,
            SummedMerkNode(5), // because no sum items
        );
    }

    #[test]
    fn test_cannot_insert_sum_item_in_regular_tree() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);
        db.insert(
            [TEST_LEAF].as_ref(),
            b"sumkey",
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert tree");
        assert!(matches!(
            db.insert(
                [TEST_LEAF, b"sumkey"].as_ref(),
                b"k1",
                Element::new_sum_item(5),
                None,
                None,
                grove_version
            )
            .unwrap(),
            Err(Error::InvalidInput("cannot add sum item to non sum tree"))
        ));
        assert!(matches!(
            db.insert(
                [TEST_LEAF, b"sumkey"].as_ref(),
                b"k2",
                Element::ItemWithSumItem(b"value".to_vec(), 10, None),
                None,
                None,
                grove_version
            )
            .unwrap(),
            Err(Error::InvalidInput("cannot add sum item to non sum tree"))
        ));
    }

    #[test]
    fn test_item_with_sum_item_updates_sum_tree_sum() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);
        db.insert(
            [TEST_LEAF].as_ref(),
            b"sum_mixed",
            Element::empty_sum_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert sum tree");

        db.insert(
            [TEST_LEAF, b"sum_mixed"].as_ref(),
            b"alpha",
            Element::ItemWithSumItem(b"payload".to_vec(), 6, None),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item with sum");

        let assert_sum = |expected_sum: i64| {
            let element = db
                .get([TEST_LEAF].as_ref(), b"sum_mixed", None, grove_version)
                .unwrap()
                .expect("should fetch sum tree");
            match element {
                Element::SumTree(_, sum, _) => assert_eq!(sum, expected_sum),
                _ => panic!("expected sum tree"),
            }
        };

        assert_sum(6);

        let ops = vec![QualifiedGroveDbOp::insert_or_replace_op(
            vec![TEST_LEAF.to_vec(), b"sum_mixed".to_vec()],
            b"alpha".to_vec(),
            Element::ItemWithSumItem(b"updated".to_vec(), -9, Some(vec![5])),
        )];
        db.apply_batch(ops, None, None, grove_version)
            .unwrap()
            .expect("should replace item with sum");

        assert_sum(-9);

        db.delete(
            [TEST_LEAF, b"sum_mixed"].as_ref(),
            b"alpha",
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should delete item");

        assert_sum(0);
    }

    #[test]
    fn test_homogenous_node_type_in_sum_trees_and_regular_trees() {
        let grove_version = GroveVersion::latest();
        // All elements in a sum tree must have a summed feature type
        let db = make_test_grovedb(grove_version);
        db.insert(
            [TEST_LEAF].as_ref(),
            b"key",
            Element::empty_sum_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert tree");
        // Add sum items
        db.insert(
            [TEST_LEAF, b"key"].as_ref(),
            b"item1",
            Element::new_sum_item(30),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item");
        db.insert(
            [TEST_LEAF, b"key"].as_ref(),
            b"item2",
            Element::new_sum_item(10),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item");
        // Add regular items
        db.insert(
            [TEST_LEAF, b"key"].as_ref(),
            b"item3",
            Element::new_item(vec![10]),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item");
        db.insert(
            [TEST_LEAF, b"key"].as_ref(),
            b"item4",
            Element::new_item(vec![15]),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item");

        let batch = StorageBatch::new();
        let transaction = db.start_transaction();

        // Open merk and check all elements in it
        let merk = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"key"].as_ref().into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open tree");
        assert!(matches!(
            merk.get_feature_type(
                b"item1",
                true,
                None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                grove_version
            )
            .unwrap()
            .expect("node should exist"),
            Some(SummedMerkNode(30))
        ));
        assert!(matches!(
            merk.get_feature_type(
                b"item2",
                true,
                None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                grove_version
            )
            .unwrap()
            .expect("node should exist"),
            Some(SummedMerkNode(10))
        ));
        assert!(matches!(
            merk.get_feature_type(
                b"item3",
                true,
                None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                grove_version
            )
            .unwrap()
            .expect("node should exist"),
            Some(SummedMerkNode(0))
        ));
        assert!(matches!(
            merk.get_feature_type(
                b"item4",
                true,
                None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                grove_version
            )
            .unwrap()
            .expect("node should exist"),
            Some(SummedMerkNode(0))
        ));
        assert_eq!(
            merk.aggregate_data()
                .expect("expected to get sum")
                .as_sum_i64(),
            40
        );

        // Perform the same test on regular trees
        let db = make_test_grovedb(grove_version);
        let transaction = db.start_transaction();

        db.insert(
            [TEST_LEAF].as_ref(),
            b"key",
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert tree");
        db.insert(
            [TEST_LEAF, b"key"].as_ref(),
            b"item1",
            Element::new_item(vec![30]),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item");
        db.insert(
            [TEST_LEAF, b"key"].as_ref(),
            b"item2",
            Element::new_item(vec![10]),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item");

        let merk = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"key"].as_ref().into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open tree");
        assert!(matches!(
            merk.get_feature_type(
                b"item1",
                true,
                Some(&Element::value_defined_cost_for_serialized_value),
                grove_version
            )
            .unwrap()
            .expect("node should exist"),
            Some(BasicMerkNode)
        ));
        assert!(matches!(
            merk.get_feature_type(
                b"item2",
                true,
                Some(&Element::value_defined_cost_for_serialized_value),
                grove_version
            )
            .unwrap()
            .expect("node should exist"),
            Some(BasicMerkNode)
        ));
        assert_eq!(
            merk.aggregate_data().expect("expected to get sum"),
            AggregateData::NoAggregateData
        );
    }

    #[test]
    fn test_sum_tree_feature() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);
        db.insert(
            [TEST_LEAF].as_ref(),
            b"key",
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert tree");

        let batch = StorageBatch::new();

        let transaction = db.start_transaction();
        // Sum should be non for non sum tree
        // TODO: change interface to retrieve element directly
        let merk = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"key"].as_ref().into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open tree");
        assert_eq!(
            merk.aggregate_data().expect("expected to get sum"),
            AggregateData::NoAggregateData
        );

        // Add sum tree
        db.insert(
            [TEST_LEAF].as_ref(),
            b"key2",
            Element::empty_sum_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert sum tree");
        let sum_tree = db
            .get([TEST_LEAF].as_ref(), b"key2", None, grove_version)
            .unwrap()
            .expect("should retrieve tree");
        assert_eq!(sum_tree.sum_value_or_default(), 0);

        // Add sum items to the sum tree
        db.insert(
            [TEST_LEAF, b"key2"].as_ref(),
            b"item1",
            Element::new_sum_item(30),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item");
        // TODO: change interface to retrieve element directly
        let merk = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"key2"].as_ref().into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open tree");
        assert_eq!(
            merk.aggregate_data().expect("expected to get sum"),
            AggregateData::Sum(30)
        );

        // Add more sum items
        db.insert(
            [TEST_LEAF, b"key2"].as_ref(),
            b"item2",
            Element::new_sum_item(-10),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item");
        db.insert(
            [TEST_LEAF, b"key2"].as_ref(),
            b"item3",
            Element::new_sum_item(50),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item");
        let merk = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"key2"].as_ref().into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open tree");
        assert_eq!(
            merk.aggregate_data().expect("expected to get sum"),
            AggregateData::Sum(70)
        ); // 30 - 10 + 50 = 70

        // Add non sum items, result should remain the same
        db.insert(
            [TEST_LEAF, b"key2"].as_ref(),
            b"item4",
            Element::new_item(vec![29]),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item");
        let merk = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"key2"].as_ref().into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open tree");
        assert_eq!(
            merk.aggregate_data().expect("expected to get sum"),
            AggregateData::Sum(70)
        );

        // Update existing sum items
        db.insert(
            [TEST_LEAF, b"key2"].as_ref(),
            b"item2",
            Element::new_sum_item(10),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item");
        db.insert(
            [TEST_LEAF, b"key2"].as_ref(),
            b"item3",
            Element::new_sum_item(-100),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item");
        let merk = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"key2"].as_ref().into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open tree");
        assert_eq!(
            merk.aggregate_data().expect("expected to get sum"),
            AggregateData::Sum(-60)
        ); // 30 + 10 - 100 = -60

        // We can not replace a normal item with a sum item, so let's delete it first
        db.delete(
            [TEST_LEAF, b"key2"].as_ref(),
            b"item4",
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("expected to delete");
        // Use a large value
        db.insert(
            [TEST_LEAF, b"key2"].as_ref(),
            b"item4",
            Element::new_sum_item(10000000),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item");
        let merk = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"key2"].as_ref().into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open tree");
        assert_eq!(
            merk.aggregate_data().expect("expected to get sum"),
            AggregateData::Sum(9999940)
        ); // 30 +
           // 10 -
           // 100 +
           // 10000000
    }

    #[test]
    fn test_sum_tree_overflow() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);
        db.insert(
            [TEST_LEAF].as_ref(),
            b"key",
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert tree");

        let batch = StorageBatch::new();

        let transaction = db.start_transaction();
        // Sum should be non for non sum tree
        // TODO: change interface to retrieve element directly
        let merk = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"key"].as_ref().into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open tree");
        assert_eq!(
            merk.aggregate_data().expect("expected to get sum"),
            AggregateData::NoAggregateData
        );

        // Add sum tree
        db.insert(
            [TEST_LEAF].as_ref(),
            b"key2",
            Element::empty_sum_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert sum tree");
        let sum_tree = db
            .get([TEST_LEAF].as_ref(), b"key2", None, grove_version)
            .unwrap()
            .expect("should retrieve tree");
        assert_eq!(sum_tree.sum_value_or_default(), 0);

        // Add sum items to the sum tree
        db.insert(
            [TEST_LEAF, b"key2"].as_ref(),
            b"item1",
            Element::new_sum_item(SumValue::MAX),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item");
        // TODO: change interface to retrieve element directly
        let merk = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"key2"].as_ref().into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open tree");
        assert_eq!(
            merk.aggregate_data().expect("expected to get sum"),
            AggregateData::Sum(SumValue::MAX)
        );

        // Subtract 10 from Max should work
        db.insert(
            [TEST_LEAF, b"key2"].as_ref(),
            b"item2",
            Element::new_sum_item(-10),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item");
        let merk = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"key2"].as_ref().into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open tree");
        assert_eq!(
            merk.aggregate_data().expect("expected to get sum"),
            AggregateData::Sum(SumValue::MAX - 10)
        );

        // Add 20 from Max should overflow
        db.insert(
            [TEST_LEAF, b"key2"].as_ref(),
            b"item3",
            Element::new_sum_item(20),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect_err("should not be able to insert item");
        let merk = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"key2"].as_ref().into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open tree");
        assert_eq!(
            merk.aggregate_data().expect("expected to get sum"),
            AggregateData::Sum(SumValue::MAX - 10)
        );

        // Add non sum items, result should remain the same
        db.insert(
            [TEST_LEAF, b"key2"].as_ref(),
            b"item4",
            Element::new_item(vec![29]),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item");
        let merk = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"key2"].as_ref().into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open tree");
        assert_eq!(
            merk.aggregate_data().expect("expected to get sum"),
            AggregateData::Sum(SumValue::MAX - 10)
        );

        // Update existing sum item will overflow
        db.insert(
            [TEST_LEAF, b"key2"].as_ref(),
            b"item2",
            Element::new_sum_item(10), // we are replacing -10 with 10
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect_err("should not be able to insert item");
        let merk = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"key2"].as_ref().into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open tree");
        assert_eq!(
            merk.aggregate_data().expect("expected to get sum"),
            AggregateData::Sum(SumValue::MAX - 10)
        );

        // Update existing sum item will overflow
        db.insert(
            [TEST_LEAF, b"key2"].as_ref(),
            b"item2",
            Element::new_sum_item(SumValue::MIN), // we are replacing -10 with SumValue::MIN
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should be able to insert item");
        let merk = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"key2"].as_ref().into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open tree");
        assert_eq!(
            merk.aggregate_data().expect("expected to get sum"),
            AggregateData::Sum(-1)
        );

        db.insert(
            [TEST_LEAF, b"key2"].as_ref(),
            b"item3",
            Element::new_sum_item(-40),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should be able to insert item");

        let merk = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"key2"].as_ref().into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open tree");
        assert_eq!(
            merk.aggregate_data().expect("expected to get sum"),
            AggregateData::Sum(-41)
        );

        // Deleting item1 should make us overflow
        db.delete(
            [TEST_LEAF, b"key2"].as_ref(),
            b"item1",
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect_err("expected not be able to delete");
        let merk = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"key2"].as_ref().into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open tree");
        assert_eq!(
            merk.aggregate_data().expect("expected to get sum"),
            AggregateData::Sum(-41)
        );
    }

    #[test]
    fn test_sum_tree_propagation() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);
        // Tree
        //   SumTree
        //      SumTree
        //        Item1
        //        SumItem1
        //        SumItem2
        //      SumItem3
        db.insert(
            [TEST_LEAF].as_ref(),
            b"key",
            Element::empty_sum_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert tree");
        db.insert(
            [TEST_LEAF, b"key"].as_ref(),
            b"tree2",
            Element::empty_sum_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert tree");
        db.insert(
            [TEST_LEAF, b"key"].as_ref(),
            b"sumitem3",
            Element::new_sum_item(20),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert tree");
        db.insert(
            [TEST_LEAF, b"key", b"tree2"].as_ref(),
            b"item1",
            Element::new_item(vec![2]),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item");
        db.insert(
            [TEST_LEAF, b"key", b"tree2"].as_ref(),
            b"sumitem1",
            Element::new_sum_item(5),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item");
        db.insert(
            [TEST_LEAF, b"key", b"tree2"].as_ref(),
            b"sumitem2",
            Element::new_sum_item(10),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item");
        db.insert(
            [TEST_LEAF, b"key", b"tree2"].as_ref(),
            b"item2",
            Element::new_reference(ReferencePathType::AbsolutePathReference(vec![
                TEST_LEAF.to_vec(),
                b"key".to_vec(),
                b"tree2".to_vec(),
                b"sumitem1".to_vec(),
            ])),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item");

        let sum_tree = db
            .get([TEST_LEAF].as_ref(), b"key", None, grove_version)
            .unwrap()
            .expect("should fetch tree");
        assert_eq!(sum_tree.sum_value_or_default(), 35);

        let batch = StorageBatch::new();
        let transaction = db.start_transaction();

        // Assert node feature types
        let test_leaf_merk = db
            .open_transactional_merk_at_path(
                [TEST_LEAF].as_ref().into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open tree");
        assert!(matches!(
            test_leaf_merk
                .get_feature_type(
                    b"key",
                    true,
                    Some(&Element::value_defined_cost_for_serialized_value),
                    grove_version
                )
                .unwrap()
                .expect("node should exist"),
            Some(BasicMerkNode)
        ));

        let parent_sum_tree = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"key"].as_ref().into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open tree");
        assert!(matches!(
            parent_sum_tree
                .get_feature_type(
                    b"tree2",
                    true,
                    Some(&Element::value_defined_cost_for_serialized_value),
                    grove_version
                )
                .unwrap()
                .expect("node should exist"),
            Some(SummedMerkNode(15)) /* 15 because the child sum tree has one sum item of
                                      * value 5 and
                                      * another of value 10 */
        ));

        let child_sum_tree = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"key", b"tree2"].as_ref().into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open tree");
        assert!(matches!(
            child_sum_tree
                .get_feature_type(
                    b"item1",
                    true,
                    None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                    grove_version
                )
                .unwrap()
                .expect("node should exist"),
            Some(SummedMerkNode(0))
        ));
        assert!(matches!(
            child_sum_tree
                .get_feature_type(
                    b"sumitem1",
                    true,
                    None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                    grove_version
                )
                .unwrap()
                .expect("node should exist"),
            Some(SummedMerkNode(5))
        ));
        assert!(matches!(
            child_sum_tree
                .get_feature_type(
                    b"sumitem2",
                    true,
                    None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                    grove_version
                )
                .unwrap()
                .expect("node should exist"),
            Some(SummedMerkNode(10))
        ));

        // TODO: should references take the sum of the referenced element??
        assert!(matches!(
            child_sum_tree
                .get_feature_type(
                    b"item2",
                    true,
                    None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                    grove_version
                )
                .unwrap()
                .expect("node should exist"),
            Some(SummedMerkNode(0))
        ));
    }

    #[test]
    fn test_big_sum_tree_propagation() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);
        // Tree
        //   BigSumTree
        //      SumTree1
        //        SumItem1
        //        SumItem2
        //      SumTree2
        //        SumItem3
        //      SumItem4
        db.insert(
            [TEST_LEAF].as_ref(),
            b"big_sum_tree",
            Element::empty_big_sum_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert tree");
        db.insert(
            [TEST_LEAF, b"big_sum_tree"].as_ref(),
            b"sum_tree_1",
            Element::empty_sum_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert tree");
        db.insert(
            [TEST_LEAF, b"big_sum_tree"].as_ref(),
            b"sum_tree_2",
            Element::empty_sum_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert tree");
        db.insert(
            [TEST_LEAF, b"big_sum_tree", b"sum_tree_1"].as_ref(),
            b"item1",
            Element::new_item(vec![2]),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item");
        db.insert(
            [TEST_LEAF, b"big_sum_tree", b"sum_tree_1"].as_ref(),
            b"sum_item_1",
            Element::new_sum_item(SumValue::MAX - 40),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item");
        db.insert(
            [TEST_LEAF, b"big_sum_tree", b"sum_tree_1"].as_ref(),
            b"sum_item_2",
            Element::new_sum_item(30),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item");
        db.insert(
            [TEST_LEAF, b"big_sum_tree", b"sum_tree_1"].as_ref(),
            b"ref_1",
            Element::new_reference(ReferencePathType::AbsolutePathReference(vec![
                TEST_LEAF.to_vec(),
                b"big_sum_tree".to_vec(),
                b"sum_tree_1".to_vec(),
                b"sum_item_1".to_vec(),
            ])),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item");

        db.insert(
            [TEST_LEAF, b"big_sum_tree", b"sum_tree_2"].as_ref(),
            b"sum_item_3",
            Element::new_sum_item(SumValue::MAX - 50),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item");

        let sum_tree = db
            .get([TEST_LEAF].as_ref(), b"big_sum_tree", None, grove_version)
            .unwrap()
            .expect("should fetch tree");
        assert_eq!(
            sum_tree.big_sum_value_or_default(),
            (SumValue::MAX - 10) as i128 + (SumValue::MAX - 50) as i128
        );

        db.insert(
            [TEST_LEAF, b"big_sum_tree"].as_ref(),
            b"sum_item_4",
            Element::new_sum_item(SumValue::MAX - 70),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item");

        let sum_tree = db
            .get([TEST_LEAF].as_ref(), b"big_sum_tree", None, grove_version)
            .unwrap()
            .expect("should fetch tree");
        assert_eq!(
            sum_tree.big_sum_value_or_default(),
            (SumValue::MAX - 10) as i128
                + (SumValue::MAX - 50) as i128
                + (SumValue::MAX - 70) as i128
        );

        let batch = StorageBatch::new();
        let transaction = db.start_transaction();

        // Assert node feature types
        let test_leaf_merk = db
            .open_transactional_merk_at_path(
                [TEST_LEAF].as_ref().into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open tree");
        assert!(matches!(
            test_leaf_merk
                .get_feature_type(
                    b"big_sum_tree",
                    true,
                    Some(&Element::value_defined_cost_for_serialized_value),
                    grove_version
                )
                .unwrap()
                .expect("node should exist"),
            Some(BasicMerkNode)
        ));

        let parent_sum_tree = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"big_sum_tree"].as_ref().into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open tree");
        let feature_type = parent_sum_tree
            .get_feature_type(
                b"sum_tree_1",
                true,
                Some(&Element::value_defined_cost_for_serialized_value),
                grove_version,
            )
            .unwrap()
            .expect("node should exist")
            .expect("expected feature type");
        assert_eq!(
            feature_type,
            BigSummedMerkNode((SumValue::MAX - 10) as i128)
        );

        let feature_type = parent_sum_tree
            .get_feature_type(
                b"sum_item_4",
                true,
                Some(&Element::value_defined_cost_for_serialized_value),
                grove_version,
            )
            .unwrap()
            .expect("node should exist")
            .expect("expected feature type");
        assert_eq!(
            feature_type,
            BigSummedMerkNode((SumValue::MAX - 70) as i128)
        );

        let child_sum_tree = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"big_sum_tree", b"sum_tree_1"].as_ref().into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open tree");
        assert_eq!(
            child_sum_tree
                .get_feature_type(
                    b"item1",
                    true,
                    None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                    grove_version
                )
                .unwrap()
                .expect("node should exist"),
            Some(SummedMerkNode(0))
        );
        assert_eq!(
            child_sum_tree
                .get_feature_type(
                    b"sum_item_1",
                    true,
                    None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                    grove_version
                )
                .unwrap()
                .expect("node should exist"),
            Some(SummedMerkNode(SumValue::MAX - 40))
        );
        assert_eq!(
            child_sum_tree
                .get_feature_type(
                    b"sum_item_2",
                    true,
                    None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                    grove_version
                )
                .unwrap()
                .expect("node should exist"),
            Some(SummedMerkNode(30))
        );

        assert_eq!(
            child_sum_tree
                .get_feature_type(
                    b"ref_1",
                    true,
                    None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                    grove_version
                )
                .unwrap()
                .expect("node should exist"),
            Some(SummedMerkNode(0))
        );

        let child_sum_tree_2 = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"big_sum_tree", b"sum_tree_2"].as_ref().into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open tree");

        assert_eq!(
            child_sum_tree_2
                .get_feature_type(
                    b"sum_item_3",
                    true,
                    None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                    grove_version
                )
                .unwrap()
                .expect("node should exist"),
            Some(SummedMerkNode(SumValue::MAX - 50))
        );
    }

    #[test]
    fn test_sum_tree_with_batches() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);
        let ops = vec![
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![TEST_LEAF.to_vec()],
                b"key1".to_vec(),
                Element::empty_sum_tree(),
            ),
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![TEST_LEAF.to_vec(), b"key1".to_vec()],
                b"a".to_vec(),
                Element::new_item(vec![214]),
            ),
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![TEST_LEAF.to_vec(), b"key1".to_vec()],
                b"b".to_vec(),
                Element::new_sum_item(10),
            ),
        ];
        db.apply_batch(ops, None, None, grove_version)
            .unwrap()
            .expect("should apply batch");
        let transaction = db.start_transaction();

        let batch = StorageBatch::new();
        let sum_tree = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"key1"].as_ref().into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open tree");

        assert_eq!(
            sum_tree
                .get_feature_type(
                    b"a",
                    true,
                    Some(&Element::value_defined_cost_for_serialized_value),
                    grove_version
                )
                .unwrap()
                .expect("node should exist"),
            Some(SummedMerkNode(0))
        );
        assert_eq!(
            sum_tree
                .get_feature_type(
                    b"b",
                    true,
                    Some(&Element::value_defined_cost_for_serialized_value),
                    grove_version
                )
                .unwrap()
                .expect("node should exist"),
            Some(SummedMerkNode(10))
        );

        // Create new batch to use existing tree
        let ops = vec![QualifiedGroveDbOp::insert_or_replace_op(
            vec![TEST_LEAF.to_vec(), b"key1".to_vec()],
            b"c".to_vec(),
            Element::new_sum_item(10),
        )];
        db.apply_batch(ops, None, None, grove_version)
            .unwrap()
            .expect("should apply batch");
        let transaction = db.start_transaction();

        let batch = StorageBatch::new();
        let sum_tree = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"key1"].as_ref().into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open tree");
        assert_eq!(
            sum_tree
                .get_feature_type(
                    b"c",
                    true,
                    None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                    grove_version
                )
                .unwrap()
                .expect("node should exist"),
            Some(SummedMerkNode(10))
        );
        assert_eq!(
            sum_tree.aggregate_data().expect("expected to get sum"),
            AggregateData::Sum(20)
        );

        // Test propagation
        // Add a new sum tree with its own sum items, should affect sum of original
        // tree
        let ops = vec![
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![TEST_LEAF.to_vec(), b"key1".to_vec()],
                b"d".to_vec(),
                Element::empty_sum_tree(),
            ),
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![TEST_LEAF.to_vec(), b"key1".to_vec(), b"d".to_vec()],
                b"first".to_vec(),
                Element::new_sum_item(4),
            ),
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![TEST_LEAF.to_vec(), b"key1".to_vec(), b"d".to_vec()],
                b"second".to_vec(),
                Element::new_item(vec![4]),
            ),
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![TEST_LEAF.to_vec(), b"key1".to_vec()],
                b"e".to_vec(),
                Element::empty_sum_tree(),
            ),
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![TEST_LEAF.to_vec(), b"key1".to_vec(), b"e".to_vec()],
                b"first".to_vec(),
                Element::new_sum_item(12),
            ),
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![TEST_LEAF.to_vec(), b"key1".to_vec(), b"e".to_vec()],
                b"second".to_vec(),
                Element::new_item(vec![4]),
            ),
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![TEST_LEAF.to_vec(), b"key1".to_vec(), b"e".to_vec()],
                b"third".to_vec(),
                Element::empty_sum_tree(),
            ),
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![
                    TEST_LEAF.to_vec(),
                    b"key1".to_vec(),
                    b"e".to_vec(),
                    b"third".to_vec(),
                ],
                b"a".to_vec(),
                Element::new_sum_item(5),
            ),
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![
                    TEST_LEAF.to_vec(),
                    b"key1".to_vec(),
                    b"e".to_vec(),
                    b"third".to_vec(),
                ],
                b"b".to_vec(),
                Element::new_item(vec![5]),
            ),
        ];
        db.apply_batch(ops, None, None, grove_version)
            .unwrap()
            .expect("should apply batch");
        let transaction = db.start_transaction();

        let batch = StorageBatch::new();
        let sum_tree = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"key1"].as_ref().into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open tree");
        assert_eq!(
            sum_tree.aggregate_data().expect("expected to get sum"),
            AggregateData::Sum(41)
        );
    }

    #[test]
    fn test_big_sum_tree_with_batches() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);
        let ops = vec![
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![TEST_LEAF.to_vec()],
                b"key1".to_vec(),
                Element::empty_big_sum_tree(),
            ),
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![TEST_LEAF.to_vec(), b"key1".to_vec()],
                b"a".to_vec(),
                Element::new_item(vec![214]),
            ),
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![TEST_LEAF.to_vec(), b"key1".to_vec()],
                b"b".to_vec(),
                Element::new_sum_item(10),
            ),
        ];
        db.apply_batch(ops, None, None, grove_version)
            .unwrap()
            .expect("should apply batch");
        let transaction = db.start_transaction();

        let batch = StorageBatch::new();
        let sum_tree = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"key1"].as_ref().into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open tree");

        assert_eq!(
            sum_tree
                .get_feature_type(
                    b"a",
                    true,
                    Some(&Element::value_defined_cost_for_serialized_value),
                    grove_version
                )
                .unwrap()
                .expect("node should exist"),
            Some(BigSummedMerkNode(0))
        );
        assert_eq!(
            sum_tree
                .get_feature_type(
                    b"b",
                    true,
                    Some(&Element::value_defined_cost_for_serialized_value),
                    grove_version
                )
                .unwrap()
                .expect("node should exist"),
            Some(BigSummedMerkNode(10))
        );

        // Create new batch to use existing tree
        let ops = vec![QualifiedGroveDbOp::insert_or_replace_op(
            vec![TEST_LEAF.to_vec(), b"key1".to_vec()],
            b"c".to_vec(),
            Element::new_sum_item(10),
        )];
        db.apply_batch(ops, None, None, grove_version)
            .unwrap()
            .expect("should apply batch");
        let transaction = db.start_transaction();

        let batch = StorageBatch::new();
        let sum_tree = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"key1"].as_ref().into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open tree");
        assert_eq!(
            sum_tree
                .get_feature_type(
                    b"c",
                    true,
                    None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                    grove_version
                )
                .unwrap()
                .expect("node should exist"),
            Some(BigSummedMerkNode(10))
        );
        assert_eq!(
            sum_tree.aggregate_data().expect("expected to get sum"),
            AggregateData::BigSum(20)
        );

        // Test propagation
        // Add a new sum tree with its own sum items, should affect sum of original
        // tree
        let ops = vec![
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![TEST_LEAF.to_vec(), b"key1".to_vec()],
                b"d".to_vec(),
                Element::empty_sum_tree(),
            ),
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![TEST_LEAF.to_vec(), b"key1".to_vec(), b"d".to_vec()],
                b"first".to_vec(),
                Element::new_sum_item(4),
            ),
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![TEST_LEAF.to_vec(), b"key1".to_vec(), b"d".to_vec()],
                b"second".to_vec(),
                Element::new_item(vec![4]),
            ),
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![TEST_LEAF.to_vec(), b"key1".to_vec()],
                b"e".to_vec(),
                Element::empty_sum_tree(),
            ),
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![TEST_LEAF.to_vec(), b"key1".to_vec(), b"e".to_vec()],
                b"first".to_vec(),
                Element::new_sum_item(12),
            ),
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![TEST_LEAF.to_vec(), b"key1".to_vec(), b"e".to_vec()],
                b"second".to_vec(),
                Element::new_item(vec![4]),
            ),
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![TEST_LEAF.to_vec(), b"key1".to_vec(), b"e".to_vec()],
                b"third".to_vec(),
                Element::empty_sum_tree(),
            ),
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![
                    TEST_LEAF.to_vec(),
                    b"key1".to_vec(),
                    b"e".to_vec(),
                    b"third".to_vec(),
                ],
                b"a".to_vec(),
                Element::new_sum_item(5),
            ),
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![
                    TEST_LEAF.to_vec(),
                    b"key1".to_vec(),
                    b"e".to_vec(),
                    b"third".to_vec(),
                ],
                b"b".to_vec(),
                Element::new_item(vec![5]),
            ),
        ];
        db.apply_batch(ops, None, None, grove_version)
            .unwrap()
            .expect("should apply batch");
        let transaction = db.start_transaction();

        let batch = StorageBatch::new();
        let sum_tree = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"key1"].as_ref().into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open tree");
        assert_eq!(
            sum_tree.aggregate_data().expect("expected to get sum"),
            AggregateData::BigSum(41)
        );
    }

    #[test]
    fn test_verify_query_get_parent_tree_info() {
        let grove_version = GroveVersion::latest();
        #[rustfmt::skip]
        let proof = hex::decode("00fb013d01a2704ddea5e5d945adbb6676daf2009e895ee02e810f85542cb1f6ae6d6\
        760e204014000240201205c140e655c0265bbc2a808716de1847985135918ad51cdfd0b76664ba95ba37c0084db131b8025b950fa24fa2\
        e843c73108263e572b9946b21c2c3fd9627e407c61001cf348c325b7635a8bdf70e9ce480344dae0ff1b150982352c036ab0ab80673360\
        28919e738c1d682a834178cbd56398ae1039b2a2fa1714c71f09da1588354fbe9100401580024020120ddce697de2792f0e584961d5596\
        b5e5f411e677832f21fb13415b3eced5bbbac0061be5470c86feef008a0f59f316dd2cde93a29378cb3eaaefc5afc92cf1076111102868\
        dbce2e37187e87d5440fa14b02e55c816bf6b2a42218b1252d438513c35721001728de12d58ac77d35ad591df34db59e929ec1f0d6e87d\
        76785145f890830d3c61111020140d101788e3a259e0f42b3b95d242d9b20d3783f8875ccfd75c419a364cc19a267cc8904202d4359152\
        2d8914e9cf3113acabe0d5c3d287ac95463bb6ee9803f30ac1dd26c00050201010100cf9f9c0c4ba2e279c21006e07d40b0b939eed14e6\
        09ffe03111f7bb111248def10016a2dd3b0bea7de92002da1e4298e51d9337cfe85a57f82b88c94e6079e1f1916110224c13650a211657\
        be239cba81f825b49b8b32d4287a641dc824066b61c7e94241001321f2ab6e866d75692664e459ba978247b9e8dc2d31cc03609ca9a2f8\
        b11afb31101202d43591522d8914e9cf3113acabe0d5c3d287ac95463bb6ee9803f30ac1dd26c59017326e2d5ed8b71c6a23737c6a7ae2\
        386cf0562e80c8ccfddc01aad545ac240c6040101001202010e646972656374507572636861736500fbc1fe44cd9d71ef7811b65385c98\
        8e4ea048a2b2c7b7d5513ad94b329437c6710010101bb018056d40565a1a6baa036c98ddf3d242057eae0a26fb3b8755c78d176aa49d44\
        8020f873d92cda86ea3c81844789a6635072e58e16551802704e466c0e6cd11ea2e10010791683dce9b978a9f23bbf0c10e49d791251e6\
        abad13e99174d42e7fb1bc65004046d696e74000b020107746f6b656e49640007affede04d18f1e14d4288aa17368b96fb363987e81e34\
        ee462f9313ac2395c10014a88b93b931ec8b3ae6ad028b40c735d9f621f7ffe1018dce33c81c1ddb60b4e111101046d696e744a0401000\
        003020000651929e1747381a16157515e5447625502f3a79843859a0a929d24c605c0b23a02c4d4ab8e6aaf3cbab84daf126cc9b454be2\
        6f097992462859547a3f18b751fca100001584a0420ddce697de2792f0e584961d5596b5e5f411e677832f21fb13415b3eced5bbbac000\
        6020102000000960fb81fab3ec029ae3b2361395b11cb8a2cf6fc9371c69359fdb2fe055b16dc0120ddce697de2792f0e584961d5596b5\
        e5f411e677832f21fb13415b3eced5bbbac2b0402000000050201014d00256c1403624ce7e1a72b402ae793333eeae622b41c943c7d385\
        1e93e1fe2c40301020000940166edea757853345f867b571b3caec6a5222f735f1250bd47701a0f74925055b404014d00240201209a530\
        aace4c548b8d8d8b0207a198a39abb674474b41b36765a634f3f7b25f0900d5807a08f8c32cae3189189485b62b982529a7bc9faf241eb\
        5a02538b743d3f4100401580003020000651929e1747381a16157515e5447625502f3a79843859a0a929d24c605c0b23a1101014d49042\
        09a530aace4c548b8d8d8b0207a198a39abb674474b41b36765a634f3f7b25f0900050201015300942e7784de40db01dd61f82f2dac500\
        e3c48bf833712e8404f075d1407ee048501209a530aace4c548b8d8d8b0207a198a39abb674474b41b36765a634f3f7b25f096c0158642\
        68816552789d60452cf54975b46f01454b9e4303b8d5fe01ac1909be37f040153002504012097052066db888f35b30814ca4bd2ce6cb10\
        efbca3402166b2d2b2efdd081ad0c02006828dad691b686174f6152f1fb15c4fad8d109ce9006b4d8379926dbf9698452100101536b042\
        097052066db888f35b30814ca4bd2ce6cb10efbca3402166b2d2b2efdd081ad0c0027030201230297052066db888f35b30814ca4bd2ce6\
        cb10efbca3402166b2d2b2efdd081ad0c000060cfba1b62b0b3c408b50b1718ce1cb20b8f6fbb5cff024966a7d78beb4\
        12fc90001").expect("expected to decode hex");

        let path_query = PathQuery::new_single_key(
            vec![
                vec![0x58],
                hex::decode("ddce697de2792f0e584961d5596b5e5f411e677832f21fb13415b3eced5bbbac")
                    .unwrap(),
                0u16.to_be_bytes().to_vec(),
                vec![0x4d],
                hex::decode("9a530aace4c548b8d8d8b0207a198a39abb674474b41b36765a634f3f7b25f09")
                    .unwrap(),
                vec![0x53],
            ],
            hex::decode("97052066db888f35b30814ca4bd2ce6cb10efbca3402166b2d2b2efdd081ad0c")
                .unwrap(),
        );

        let (_root_hash, parent, result_set) =
            GroveDb::verify_query_get_parent_tree_info(&proof, &path_query, grove_version)
                .expect("should verify proof");

        assert_eq!(parent, SummedMerkNode(1));

        // We can also check the result if desired
        assert_eq!(result_set.len(), 1);

        let (_path, _key, maybe_element) = &result_set[0];

        let element = maybe_element.as_ref().expect("expected Some(element)");

        assert!(
            matches!(element, Element::SumItem(1, _)),
            "expected SumItem(1), got: {:?}",
            element
        );
    }

    #[test]
    fn test_count_sum_tree_sum_propagates_to_sum_tree_parent() {
        // Test that sums from CountSumTree propagate correctly to SumTree parent
        //
        // Tree structure:
        //   TEST_LEAF
        //     └── sum_tree_parent (SumTree)
        //           ├── count_sum_child (CountSumTree)
        //           │     ├── sum_item_1 = 100
        //           │     ├── sum_item_2 = 200
        //           │     └── sum_item_3 = -50
        //           └── direct_sum_item = 25
        //
        // Expected: sum_tree_parent sum = 100 + 200 + (-50) + 25 = 275

        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        // Create parent SumTree
        db.insert(
            [TEST_LEAF].as_ref(),
            b"sum_tree_parent",
            Element::empty_sum_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert sum_tree_parent");

        // Create child CountSumTree inside the SumTree
        db.insert(
            [TEST_LEAF, b"sum_tree_parent"].as_ref(),
            b"count_sum_child",
            Element::new_count_sum_tree(None),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert count_sum_child");

        // Insert sum items into CountSumTree
        db.insert(
            [TEST_LEAF, b"sum_tree_parent", b"count_sum_child"].as_ref(),
            b"sum_item_1",
            Element::new_sum_item(100),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert sum_item_1");

        db.insert(
            [TEST_LEAF, b"sum_tree_parent", b"count_sum_child"].as_ref(),
            b"sum_item_2",
            Element::new_sum_item(200),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert sum_item_2");

        db.insert(
            [TEST_LEAF, b"sum_tree_parent", b"count_sum_child"].as_ref(),
            b"sum_item_3",
            Element::new_sum_item(-50),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert sum_item_3");

        // Insert a direct sum item into the parent SumTree
        db.insert(
            [TEST_LEAF, b"sum_tree_parent"].as_ref(),
            b"direct_sum_item",
            Element::new_sum_item(25),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert direct_sum_item");

        // Verify CountSumTree child has correct count and sum
        let batch = StorageBatch::new();
        let transaction = db.start_transaction();

        let child_merk = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"sum_tree_parent", b"count_sum_child"]
                    .as_ref()
                    .into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open count_sum_child");

        let child_aggregate = child_merk
            .aggregate_data()
            .expect("expected to get aggregate data");
        assert_eq!(
            child_aggregate,
            AggregateData::CountAndSum(3, 250),
            "CountSumTree should have count=3, sum=250"
        );

        // Verify parent SumTree has the propagated sum
        let parent = db
            .get(
                [TEST_LEAF].as_ref(),
                b"sum_tree_parent",
                None,
                grove_version,
            )
            .unwrap()
            .expect("should get sum_tree_parent");

        assert_eq!(
            parent.sum_value_or_default(),
            275, // 100 + 200 + (-50) + 25 = 275
            "SumTree parent should have sum=275 (propagated from CountSumTree + direct item)"
        );
    }

    #[test]
    fn test_count_sum_tree_sum_propagates_to_big_sum_tree_parent() {
        // Test that sums from CountSumTree propagate correctly to BigSumTree parent
        // Uses separate CountSumTrees each with a large value to demonstrate
        // BigSumTree can hold sums exceeding i64::MAX
        //
        // Tree structure:
        //   TEST_LEAF
        //     └── big_sum_tree_parent (BigSumTree)
        //           ├── count_sum_child_1 (CountSumTree)
        //           │     └── sum_item_a = i64::MAX - 100
        //           ├── count_sum_child_2 (CountSumTree)
        //           │     └── sum_item_b = i64::MAX - 200
        //           └── direct_sum_item = 1000
        //
        // Expected: big_sum_tree sum = (i64::MAX-100) + (i64::MAX-200) + 1000
        //         which exceeds i64::MAX, demonstrating BigSumTree capability

        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        // Create parent BigSumTree
        db.insert(
            [TEST_LEAF].as_ref(),
            b"big_sum_tree_parent",
            Element::empty_big_sum_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert big_sum_tree_parent");

        // Create first child CountSumTree
        db.insert(
            [TEST_LEAF, b"big_sum_tree_parent"].as_ref(),
            b"count_sum_child_1",
            Element::new_count_sum_tree(None),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert count_sum_child_1");

        // Create second child CountSumTree
        db.insert(
            [TEST_LEAF, b"big_sum_tree_parent"].as_ref(),
            b"count_sum_child_2",
            Element::new_count_sum_tree(None),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert count_sum_child_2");

        // Insert sum item into first CountSumTree (large value)
        db.insert(
            [TEST_LEAF, b"big_sum_tree_parent", b"count_sum_child_1"].as_ref(),
            b"sum_item_a",
            Element::new_sum_item(SumValue::MAX - 100),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert sum_item_a");

        // Insert sum item into second CountSumTree (large value)
        db.insert(
            [TEST_LEAF, b"big_sum_tree_parent", b"count_sum_child_2"].as_ref(),
            b"sum_item_b",
            Element::new_sum_item(SumValue::MAX - 200),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert sum_item_b");

        // Insert a direct sum item into the parent BigSumTree
        db.insert(
            [TEST_LEAF, b"big_sum_tree_parent"].as_ref(),
            b"direct_sum_item",
            Element::new_sum_item(1000),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert direct_sum_item");

        // Verify first CountSumTree child
        let batch = StorageBatch::new();
        let transaction = db.start_transaction();

        let child1_merk = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"big_sum_tree_parent", b"count_sum_child_1"]
                    .as_ref()
                    .into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open count_sum_child_1");

        let child1_aggregate = child1_merk
            .aggregate_data()
            .expect("expected to get aggregate data");
        assert_eq!(
            child1_aggregate,
            AggregateData::CountAndSum(1, SumValue::MAX - 100),
            "CountSumTree child_1 should have count=1, sum=MAX-100"
        );

        // Verify second CountSumTree child
        let child2_merk = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"big_sum_tree_parent", b"count_sum_child_2"]
                    .as_ref()
                    .into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open count_sum_child_2");

        let child2_aggregate = child2_merk
            .aggregate_data()
            .expect("expected to get aggregate data");
        assert_eq!(
            child2_aggregate,
            AggregateData::CountAndSum(1, SumValue::MAX - 200),
            "CountSumTree child_2 should have count=1, sum=MAX-200"
        );

        // Verify parent BigSumTree has the propagated sum
        let parent = db
            .get(
                [TEST_LEAF].as_ref(),
                b"big_sum_tree_parent",
                None,
                grove_version,
            )
            .unwrap()
            .expect("should get big_sum_tree_parent");

        // Total exceeds i64::MAX, demonstrating BigSumTree capability
        let expected_total: i128 =
            (SumValue::MAX - 100) as i128 + (SumValue::MAX - 200) as i128 + 1000i128;

        assert_eq!(
            parent.big_sum_value_or_default(),
            expected_total,
            "BigSumTree parent should have correct propagated sum from CountSumTrees"
        );

        // Verify the sum actually exceeds i64::MAX
        assert!(
            expected_total > i64::MAX as i128,
            "Expected total should exceed i64::MAX to demonstrate BigSumTree"
        );
    }
}
