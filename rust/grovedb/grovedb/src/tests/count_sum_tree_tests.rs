//! Count sum tree tests

#[cfg(test)]
mod count_sum_tree_tests {
    use grovedb_merk::{
        element::costs::ElementCostExtensions,
        tree::{kv::ValueDefinedCostType, AggregateData},
        TreeFeatureType,
    };
    use grovedb_storage::StorageBatch;
    use grovedb_version::version::GroveVersion;

    use crate::{
        batch::QualifiedGroveDbOp,
        tests::{make_test_grovedb, TEST_LEAF},
        Element,
    };

    #[test]
    fn test_count_sum_tree_behaves_like_regular_tree() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        // Insert a CountSumTree
        db.insert(
            [TEST_LEAF].as_ref(),
            b"count_sum_key",
            Element::new_count_sum_tree(None),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert CountSumTree");

        // Fetch the CountSumTree
        let count_sum_tree = db
            .get([TEST_LEAF].as_ref(), b"count_sum_key", None, grove_version)
            .unwrap()
            .expect("should get CountSumTree");
        assert!(matches!(count_sum_tree, Element::CountSumTree(..)));

        // Insert items into the CountSumTree
        db.insert(
            [TEST_LEAF, b"count_sum_key"].as_ref(),
            b"item1",
            Element::new_item(vec![1]),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item1");

        db.insert(
            [TEST_LEAF, b"count_sum_key"].as_ref(),
            b"item2",
            Element::new_sum_item(3),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item2");

        db.insert(
            [TEST_LEAF, b"count_sum_key"].as_ref(),
            b"item3",
            Element::new_sum_item(5),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item3");

        // Test proper item retrieval
        let item1 = db
            .get(
                [TEST_LEAF, b"count_sum_key"].as_ref(),
                b"item1",
                None,
                grove_version,
            )
            .unwrap()
            .expect("should get item1");
        assert_eq!(item1, Element::new_item(vec![1]));

        let item2 = db
            .get(
                [TEST_LEAF, b"count_sum_key"].as_ref(),
                b"item2",
                None,
                grove_version,
            )
            .unwrap()
            .expect("should get item2");
        assert_eq!(item2, Element::new_sum_item(3));

        let item3 = db
            .get(
                [TEST_LEAF, b"count_sum_key"].as_ref(),
                b"item3",
                None,
                grove_version,
            )
            .unwrap()
            .expect("should get item3");
        assert_eq!(item3, Element::new_sum_item(5));

        // Test aggregate data (count and sum)
        let batch = StorageBatch::new();
        let transaction = db.start_transaction();

        let merk = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"count_sum_key"].as_ref().into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open CountSumTree");

        let aggregate_data = merk
            .aggregate_data()
            .expect("expected to get aggregate data");

        // Assuming AggregateData::CountAndSum is implemented
        assert_eq!(aggregate_data, AggregateData::CountAndSum(3, 8)); // 3 items: 1, 3, 5
    }

    #[test]
    fn test_count_sum_tree_item_behaves_like_regular_item() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        // Insert a CountSumTree with flags
        db.insert(
            [TEST_LEAF].as_ref(),
            b"count_sum_key2",
            Element::new_count_sum_tree_with_flags(None, None),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert CountSumTree with flags");

        // Insert count and sum items
        db.insert(
            [TEST_LEAF, b"count_sum_key2"].as_ref(),
            b"count_item",
            Element::new_item(vec![2]),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert count_item");

        db.insert(
            [TEST_LEAF, b"count_sum_key2"].as_ref(),
            b"sum_item",
            Element::new_sum_item(4),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert sum_item");

        // Test aggregate data
        let batch = StorageBatch::new();
        let transaction = db.start_transaction();

        let merk = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"count_sum_key2"].as_ref().into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open CountSumTree with flags");

        let aggregate_data = merk
            .aggregate_data()
            .expect("expected to get aggregate data");

        assert_eq!(aggregate_data, AggregateData::CountAndSum(2, 4));
    }

    #[test]
    fn test_homogenous_node_type_in_count_sum_trees_and_regular_trees() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        // Insert a CountSumTree with initial sum and count values
        db.insert(
            [TEST_LEAF].as_ref(),
            b"count_sum_key3",
            Element::new_count_sum_tree_with_flags_and_sum_and_count_value(None, 0, 0, None),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert CountSumTree with sum and count values");

        // Add count and sum items
        db.insert(
            [TEST_LEAF, b"count_sum_key3"].as_ref(),
            b"item1",
            Element::new_item(vec![10]),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item1");

        db.insert(
            [TEST_LEAF, b"count_sum_key3"].as_ref(),
            b"item2",
            Element::new_sum_item(20),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item2");

        // Add regular items
        db.insert(
            [TEST_LEAF, b"count_sum_key3"].as_ref(),
            b"item3",
            Element::new_item(vec![30]),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item3");

        db.insert(
            [TEST_LEAF, b"count_sum_key3"].as_ref(),
            b"item4",
            Element::new_item(vec![40]),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item4");

        // Open merk and check all elements in it
        let batch = StorageBatch::new();
        let transaction = db.start_transaction();

        let merk = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"count_sum_key3"].as_ref().into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open CountSumTree");

        // Verify feature types
        let feature_type_item1 = merk
            .get_feature_type(
                b"item1",
                true,
                None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                grove_version,
            )
            .unwrap()
            .expect("node should exist")
            .expect("expected feature type");
        assert_eq!(
            feature_type_item1,
            TreeFeatureType::CountedSummedMerkNode(1, 0)
        );

        let feature_type_item2 = merk
            .get_feature_type(
                b"item2",
                true,
                None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                grove_version,
            )
            .unwrap()
            .expect("node should exist")
            .expect("expected feature type");
        assert_eq!(
            feature_type_item2,
            TreeFeatureType::CountedSummedMerkNode(1, 20)
        );

        let feature_type_item3 = merk
            .get_feature_type(
                b"item3",
                true,
                Some(&Element::value_defined_cost_for_serialized_value),
                grove_version,
            )
            .unwrap()
            .expect("node should exist")
            .expect("expected feature type");
        assert_eq!(
            feature_type_item3,
            TreeFeatureType::CountedSummedMerkNode(1, 0)
        );

        let feature_type_item4 = merk
            .get_feature_type(
                b"item4",
                true,
                Some(&Element::value_defined_cost_for_serialized_value),
                grove_version,
            )
            .unwrap()
            .expect("node should exist")
            .expect("expected feature type");
        assert_eq!(
            feature_type_item4,
            TreeFeatureType::CountedSummedMerkNode(1, 0)
        );

        // Verify aggregate data
        let aggregate_data = merk
            .aggregate_data()
            .expect("expected to get aggregate data");
        assert_eq!(aggregate_data, AggregateData::CountAndSum(4, 20)); // 2 count, 10 + 20 sum
    }

    #[test]
    fn test_count_sum_tree_feature() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        // Insert a regular tree
        db.insert(
            [TEST_LEAF].as_ref(),
            b"regular_key",
            Element::new_tree(None),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert regular tree");

        let batch = StorageBatch::new();
        let transaction = db.start_transaction();

        // Aggregate data should be None for regular tree
        let merk = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"regular_key"].as_ref().into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open regular tree");
        assert_eq!(
            merk.aggregate_data()
                .expect("expected to get aggregate data"),
            AggregateData::NoAggregateData
        );

        // Insert a CountSumTree
        db.insert(
            [TEST_LEAF].as_ref(),
            b"count_sum_key4",
            Element::new_count_sum_tree_with_flags_and_sum_and_count_value(None, 0, 0, None),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert CountSumTree");

        let count_sum_tree = db
            .get([TEST_LEAF].as_ref(), b"count_sum_key4", None, grove_version)
            .unwrap()
            .expect("should retrieve CountSumTree");
        assert!(matches!(count_sum_tree, Element::CountSumTree(..)));
        // Note: Directly accessing count_sum_value_or_default is not shown in original
        // code. Assuming you have a method like this to extract count and sum
        // from the Element. If not, rely on aggregate_data as below.

        // Add count and sum items
        db.insert(
            [TEST_LEAF, b"count_sum_key4"].as_ref(),
            b"count_item1",
            Element::new_item(vec![1]),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert count_item1");

        db.insert(
            [TEST_LEAF, b"count_sum_key4"].as_ref(),
            b"sum_item1",
            Element::new_sum_item(5),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert sum_item1");

        // Verify aggregate data
        let batch = StorageBatch::new();
        let merk = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"count_sum_key4"].as_ref().into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open CountSumTree");

        let aggregate_data = merk
            .aggregate_data()
            .expect("expected to get aggregate data");
        assert_eq!(aggregate_data, AggregateData::CountAndSum(2, 5));
    }

    #[test]
    fn test_count_sum_tree_with_batches() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        // Prepare a batch of operations
        let ops = vec![
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![TEST_LEAF.to_vec()],
                b"count_sum_key6".to_vec(),
                Element::new_count_sum_tree(None),
            ),
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![TEST_LEAF.to_vec(), b"count_sum_key6".to_vec()],
                b"a".to_vec(),
                Element::new_item(vec![10]),
            ),
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![TEST_LEAF.to_vec(), b"count_sum_key6".to_vec()],
                b"b".to_vec(),
                Element::new_sum_item(20),
            ),
        ];

        // Apply the batch
        db.apply_batch(ops, None, None, grove_version)
            .unwrap()
            .expect("should apply batch");

        // Open the CountSumTree and verify aggregate data
        let batch = StorageBatch::new();
        let transaction = db.start_transaction();

        let merk = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"count_sum_key6"].as_ref().into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open CountSumTree");

        let aggregate_data = merk
            .aggregate_data()
            .expect("expected to get aggregate data");
        assert_eq!(aggregate_data, AggregateData::CountAndSum(2, 20));
    }

    #[test]
    fn test_count_sum_tree_propagation() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        // Insert a parent CountSumTree
        db.insert(
            [TEST_LEAF].as_ref(),
            b"parent_count_sum",
            Element::new_count_sum_tree(None),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert parent CountSumTree");

        // Insert a child CountSumTree within the parent
        db.insert(
            [TEST_LEAF, b"parent_count_sum"].as_ref(),
            b"child_count_sum",
            Element::new_count_sum_tree(None),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert child CountSumTree");

        // Insert items into the child CountSumTree
        db.insert(
            [TEST_LEAF, b"parent_count_sum", b"child_count_sum"].as_ref(),
            b"item1",
            Element::new_item(vec![5]),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item1 into child");

        db.insert(
            [TEST_LEAF, b"parent_count_sum", b"child_count_sum"].as_ref(),
            b"item2",
            Element::new_sum_item(15),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item2 into child");

        // Verify aggregate data of child
        let batch = StorageBatch::new();
        let transaction = db.start_transaction();

        let child_merk = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"parent_count_sum", b"child_count_sum"]
                    .as_ref()
                    .into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open child CountSumTree");

        let child_aggregate = child_merk
            .aggregate_data()
            .expect("expected to get aggregate data");
        assert_eq!(child_aggregate, AggregateData::CountAndSum(2, 15));

        // Verify aggregate data of parent
        let parent_batch = StorageBatch::new();
        let parent_merk = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"parent_count_sum"].as_ref().into(),
                &transaction,
                Some(&parent_batch),
                grove_version,
            )
            .unwrap()
            .expect("should open parent CountSumTree");

        let parent_aggregate = parent_merk
            .aggregate_data()
            .expect("expected to get aggregate data");
        assert_eq!(parent_aggregate, AggregateData::CountAndSum(2, 15));
    }
}
