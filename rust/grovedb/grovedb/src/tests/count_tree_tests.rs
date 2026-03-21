//! Count tree tests

#[cfg(test)]
mod tests {
    use assert_matches::assert_matches;
    use grovedb_merk::{
        element::costs::ElementCostExtensions,
        proofs::Query,
        tree::{kv::ValueDefinedCostType, AggregateData},
        TreeFeatureType::{BasicMerkNode, CountedMerkNode},
    };
    use grovedb_storage::StorageBatch;
    use grovedb_version::version::GroveVersion;

    use crate::{
        batch::QualifiedGroveDbOp,
        reference_path::ReferencePathType,
        tests::{make_test_grovedb, TEST_LEAF},
        Element, GroveDb, PathQuery,
    };

    #[test]
    fn test_count_tree_behaves_like_regular_tree() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);
        db.insert(
            [TEST_LEAF].as_ref(),
            b"key",
            Element::empty_count_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert tree");

        // Can fetch count tree
        let count_tree = db
            .get([TEST_LEAF].as_ref(), b"key", None, grove_version)
            .unwrap()
            .expect("should get count tree");
        assert!(matches!(count_tree, Element::CountTree(..)));

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
    }

    #[test]
    fn test_count_tree_without_aggregation_query_result() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);
        db.insert(
            [TEST_LEAF].as_ref(),
            b"key",
            Element::empty_count_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert tree");

        // Can fetch count tree
        let count_tree = db
            .get([TEST_LEAF].as_ref(), b"key", None, grove_version)
            .unwrap()
            .expect("should get count tree");
        assert!(matches!(count_tree, Element::CountTree(..)));

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
        query.insert_key("key".as_bytes().to_vec());

        let sub_query = Query::new_range_full();

        query.set_subquery(sub_query);

        let path_query = PathQuery::new_unsized(vec![TEST_LEAF.to_vec()], query);
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
        assert_eq!(result_set.len(), 3);
        assert_eq!(
            Element::deserialize(&result_set[0].value, grove_version)
                .expect("should deserialize element"),
            Element::new_item(vec![1])
        );
        assert_eq!(
            Element::deserialize(&result_set[1].value, grove_version)
                .expect("should deserialize element"),
            Element::new_item(vec![3])
        );
        assert_eq!(
            Element::deserialize(&result_set[2].value, grove_version)
                .expect("should deserialize element"),
            Element::empty_tree()
        );
    }

    #[test]
    fn test_count_tree_aggregation_query_result() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);
        db.insert(
            [TEST_LEAF].as_ref(),
            b"key",
            Element::empty_count_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert tree");

        // Can fetch count tree
        let count_tree = db
            .get([TEST_LEAF].as_ref(), b"key", None, grove_version)
            .unwrap()
            .expect("should get count tree");
        assert!(matches!(count_tree, Element::CountTree(..)));

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
        query.insert_key("key".as_bytes().to_vec());

        let sub_query = Query::new_range_full();

        query.set_subquery(sub_query);

        query.add_parent_tree_on_subquery = true;

        let path_query = PathQuery::new_unsized(vec![TEST_LEAF.to_vec()], query);
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
        assert_eq!(result_set.len(), 4);
        assert_eq!(
            Element::deserialize(&result_set[0].value, grove_version)
                .expect("should deserialize element"),
            Element::CountTree(Some("innerkey2".as_bytes().to_vec()), 3, None)
        );
        assert_eq!(
            Element::deserialize(&result_set[1].value, grove_version)
                .expect("should deserialize element"),
            Element::new_item(vec![1])
        );
        assert_eq!(
            Element::deserialize(&result_set[2].value, grove_version)
                .expect("should deserialize element"),
            Element::new_item(vec![3])
        );
        assert_eq!(
            Element::deserialize(&result_set[3].value, grove_version)
                .expect("should deserialize element"),
            Element::empty_tree()
        );
    }

    #[test]
    fn test_homogenous_node_type_in_count_trees_and_regular_trees() {
        let grove_version = GroveVersion::latest();
        // All elements in a count tree must have a count feature type
        let db = make_test_grovedb(grove_version);
        db.insert(
            [TEST_LEAF].as_ref(),
            b"key",
            Element::empty_count_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert tree");
        // Add count items
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
        let feature_type_node_1 = merk
            .get_feature_type(
                b"item1",
                true,
                None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                grove_version,
            )
            .unwrap()
            .expect("node should exist")
            .expect("expected feature type");
        let feature_type_node_2 = merk
            .get_feature_type(
                b"item2",
                true,
                None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                grove_version,
            )
            .unwrap()
            .expect("node should exist")
            .expect("expected feature type");
        let feature_type_node_3 = merk
            .get_feature_type(
                b"item3",
                true,
                None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                grove_version,
            )
            .unwrap()
            .expect("node should exist")
            .expect("expected feature type");
        let feature_type_node_4 = merk
            .get_feature_type(
                b"item4",
                true,
                None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                grove_version,
            )
            .unwrap()
            .expect("node should exist")
            .expect("expected feature type");

        assert_eq!(feature_type_node_1, CountedMerkNode(1));
        assert_eq!(feature_type_node_2, CountedMerkNode(1));
        assert_eq!(feature_type_node_3, CountedMerkNode(1));
        assert_eq!(feature_type_node_4, CountedMerkNode(1));

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
            merk.aggregate_data().expect("expected to get count"),
            AggregateData::NoAggregateData
        );
    }

    #[test]
    fn test_count_tree_feature() {
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

        // Sum should be non for non count tree
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
            merk.aggregate_data().expect("expected to get count"),
            AggregateData::NoAggregateData
        );

        // Add count tree
        db.insert(
            [TEST_LEAF].as_ref(),
            b"key2",
            Element::empty_count_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert count tree");
        let count_tree = db
            .get([TEST_LEAF].as_ref(), b"key2", None, grove_version)
            .unwrap()
            .expect("should retrieve tree");
        assert_eq!(count_tree.count_value_or_default(), 0);

        // Add count items to the count tree
        db.insert(
            [TEST_LEAF, b"key2"].as_ref(),
            b"item1",
            Element::new_item(vec![30]),
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
            merk.aggregate_data().expect("expected to get count"),
            AggregateData::Count(1)
        );

        // Add more count items
        db.insert(
            [TEST_LEAF, b"key2"].as_ref(),
            b"item2",
            Element::new_item(vec![3]),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item");
        db.insert(
            [TEST_LEAF, b"key2"].as_ref(),
            b"item3",
            Element::new_item(vec![3]),
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
            merk.aggregate_data().expect("expected to get count"),
            AggregateData::Count(3)
        );

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
            merk.aggregate_data().expect("expected to get count"),
            AggregateData::Count(4)
        );

        db.insert(
            [TEST_LEAF, b"key2"].as_ref(),
            b"item2",
            Element::new_item(vec![10]),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item");
        db.insert(
            [TEST_LEAF, b"key2"].as_ref(),
            b"item3",
            Element::new_item(vec![3]),
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
            merk.aggregate_data().expect("expected to get count"),
            AggregateData::Count(4)
        );

        db.delete(
            [TEST_LEAF, b"key2"].as_ref(),
            b"item4",
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("expected to delete");
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
            merk.aggregate_data().expect("expected to get count"),
            AggregateData::Count(3)
        );
    }

    #[test]
    fn test_count_tree_propagation() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);
        // Tree
        //   count_key: CountTree
        //        /        \
        // countitem3    tree2: CountTree
        //
        //   tree2 : CountTree
        //    /
        // item1   item2   item3 ref1
        db.insert(
            [TEST_LEAF].as_ref(),
            b"count_key",
            Element::empty_count_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert tree");
        db.insert(
            [TEST_LEAF, b"count_key"].as_ref(),
            b"tree2",
            Element::empty_count_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert tree");
        db.insert(
            [TEST_LEAF, b"count_key"].as_ref(),
            b"countitem3",
            Element::new_item(vec![3]),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert tree");
        db.insert(
            [TEST_LEAF, b"count_key", b"tree2"].as_ref(),
            b"item1",
            Element::new_item(vec![2]),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item");
        db.insert(
            [TEST_LEAF, b"count_key", b"tree2"].as_ref(),
            b"item2",
            Element::new_item(vec![5]),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item");
        db.insert(
            [TEST_LEAF, b"count_key", b"tree2"].as_ref(),
            b"item3",
            Element::new_item(vec![10]),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item");
        db.insert(
            [TEST_LEAF, b"count_key", b"tree2"].as_ref(),
            b"ref1",
            Element::new_reference(ReferencePathType::AbsolutePathReference(vec![
                TEST_LEAF.to_vec(),
                b"count_key".to_vec(),
                b"tree2".to_vec(),
                b"item1".to_vec(),
            ])),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item");

        let count_tree = db
            .get([TEST_LEAF].as_ref(), b"count_key", None, grove_version)
            .unwrap()
            .expect("should fetch tree");
        assert_eq!(count_tree.count_value_or_default(), 5);

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
        let root_tree_feature_type = test_leaf_merk
            .get_feature_type(
                b"count_key",
                true,
                Some(&Element::value_defined_cost_for_serialized_value),
                grove_version,
            )
            .unwrap()
            .expect("node should exist")
            .expect("tree feature type");

        assert_matches!(root_tree_feature_type, BasicMerkNode);

        let parent_count_tree = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"count_key"].as_ref().into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open tree");
        let count_tree_feature_type = parent_count_tree
            .get_feature_type(
                b"tree2",
                true,
                Some(&Element::value_defined_cost_for_serialized_value),
                grove_version,
            )
            .unwrap()
            .expect("node should exist")
            .expect("tree feature type");
        assert_matches!(count_tree_feature_type, CountedMerkNode(4));

        let child_count_tree = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"count_key", b"tree2"].as_ref().into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open tree");
        let count_tree_feature_type = child_count_tree
            .get_feature_type(
                b"item1",
                true,
                None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                grove_version,
            )
            .unwrap()
            .expect("node should exist")
            .expect("tree feature type");

        assert_matches!(count_tree_feature_type, CountedMerkNode(1));

        let count_tree_feature_type = child_count_tree
            .get_feature_type(
                b"item2",
                true,
                None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                grove_version,
            )
            .unwrap()
            .expect("node should exist")
            .expect("tree feature type");

        assert_matches!(count_tree_feature_type, CountedMerkNode(1));

        let count_tree_feature_type = child_count_tree
            .get_feature_type(
                b"item3",
                true,
                None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                grove_version,
            )
            .unwrap()
            .expect("node should exist")
            .expect("tree feature type");

        assert_matches!(count_tree_feature_type, CountedMerkNode(1));

        let count_tree_feature_type = child_count_tree
            .get_feature_type(
                b"ref1",
                true,
                None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                grove_version,
            )
            .unwrap()
            .expect("node should exist")
            .expect("tree feature type");

        assert_matches!(count_tree_feature_type, CountedMerkNode(1));
    }

    #[test]
    fn test_count_tree_with_batches() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);
        let ops = vec![
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![TEST_LEAF.to_vec()],
                b"key1".to_vec(),
                Element::empty_count_tree(),
            ),
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![TEST_LEAF.to_vec(), b"key1".to_vec()],
                b"a".to_vec(),
                Element::new_item(vec![214]),
            ),
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![TEST_LEAF.to_vec(), b"key1".to_vec()],
                b"b".to_vec(),
                Element::new_item(vec![10]),
            ),
        ];
        db.apply_batch(ops, None, None, grove_version)
            .unwrap()
            .expect("should apply batch");

        let batch = StorageBatch::new();
        let transaction = db.start_transaction();

        let count_tree = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"key1"].as_ref().into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open tree");

        let tree_feature_type_a = count_tree
            .get_feature_type(
                b"a",
                true,
                Some(&Element::value_defined_cost_for_serialized_value),
                grove_version,
            )
            .unwrap()
            .expect("node should exist")
            .expect("expected tree feature type");

        let tree_feature_type_b = count_tree
            .get_feature_type(
                b"a",
                true,
                Some(&Element::value_defined_cost_for_serialized_value),
                grove_version,
            )
            .unwrap()
            .expect("node should exist")
            .expect("expected tree feature type");

        assert_matches!(tree_feature_type_a, CountedMerkNode(1));
        assert_matches!(tree_feature_type_b, CountedMerkNode(1));

        // Create new batch to use existing tree
        let ops = vec![QualifiedGroveDbOp::insert_or_replace_op(
            vec![TEST_LEAF.to_vec(), b"key1".to_vec()],
            b"c".to_vec(),
            Element::new_item(vec![10]),
        )];
        db.apply_batch(ops, None, None, grove_version)
            .unwrap()
            .expect("should apply batch");

        let batch = StorageBatch::new();
        let count_tree = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"key1"].as_ref().into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open tree");
        let tree_feature_type_c = count_tree
            .get_feature_type(
                b"c",
                true,
                None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                grove_version,
            )
            .unwrap()
            .expect("node should exist")
            .expect("expected tree feature type");
        assert_matches!(tree_feature_type_c, CountedMerkNode(1));
        assert_eq!(
            count_tree.aggregate_data().expect("expected to get count"),
            AggregateData::Count(3)
        );

        // Test propagation
        // Add a new count tree with its own count items, should affect count of
        // original tree
        let ops = vec![
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![TEST_LEAF.to_vec(), b"key1".to_vec()],
                b"d".to_vec(),
                Element::empty_count_tree(),
            ),
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![TEST_LEAF.to_vec(), b"key1".to_vec(), b"d".to_vec()],
                b"first".to_vec(),
                Element::new_item(vec![2]),
            ),
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![TEST_LEAF.to_vec(), b"key1".to_vec(), b"d".to_vec()],
                b"second".to_vec(),
                Element::new_item(vec![4]),
            ),
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![TEST_LEAF.to_vec(), b"key1".to_vec()],
                b"e".to_vec(),
                Element::empty_count_tree(),
            ),
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![TEST_LEAF.to_vec(), b"key1".to_vec(), b"e".to_vec()],
                b"first".to_vec(),
                Element::new_item(vec![3]),
            ),
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![TEST_LEAF.to_vec(), b"key1".to_vec(), b"e".to_vec()],
                b"second".to_vec(),
                Element::new_item(vec![4]),
            ),
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![TEST_LEAF.to_vec(), b"key1".to_vec(), b"e".to_vec()],
                b"third".to_vec(),
                Element::empty_count_tree(),
            ),
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![
                    TEST_LEAF.to_vec(),
                    b"key1".to_vec(),
                    b"e".to_vec(),
                    b"third".to_vec(),
                ],
                b"a".to_vec(),
                Element::new_item(vec![5]),
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

        let batch = StorageBatch::new();
        let count_tree = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"key1"].as_ref().into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open tree");
        assert_eq!(
            count_tree.aggregate_data().expect("expected to get count"),
            AggregateData::Count(9)
        );
    }
}
