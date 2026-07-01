//! ProvableCountSumTree tests
//!
//! ProvableCountSumTree combines the functionality of ProvableCountTree and
//! CountSumTree:
//! - The COUNT is included in the cryptographic hash (like ProvableCountTree)
//! - The SUM is tracked but NOT included in the hash (for query purposes)
//!
//! This allows for cryptographic proofs of element count while also tracking
//! sums.

#[cfg(test)]
mod tests {
    use grovedb_merk::{
        proofs::{encoding::Decoder, tree::execute, Node, Op, Query},
        tree::{kv::ValueDefinedCostType, AggregateData},
        TreeFeatureType,
    };
    use grovedb_storage::StorageBatch;
    use grovedb_version::version::GroveVersion;

    use crate::{
        batch::QualifiedGroveDbOp,
        operations::proof::GroveDBProof,
        query::SizedQuery,
        tests::{make_test_grovedb, TEST_LEAF},
        Element, GroveDb, PathQuery,
    };

    /// Extract count from a proof Node
    fn get_node_count(node: &Node) -> Option<u64> {
        match node {
            Node::KVCount(_, _, count) => Some(*count),
            Node::KVDigestCount(_, _, count) => Some(*count),
            Node::KVValueHashFeatureType(
                _,
                _,
                _,
                TreeFeatureType::ProvableCountedMerkNode(count),
            ) => Some(*count),
            Node::KVValueHashFeatureType(
                _,
                _,
                _,
                TreeFeatureType::ProvableCountedSummedMerkNode(count, _),
            ) => Some(*count),
            _ => None,
        }
    }

    /// Walk a proof tree and collect all nodes with their counts
    /// Returns (key, count) for each node that has count data
    fn collect_tree_node_counts(tree: &grovedb_merk::proofs::tree::Tree) -> Vec<(Vec<u8>, u64)> {
        let mut results = Vec::new();

        // Get count from current node
        if let Some(count) = get_node_count(&tree.node) {
            let key = match &tree.node {
                Node::KVCount(k, ..) => k.clone(),
                Node::KVValueHashFeatureType(k, ..) => k.clone(),
                Node::KV(k, _) => k.clone(),
                Node::KVValueHash(k, ..) => k.clone(),
                Node::KVDigest(k, _) => k.clone(),
                Node::KVDigestCount(k, ..) => k.clone(),
                Node::KVRefValueHash(k, ..) => k.clone(),
                Node::KVRefValueHashCount(k, ..) => k.clone(),
                Node::KVHashCount(..) => vec![],
                Node::Hash(_) | Node::KVHash(_) => vec![],
            };
            results.push((key, count));
        }

        // Recursively collect from children
        if let Some(child) = &tree.left {
            results.extend(collect_tree_node_counts(&child.tree));
        }
        if let Some(child) = &tree.right {
            results.extend(collect_tree_node_counts(&child.tree));
        }

        results
    }

    /// Execute a merk proof and build the tree structure
    /// Use collapse=false to preserve the full tree structure for inspection
    fn execute_merk_proof(
        merk_proof_bytes: &[u8],
    ) -> Result<grovedb_merk::proofs::tree::Tree, grovedb_merk::error::Error> {
        let decoder = Decoder::new(merk_proof_bytes);
        // collapse=false preserves the full tree structure
        execute(decoder, false, |_node| Ok(()))
            .unwrap()
            .map_err(|e| e)
    }

    #[test]
    fn test_provable_count_sum_tree_behaves_like_regular_tree() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        // Insert a ProvableCountSumTree
        db.insert(
            [TEST_LEAF].as_ref(),
            b"provable_count_sum_key",
            Element::new_provable_count_sum_tree(None),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert ProvableCountSumTree");

        // Fetch the ProvableCountSumTree
        let provable_count_sum_tree = db
            .get(
                [TEST_LEAF].as_ref(),
                b"provable_count_sum_key",
                None,
                grove_version,
            )
            .unwrap()
            .expect("should get ProvableCountSumTree");
        assert!(matches!(
            provable_count_sum_tree,
            Element::ProvableCountSumTree(..)
        ));

        // Insert items into the ProvableCountSumTree
        db.insert(
            [TEST_LEAF, b"provable_count_sum_key"].as_ref(),
            b"item1",
            Element::new_item(vec![1]),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item1");

        db.insert(
            [TEST_LEAF, b"provable_count_sum_key"].as_ref(),
            b"item2",
            Element::new_sum_item(3),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item2");

        db.insert(
            [TEST_LEAF, b"provable_count_sum_key"].as_ref(),
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
                [TEST_LEAF, b"provable_count_sum_key"].as_ref(),
                b"item1",
                None,
                grove_version,
            )
            .unwrap()
            .expect("should get item1");
        assert_eq!(item1, Element::new_item(vec![1]));

        let item2 = db
            .get(
                [TEST_LEAF, b"provable_count_sum_key"].as_ref(),
                b"item2",
                None,
                grove_version,
            )
            .unwrap()
            .expect("should get item2");
        assert_eq!(item2, Element::new_sum_item(3));

        let item3 = db
            .get(
                [TEST_LEAF, b"provable_count_sum_key"].as_ref(),
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
                [TEST_LEAF, b"provable_count_sum_key"].as_ref().into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open ProvableCountSumTree");

        let aggregate_data = merk
            .aggregate_data()
            .expect("expected to get aggregate data");

        // Should have ProvableCountAndSum aggregate data: 3 items with sum of 8 (3 + 5)
        assert_eq!(aggregate_data, AggregateData::ProvableCountAndSum(3, 8));
    }

    #[test]
    fn test_provable_count_sum_tree_feature_types() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        // Insert a ProvableCountSumTree
        db.insert(
            [TEST_LEAF].as_ref(),
            b"feature_test",
            Element::new_provable_count_sum_tree(None),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert ProvableCountSumTree");

        // Insert items
        db.insert(
            [TEST_LEAF, b"feature_test"].as_ref(),
            b"regular_item",
            Element::new_item(vec![10]),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert regular_item");

        db.insert(
            [TEST_LEAF, b"feature_test"].as_ref(),
            b"sum_item",
            Element::new_sum_item(20),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert sum_item");

        // Open merk and check feature types
        let batch = StorageBatch::new();
        let transaction = db.start_transaction();

        let merk = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"feature_test"].as_ref().into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open ProvableCountSumTree");

        // Verify feature types - should be ProvableCountedSummedMerkNode
        let feature_type_regular = merk
            .get_feature_type(
                b"regular_item",
                true,
                None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                grove_version,
            )
            .unwrap()
            .expect("node should exist")
            .expect("expected feature type");
        assert_eq!(
            feature_type_regular,
            TreeFeatureType::ProvableCountedSummedMerkNode(1, 0)
        );

        let feature_type_sum = merk
            .get_feature_type(
                b"sum_item",
                true,
                None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                grove_version,
            )
            .unwrap()
            .expect("node should exist")
            .expect("expected feature type");
        assert_eq!(
            feature_type_sum,
            TreeFeatureType::ProvableCountedSummedMerkNode(1, 20)
        );
    }

    #[test]
    fn test_provable_count_sum_tree_proof_generation_and_verification() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        // Create a ProvableCountSumTree - following the same pattern as
        // ProvableCountTree test
        db.insert(
            &[] as &[&[u8]],
            b"test_tree",
            Element::empty_provable_count_sum_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert tree");

        // Insert an item
        db.insert(
            &[b"test_tree"],
            b"key1",
            Element::new_item(b"value1".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item");

        // Query for the item
        let mut query = Query::new();
        query.insert_key(b"key1".to_vec());
        let path_query = PathQuery::new_unsized(vec![b"test_tree".to_vec()], query);

        // Generate proof
        let proof = db
            .prove_query(&path_query, None, grove_version)
            .unwrap()
            .expect("should generate proof");

        // Verify proof
        let (root_hash, proved_values) =
            GroveDb::verify_query_raw(&proof, &path_query, grove_version)
                .expect("should verify proof");

        // Check root hash matches
        let actual_root_hash = db
            .root_hash(None, grove_version)
            .unwrap()
            .expect("should get root hash");

        assert_eq!(root_hash, actual_root_hash, "Root hash should match");
        assert_eq!(proved_values.len(), 1, "Should have 1 proved value");
        assert_eq!(proved_values[0].key, b"key1");
    }

    #[test]
    fn test_provable_count_sum_tree_with_batches() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        // Prepare a batch of operations
        let ops = vec![
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![TEST_LEAF.to_vec()],
                b"batch_provable_count_sum".to_vec(),
                Element::new_provable_count_sum_tree(None),
            ),
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![TEST_LEAF.to_vec(), b"batch_provable_count_sum".to_vec()],
                b"a".to_vec(),
                Element::new_item(vec![10]),
            ),
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![TEST_LEAF.to_vec(), b"batch_provable_count_sum".to_vec()],
                b"b".to_vec(),
                Element::new_sum_item(20),
            ),
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![TEST_LEAF.to_vec(), b"batch_provable_count_sum".to_vec()],
                b"c".to_vec(),
                Element::new_sum_item(30),
            ),
        ];

        // Apply the batch
        db.apply_batch(ops, None, None, grove_version)
            .unwrap()
            .expect("should apply batch");

        // Open the ProvableCountSumTree and verify aggregate data
        let batch = StorageBatch::new();
        let transaction = db.start_transaction();

        let merk = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"batch_provable_count_sum"].as_ref().into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open ProvableCountSumTree");

        let aggregate_data = merk
            .aggregate_data()
            .expect("expected to get aggregate data");
        // count=3, sum=20+30=50
        assert_eq!(aggregate_data, AggregateData::ProvableCountAndSum(3, 50));
    }

    #[test]
    fn test_provable_count_sum_tree_propagation() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        // Insert a parent ProvableCountSumTree
        db.insert(
            [TEST_LEAF].as_ref(),
            b"parent_provable",
            Element::new_provable_count_sum_tree(None),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert parent ProvableCountSumTree");

        // Insert a child ProvableCountSumTree within the parent
        db.insert(
            [TEST_LEAF, b"parent_provable"].as_ref(),
            b"child_provable",
            Element::new_provable_count_sum_tree(None),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert child ProvableCountSumTree");

        // Insert items into the child ProvableCountSumTree
        db.insert(
            [TEST_LEAF, b"parent_provable", b"child_provable"].as_ref(),
            b"item1",
            Element::new_item(vec![5]),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item1 into child");

        db.insert(
            [TEST_LEAF, b"parent_provable", b"child_provable"].as_ref(),
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
                [TEST_LEAF, b"parent_provable", b"child_provable"]
                    .as_ref()
                    .into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open child ProvableCountSumTree");

        let child_aggregate = child_merk
            .aggregate_data()
            .expect("expected to get aggregate data");
        assert_eq!(child_aggregate, AggregateData::ProvableCountAndSum(2, 15));

        // Verify aggregate data of parent (should include child tree's contribution)
        let parent_batch = StorageBatch::new();
        let parent_merk = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"parent_provable"].as_ref().into(),
                &transaction,
                Some(&parent_batch),
                grove_version,
            )
            .unwrap()
            .expect("should open parent ProvableCountSumTree");

        let parent_aggregate = parent_merk
            .aggregate_data()
            .expect("expected to get aggregate data");
        // Parent propagates the child tree's count and sum values
        assert_eq!(parent_aggregate, AggregateData::ProvableCountAndSum(2, 15));
    }

    #[test]
    fn test_provable_count_sum_tree_constructors() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        // Test empty_provable_count_sum_tree
        db.insert(
            [TEST_LEAF].as_ref(),
            b"empty_tree",
            Element::empty_provable_count_sum_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert empty ProvableCountSumTree");

        // Test empty_provable_count_sum_tree_with_flags
        db.insert(
            [TEST_LEAF].as_ref(),
            b"flagged_tree",
            Element::empty_provable_count_sum_tree_with_flags(Some(vec![1, 2, 3])),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert flagged ProvableCountSumTree");

        // Test new_provable_count_sum_tree
        db.insert(
            [TEST_LEAF].as_ref(),
            b"new_tree",
            Element::new_provable_count_sum_tree(None),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert new ProvableCountSumTree");

        // Test new_provable_count_sum_tree_with_flags
        db.insert(
            [TEST_LEAF].as_ref(),
            b"new_flagged_tree",
            Element::new_provable_count_sum_tree_with_flags(None, Some(vec![4, 5, 6])),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert new flagged ProvableCountSumTree");

        // Verify all trees were inserted correctly
        let empty_tree = db
            .get([TEST_LEAF].as_ref(), b"empty_tree", None, grove_version)
            .unwrap()
            .expect("should get empty_tree");
        assert!(matches!(empty_tree, Element::ProvableCountSumTree(..)));

        let flagged_tree = db
            .get([TEST_LEAF].as_ref(), b"flagged_tree", None, grove_version)
            .unwrap()
            .expect("should get flagged_tree");
        assert!(matches!(flagged_tree, Element::ProvableCountSumTree(..)));
        assert_eq!(flagged_tree.get_flags(), &Some(vec![1, 2, 3]));
    }

    #[test]
    fn test_provable_count_sum_tree_vs_count_sum_tree_aggregate_difference() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        // Insert a regular CountSumTree
        db.insert(
            [TEST_LEAF].as_ref(),
            b"regular_count_sum",
            Element::new_count_sum_tree(None),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert CountSumTree");

        // Insert a ProvableCountSumTree
        db.insert(
            [TEST_LEAF].as_ref(),
            b"provable_count_sum",
            Element::new_provable_count_sum_tree(None),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert ProvableCountSumTree");

        // Insert same items into both
        for tree_key in [
            b"regular_count_sum".as_slice(),
            b"provable_count_sum".as_slice(),
        ] {
            db.insert(
                [TEST_LEAF, tree_key].as_ref(),
                b"item1",
                Element::new_sum_item(10),
                None,
                None,
                grove_version,
            )
            .unwrap()
            .expect("should insert item1");

            db.insert(
                [TEST_LEAF, tree_key].as_ref(),
                b"item2",
                Element::new_sum_item(20),
                None,
                None,
                grove_version,
            )
            .unwrap()
            .expect("should insert item2");
        }

        let batch = StorageBatch::new();
        let transaction = db.start_transaction();

        // Check regular CountSumTree aggregate
        let regular_merk = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"regular_count_sum"].as_ref().into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open CountSumTree");

        let regular_aggregate = regular_merk
            .aggregate_data()
            .expect("expected to get aggregate data");
        assert_eq!(regular_aggregate, AggregateData::CountAndSum(2, 30));

        // Check ProvableCountSumTree aggregate
        let provable_batch = StorageBatch::new();
        let provable_merk = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"provable_count_sum"].as_ref().into(),
                &transaction,
                Some(&provable_batch),
                grove_version,
            )
            .unwrap()
            .expect("should open ProvableCountSumTree");

        let provable_aggregate = provable_merk
            .aggregate_data()
            .expect("expected to get aggregate data");
        // Both should have same count and sum values, but different types
        assert_eq!(
            provable_aggregate,
            AggregateData::ProvableCountAndSum(2, 30)
        );
    }

    #[test]
    fn test_provable_count_sum_tree_helper_methods() {
        // Test is_provable_count_sum_tree helper
        let tree = Element::new_provable_count_sum_tree(None);
        assert!(tree.is_any_tree());

        // Test count_sum_value_or_default
        let tree_with_values =
            Element::new_provable_count_sum_tree_with_flags_and_sum_and_count_value(
                None, 5, 100, None,
            );
        assert_eq!(tree_with_values.count_sum_value_or_default(), (5, 100));
    }

    // ==================== EDGE CASE TESTS ====================

    #[test]
    fn test_provable_count_sum_tree_empty_tree() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        // Insert an empty ProvableCountSumTree (no items inside)
        db.insert(
            [TEST_LEAF].as_ref(),
            b"empty_provable",
            Element::empty_provable_count_sum_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert empty ProvableCountSumTree");

        // Verify we can retrieve it
        let retrieved = db
            .get([TEST_LEAF].as_ref(), b"empty_provable", None, grove_version)
            .unwrap()
            .expect("should get empty tree");
        assert!(matches!(
            retrieved,
            Element::ProvableCountSumTree(None, 0, 0, None)
        ));

        // Verify aggregate data for empty tree
        let batch = StorageBatch::new();
        let transaction = db.start_transaction();

        let merk = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"empty_provable"].as_ref().into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open empty ProvableCountSumTree");

        let aggregate_data = merk
            .aggregate_data()
            .expect("expected to get aggregate data");
        // Empty tree (no root node) returns NoAggregateData
        assert_eq!(aggregate_data, AggregateData::NoAggregateData);
    }

    #[test]
    fn test_provable_count_sum_tree_empty_tree_proof() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        // Insert an empty ProvableCountSumTree
        db.insert(
            &[] as &[&[u8]],
            b"empty_tree",
            Element::empty_provable_count_sum_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert empty tree");

        // Query for a non-existent key (absence proof)
        let mut query = Query::new();
        query.insert_key(b"nonexistent".to_vec());
        let path_query = PathQuery::new_unsized(vec![b"empty_tree".to_vec()], query);

        // Generate proof for absence
        let proof = db
            .prove_query(&path_query, None, grove_version)
            .unwrap()
            .expect("should generate absence proof");

        // Verify proof
        let (root_hash, proved_values) =
            GroveDb::verify_query_raw(&proof, &path_query, grove_version)
                .expect("should verify absence proof");

        // Check root hash matches
        let actual_root_hash = db
            .root_hash(None, grove_version)
            .unwrap()
            .expect("should get root hash");

        assert_eq!(root_hash, actual_root_hash, "Root hash should match");
        assert_eq!(
            proved_values.len(),
            0,
            "Should have no proved values for absence"
        );
    }

    #[test]
    fn test_provable_count_sum_tree_with_negative_sums() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        // Insert a ProvableCountSumTree
        db.insert(
            [TEST_LEAF].as_ref(),
            b"negative_sum_tree",
            Element::new_provable_count_sum_tree(None),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert tree");

        // Insert items with negative sum values
        db.insert(
            [TEST_LEAF, b"negative_sum_tree"].as_ref(),
            b"positive",
            Element::new_sum_item(100),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert positive sum item");

        db.insert(
            [TEST_LEAF, b"negative_sum_tree"].as_ref(),
            b"negative",
            Element::new_sum_item(-150),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert negative sum item");

        // Verify aggregate data
        let batch = StorageBatch::new();
        let transaction = db.start_transaction();

        let merk = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"negative_sum_tree"].as_ref().into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open tree");

        let aggregate_data = merk
            .aggregate_data()
            .expect("expected to get aggregate data");
        // count=2, sum=100+(-150)=-50
        assert_eq!(aggregate_data, AggregateData::ProvableCountAndSum(2, -50));
    }

    #[test]
    fn test_provable_count_sum_tree_single_item() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        // Insert a ProvableCountSumTree with a single item
        db.insert(
            [TEST_LEAF].as_ref(),
            b"single_item_tree",
            Element::new_provable_count_sum_tree(None),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert tree");

        db.insert(
            [TEST_LEAF, b"single_item_tree"].as_ref(),
            b"only_item",
            Element::new_sum_item(42),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert single item");

        // Verify aggregate data
        let batch = StorageBatch::new();
        let transaction = db.start_transaction();

        let merk = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"single_item_tree"].as_ref().into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open tree");

        let aggregate_data = merk
            .aggregate_data()
            .expect("expected to get aggregate data");
        assert_eq!(aggregate_data, AggregateData::ProvableCountAndSum(1, 42));

        // Also verify proof works with single item
        let mut query = Query::new();
        query.insert_key(b"only_item".to_vec());
        let path_query = PathQuery::new_unsized(
            vec![TEST_LEAF.to_vec(), b"single_item_tree".to_vec()],
            query,
        );

        let proof = db
            .prove_query(&path_query, None, grove_version)
            .unwrap()
            .expect("should generate proof");

        let (root_hash, proved_values) =
            GroveDb::verify_query_raw(&proof, &path_query, grove_version)
                .expect("should verify proof");

        let actual_root_hash = db
            .root_hash(None, grove_version)
            .unwrap()
            .expect("should get root hash");

        assert_eq!(root_hash, actual_root_hash);
        assert_eq!(proved_values.len(), 1);
    }

    // ==================== COMPLEX NESTED TREE TESTS ====================

    #[test]
    fn test_provable_count_sum_tree_deeply_nested() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        // Create a deeply nested structure:
        // TEST_LEAF -> level1 (ProvableCountSumTree) -> level2 (ProvableCountSumTree)
        // -> level3 (ProvableCountSumTree) -> items

        db.insert(
            [TEST_LEAF].as_ref(),
            b"level1",
            Element::new_provable_count_sum_tree(None),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert level1");

        db.insert(
            [TEST_LEAF, b"level1"].as_ref(),
            b"level2",
            Element::new_provable_count_sum_tree(None),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert level2");

        db.insert(
            [TEST_LEAF, b"level1", b"level2"].as_ref(),
            b"level3",
            Element::new_provable_count_sum_tree(None),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert level3");

        // Insert items at deepest level
        db.insert(
            [TEST_LEAF, b"level1", b"level2", b"level3"].as_ref(),
            b"item1",
            Element::new_sum_item(10),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item1");

        db.insert(
            [TEST_LEAF, b"level1", b"level2", b"level3"].as_ref(),
            b"item2",
            Element::new_sum_item(20),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item2");

        // Verify aggregate at deepest level
        let batch = StorageBatch::new();
        let transaction = db.start_transaction();

        let level3_merk = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"level1", b"level2", b"level3"].as_ref().into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open level3");

        let level3_aggregate = level3_merk
            .aggregate_data()
            .expect("expected to get aggregate data");
        assert_eq!(level3_aggregate, AggregateData::ProvableCountAndSum(2, 30));

        // Query items at deepest level and verify proof
        let mut query = Query::new();
        query.insert_all();
        let path_query = PathQuery::new_unsized(
            vec![
                TEST_LEAF.to_vec(),
                b"level1".to_vec(),
                b"level2".to_vec(),
                b"level3".to_vec(),
            ],
            query,
        );

        let proof = db
            .prove_query(&path_query, None, grove_version)
            .unwrap()
            .expect("should generate proof");

        let (root_hash, proved_values) =
            GroveDb::verify_query_raw(&proof, &path_query, grove_version)
                .expect("should verify proof");

        let actual_root_hash = db
            .root_hash(None, grove_version)
            .unwrap()
            .expect("should get root hash");

        assert_eq!(root_hash, actual_root_hash);
        assert_eq!(proved_values.len(), 2);
    }

    #[test]
    fn test_provable_count_sum_tree_mixed_tree_types_nested() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        // Create a mixed structure:
        // TEST_LEAF -> regular_tree (Tree) -> provable_sum (ProvableCountSumTree) ->
        // items

        db.insert(
            [TEST_LEAF].as_ref(),
            b"regular_tree",
            Element::new_tree(None),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert regular tree");

        db.insert(
            [TEST_LEAF, b"regular_tree"].as_ref(),
            b"provable_sum",
            Element::new_provable_count_sum_tree(None),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert provable count sum tree");

        // Insert items
        db.insert(
            [TEST_LEAF, b"regular_tree", b"provable_sum"].as_ref(),
            b"a",
            Element::new_sum_item(100),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item a");

        db.insert(
            [TEST_LEAF, b"regular_tree", b"provable_sum"].as_ref(),
            b"b",
            Element::new_item(vec![1, 2, 3]),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item b");

        // Verify aggregate in provable tree
        let batch = StorageBatch::new();
        let transaction = db.start_transaction();

        let merk = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"regular_tree", b"provable_sum"]
                    .as_ref()
                    .into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open provable count sum tree");

        let aggregate_data = merk
            .aggregate_data()
            .expect("expected to get aggregate data");
        // count=2 (both items), sum=100 (only from sum item, regular item contributes
        // 0)
        assert_eq!(aggregate_data, AggregateData::ProvableCountAndSum(2, 100));
    }

    #[test]
    fn test_provable_count_sum_tree_sibling_trees() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        // Create sibling ProvableCountSumTrees at same level
        db.insert(
            [TEST_LEAF].as_ref(),
            b"sibling1",
            Element::new_provable_count_sum_tree(None),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert sibling1");

        db.insert(
            [TEST_LEAF].as_ref(),
            b"sibling2",
            Element::new_provable_count_sum_tree(None),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert sibling2");

        db.insert(
            [TEST_LEAF].as_ref(),
            b"sibling3",
            Element::new_provable_count_sum_tree(None),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert sibling3");

        // Insert different items into each sibling
        db.insert(
            [TEST_LEAF, b"sibling1"].as_ref(),
            b"item",
            Element::new_sum_item(10),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert into sibling1");

        db.insert(
            [TEST_LEAF, b"sibling2"].as_ref(),
            b"item",
            Element::new_sum_item(20),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert into sibling2");

        db.insert(
            [TEST_LEAF, b"sibling3"].as_ref(),
            b"item",
            Element::new_sum_item(30),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert into sibling3");

        // Verify each sibling has correct aggregate data
        let batch = StorageBatch::new();
        let transaction = db.start_transaction();

        for (sibling, expected_sum) in [(b"sibling1", 10), (b"sibling2", 20), (b"sibling3", 30)] {
            let merk = db
                .open_transactional_merk_at_path(
                    [TEST_LEAF, sibling.as_slice()].as_ref().into(),
                    &transaction,
                    Some(&batch),
                    grove_version,
                )
                .unwrap()
                .expect("should open sibling");

            let aggregate_data = merk
                .aggregate_data()
                .expect("expected to get aggregate data");
            assert_eq!(
                aggregate_data,
                AggregateData::ProvableCountAndSum(1, expected_sum)
            );
        }
    }

    #[test]
    fn test_provable_count_sum_tree_with_subtree_inside() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        // ProvableCountSumTree containing another ProvableCountSumTree
        // The parent should see the child tree as one element
        db.insert(
            [TEST_LEAF].as_ref(),
            b"parent",
            Element::new_provable_count_sum_tree(None),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert parent");

        // Add a regular item to parent
        db.insert(
            [TEST_LEAF, b"parent"].as_ref(),
            b"regular_item",
            Element::new_sum_item(50),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert regular item");

        // Add a child ProvableCountSumTree to parent
        db.insert(
            [TEST_LEAF, b"parent"].as_ref(),
            b"child_tree",
            Element::new_provable_count_sum_tree(None),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert child tree");

        // Add items to child tree
        db.insert(
            [TEST_LEAF, b"parent", b"child_tree"].as_ref(),
            b"child_item1",
            Element::new_sum_item(100),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert child_item1");

        db.insert(
            [TEST_LEAF, b"parent", b"child_tree"].as_ref(),
            b"child_item2",
            Element::new_sum_item(200),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert child_item2");

        let batch = StorageBatch::new();
        let transaction = db.start_transaction();

        // Check child aggregate: count=2, sum=300
        let child_merk = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"parent", b"child_tree"].as_ref().into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open child");

        let child_aggregate = child_merk
            .aggregate_data()
            .expect("expected to get aggregate data");
        assert_eq!(child_aggregate, AggregateData::ProvableCountAndSum(2, 300));

        // Check parent aggregate: count=2 (item + child_tree), sum=50+300=350
        let parent_batch = StorageBatch::new();
        let parent_merk = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"parent"].as_ref().into(),
                &transaction,
                Some(&parent_batch),
                grove_version,
            )
            .unwrap()
            .expect("should open parent");

        let parent_aggregate = parent_merk
            .aggregate_data()
            .expect("expected to get aggregate data");
        // Parent count is 3: 1 (regular_item) + 2 (propagated from child_tree's count)
        // Sum from regular_item (50) + child_tree's sum (300) = 350
        assert_eq!(parent_aggregate, AggregateData::ProvableCountAndSum(3, 350));
    }

    #[test]
    fn test_provable_count_sum_tree_delete_and_reinsert() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        // Insert tree with items
        db.insert(
            [TEST_LEAF].as_ref(),
            b"mutable_tree",
            Element::new_provable_count_sum_tree(None),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert tree");

        db.insert(
            [TEST_LEAF, b"mutable_tree"].as_ref(),
            b"item1",
            Element::new_sum_item(100),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item1");

        db.insert(
            [TEST_LEAF, b"mutable_tree"].as_ref(),
            b"item2",
            Element::new_sum_item(200),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item2");

        // Verify initial state
        let batch = StorageBatch::new();
        let transaction = db.start_transaction();

        let merk = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"mutable_tree"].as_ref().into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open tree");

        let aggregate = merk.aggregate_data().expect("should get aggregate");
        assert_eq!(aggregate, AggregateData::ProvableCountAndSum(2, 300));
        drop(merk);
        drop(transaction);

        // Delete one item
        db.delete(
            [TEST_LEAF, b"mutable_tree"].as_ref(),
            b"item1",
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should delete item1");

        // Verify after delete
        let batch2 = StorageBatch::new();
        let transaction2 = db.start_transaction();

        let merk2 = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"mutable_tree"].as_ref().into(),
                &transaction2,
                Some(&batch2),
                grove_version,
            )
            .unwrap()
            .expect("should open tree");

        let aggregate2 = merk2.aggregate_data().expect("should get aggregate");
        assert_eq!(aggregate2, AggregateData::ProvableCountAndSum(1, 200));
        drop(merk2);
        drop(transaction2);

        // Reinsert with different value
        db.insert(
            [TEST_LEAF, b"mutable_tree"].as_ref(),
            b"item1",
            Element::new_sum_item(500),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should reinsert item1");

        // Verify after reinsert
        let batch3 = StorageBatch::new();
        let transaction3 = db.start_transaction();

        let merk3 = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"mutable_tree"].as_ref().into(),
                &transaction3,
                Some(&batch3),
                grove_version,
            )
            .unwrap()
            .expect("should open tree");

        let aggregate3 = merk3.aggregate_data().expect("should get aggregate");
        assert_eq!(aggregate3, AggregateData::ProvableCountAndSum(2, 700));
    }

    #[test]
    fn test_provable_count_sum_tree_avl_rotations() {
        // This test inserts items in a specific order to trigger AVL rotations
        // and verifies that aggregate data (count and sum) remains correct after
        // rebalancing. It also verifies each proof node has the correct count.
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        db.insert(
            [TEST_LEAF].as_ref(),
            b"rotation_tree",
            Element::new_provable_count_sum_tree(None),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert tree");

        // Insert items in ascending order to trigger left rotations
        // Keys: a, b, c, d, e, f, g (ascending order forces right-heavy tree)
        let items = [
            (b"a".to_vec(), 10i64),
            (b"b".to_vec(), 20),
            (b"c".to_vec(), 30),
            (b"d".to_vec(), 40),
            (b"e".to_vec(), 50),
            (b"f".to_vec(), 60),
            (b"g".to_vec(), 70),
        ];

        let mut expected_count = 0u64;
        let mut expected_sum = 0i64;

        for (key, sum_value) in &items {
            db.insert(
                [TEST_LEAF, b"rotation_tree"].as_ref(),
                key,
                Element::new_sum_item(*sum_value),
                None,
                None,
                grove_version,
            )
            .unwrap()
            .expect("should insert item");

            expected_count += 1;
            expected_sum += sum_value;

            // Verify aggregate data after each insert
            let batch = StorageBatch::new();
            let transaction = db.start_transaction();

            let merk = db
                .open_transactional_merk_at_path(
                    [TEST_LEAF, b"rotation_tree"].as_ref().into(),
                    &transaction,
                    Some(&batch),
                    grove_version,
                )
                .unwrap()
                .expect("should open tree");

            let aggregate = merk.aggregate_data().expect("should get aggregate");
            assert_eq!(
                aggregate,
                AggregateData::ProvableCountAndSum(expected_count, expected_sum),
                "Aggregate mismatch after inserting {:?}",
                String::from_utf8_lossy(key)
            );
        }

        // Final verification: count=7, sum=10+20+30+40+50+60+70=280
        assert_eq!(expected_count, 7);
        assert_eq!(expected_sum, 280);

        // Generate proof to ensure hash integrity after rotations
        let mut query = Query::new();
        query.insert_all();
        let path_query =
            PathQuery::new_unsized(vec![TEST_LEAF.to_vec(), b"rotation_tree".to_vec()], query);

        // Use prove_query_non_serialized to get GroveDBProof directly
        let grovedb_proof = db
            .prove_query_non_serialized(&path_query, None, grove_version)
            .unwrap()
            .expect("should generate proof");

        let GroveDBProof::V0(proof_v0) = &grovedb_proof;
        let root_layer = &proof_v0.root_layer;

        // Navigate through proof hierarchy: root -> TEST_LEAF -> rotation_tree
        let test_leaf_layer = root_layer
            .lower_layers
            .get(&TEST_LEAF.to_vec())
            .expect("should have TEST_LEAF layer");

        let rotation_tree_layer = test_leaf_layer
            .lower_layers
            .get(&b"rotation_tree".to_vec())
            .expect("should have rotation_tree layer");

        // Execute the proof to build the tree structure
        let proof_tree =
            execute_merk_proof(&rotation_tree_layer.merk_proof).expect("should execute proof");

        // Collect all nodes with counts from the tree
        let tree_nodes = collect_tree_node_counts(&proof_tree);

        // All 7 items should be in the proof tree
        assert_eq!(
            tree_nodes.len(),
            7,
            "Should have 7 nodes with counts in proof tree"
        );

        // Verify root node has correct total count = 7
        let root_count = get_node_count(&proof_tree.node).expect("Root should have count data");
        assert_eq!(
            root_count, 7,
            "Root node should have count=7, got count={}",
            root_count
        );

        // Verify all node counts are valid (>= 1) and form a proper tree
        // Internal nodes have count = 1 + left_count + right_count
        // Leaf nodes have count = 1
        for (key, count) in &tree_nodes {
            assert!(
                *count >= 1,
                "Node {:?} should have count >= 1, got count={}",
                String::from_utf8_lossy(key),
                count
            );
        }

        // Verify the counts follow AVL tree invariant:
        // - Each node's count = 1 + left_subtree_count + right_subtree_count
        // - We can verify this by checking the sum of all (count - 1) = total nodes - 1
        // But simpler: verify each leaf node (no children in proof) has count=1
        // and root has count=7
        fn verify_tree_counts(tree: &grovedb_merk::proofs::tree::Tree) -> u64 {
            let node_count = get_node_count(&tree.node).unwrap_or(0);
            let left_count = tree
                .left
                .as_ref()
                .map(|c| verify_tree_counts(&c.tree))
                .unwrap_or(0);
            let right_count = tree
                .right
                .as_ref()
                .map(|c| verify_tree_counts(&c.tree))
                .unwrap_or(0);

            // Node count should equal 1 + left_count + right_count
            let expected_count = 1 + left_count + right_count;
            assert_eq!(
                node_count, expected_count,
                "Node count {} should equal 1 + {} + {} = {}",
                node_count, left_count, right_count, expected_count
            );

            node_count
        }

        let total_verified = verify_tree_counts(&proof_tree);
        assert_eq!(total_verified, 7, "Total tree count should be 7");
    }

    #[test]
    fn test_provable_count_sum_tree_many_items_rotation_stress() {
        // Stress test: insert many items to trigger multiple rotations
        // Also verifies proof node counts are correct
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        db.insert(
            [TEST_LEAF].as_ref(),
            b"stress_tree",
            Element::new_provable_count_sum_tree(None),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert tree");

        // Insert 50 items with various sum values
        let item_count = 50u64;
        let mut expected_sum = 0i64;
        let mut items: Vec<(String, i64)> = Vec::new();

        for i in 0..item_count {
            let key = format!("key_{:03}", i);
            let sum_value = (i as i64) * 10 - 250; // Mix of positive and negative

            db.insert(
                [TEST_LEAF, b"stress_tree"].as_ref(),
                key.as_bytes(),
                Element::new_sum_item(sum_value),
                None,
                None,
                grove_version,
            )
            .unwrap()
            .expect("should insert item");

            expected_sum += sum_value;
            items.push((key, sum_value));
        }

        // Verify final aggregate
        let batch = StorageBatch::new();
        let transaction = db.start_transaction();

        let merk = db
            .open_transactional_merk_at_path(
                [TEST_LEAF, b"stress_tree"].as_ref().into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("should open tree");

        let aggregate = merk.aggregate_data().expect("should get aggregate");
        assert_eq!(
            aggregate,
            AggregateData::ProvableCountAndSum(item_count, expected_sum)
        );
        drop(merk);
        drop(transaction);

        // Generate proof and verify counts
        let mut query = Query::new();
        query.insert_all();
        let path_query =
            PathQuery::new_unsized(vec![TEST_LEAF.to_vec(), b"stress_tree".to_vec()], query);

        // Use prove_query_non_serialized to get GroveDBProof directly
        let grovedb_proof = db
            .prove_query_non_serialized(&path_query, None, grove_version)
            .unwrap()
            .expect("should generate proof");

        let GroveDBProof::V0(proof_v0) = &grovedb_proof;
        let root_layer = &proof_v0.root_layer;

        // Navigate through proof hierarchy: root -> TEST_LEAF -> stress_tree
        let test_leaf_layer = root_layer
            .lower_layers
            .get(&TEST_LEAF.to_vec())
            .expect("should have TEST_LEAF layer");

        let stress_tree_layer = test_leaf_layer
            .lower_layers
            .get(&b"stress_tree".to_vec())
            .expect("should have stress_tree layer");

        // Execute the proof to build the tree structure
        let proof_tree =
            execute_merk_proof(&stress_tree_layer.merk_proof).expect("should execute proof");

        // Collect all nodes with counts from the tree
        let tree_nodes = collect_tree_node_counts(&proof_tree);

        // All 50 items should be in the proof tree
        assert_eq!(
            tree_nodes.len(),
            item_count as usize,
            "Should have {} nodes with counts in proof tree",
            item_count
        );

        // Verify root node has correct total count
        let root_count = get_node_count(&proof_tree.node).expect("Root should have count data");
        assert_eq!(
            root_count, item_count,
            "Root node should have count={}, got count={}",
            item_count, root_count
        );

        // Verify the counts follow AVL tree invariant recursively
        fn verify_tree_counts(tree: &grovedb_merk::proofs::tree::Tree) -> u64 {
            let node_count = get_node_count(&tree.node).unwrap_or(0);
            let left_count = tree
                .left
                .as_ref()
                .map(|c| verify_tree_counts(&c.tree))
                .unwrap_or(0);
            let right_count = tree
                .right
                .as_ref()
                .map(|c| verify_tree_counts(&c.tree))
                .unwrap_or(0);

            // Node count should equal 1 + left_count + right_count
            let expected_count = 1 + left_count + right_count;
            assert_eq!(
                node_count, expected_count,
                "Node count {} should equal 1 + {} + {} = {}",
                node_count, left_count, right_count, expected_count
            );

            node_count
        }

        let total_verified = verify_tree_counts(&proof_tree);
        assert_eq!(
            total_verified, item_count,
            "Total tree count should be {}",
            item_count
        );
    }

    // ==================== ABSENCE PROOF TESTS ====================

    #[test]
    fn test_provable_count_sum_tree_query_existing_and_nonexistent_keys_on_right() {
        // This test mirrors the platform query that queries for multiple addresses
        // including one that doesn't exist in a ProvableCountSumTree.
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        // Create a ProvableCountSumTree (like the platform address balances tree)
        db.insert(
            [TEST_LEAF].as_ref(),
            b"balances",
            Element::empty_provable_count_sum_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert tree");

        // Insert two known items (like platform addresses with balances)
        let address1 = b"address_1_xxxxxxxxxxxxxxxx"; // 26 bytes like platform addresses
        let address2 = b"address_2_xxxxxxxxxxxxxxxx";
        let unknown_address = b"unknown_address_xxxxxxxxx";

        let item_value1 = b"some_data_for_address_1".to_vec();
        let item_value2 = b"some_data_for_address_2".to_vec();
        let sum_value1: i64 = 1000000;
        let sum_value2: i64 = 2000000;

        db.insert(
            [TEST_LEAF, b"balances"].as_ref(),
            address1,
            Element::new_item_with_sum_item(item_value1, sum_value1),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert address 1");

        db.insert(
            [TEST_LEAF, b"balances"].as_ref(),
            address2,
            Element::new_item_with_sum_item(item_value2, sum_value2),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert address 2");

        // Query for all three keys (two existing + one non-existent)
        let mut query = Query::new();
        query.insert_key(address1.to_vec());
        query.insert_key(address2.to_vec());
        query.insert_key(unknown_address.to_vec());

        // Use sized query with limit (required for absence proof verification)
        let path_query = PathQuery::new(
            vec![TEST_LEAF.to_vec(), b"balances".to_vec()],
            SizedQuery::new(query, Some(100), None),
        );

        // Generate proof
        let proof = db
            .prove_query(&path_query, None, grove_version)
            .unwrap()
            .expect("should generate proof for mixed existing/nonexistent keys");

        // Verify the proof with absence proof verification
        let (root_hash, proved_values) =
            GroveDb::verify_query_with_absence_proof(&proof, &path_query, grove_version)
                .expect("should verify proof with mixed existing/nonexistent keys");

        let actual_root_hash = db
            .root_hash(None, grove_version)
            .unwrap()
            .expect("should get root hash");

        assert_eq!(root_hash, actual_root_hash, "Root hash should match");

        // Should have 3 results (2 existing + 1 absent)
        assert_eq!(
            proved_values.len(),
            3,
            "Should have 3 results (2 existing + 1 absent)"
        );

        // Verify the proved values contain our keys with correct presence/absence
        let address1_result = proved_values
            .iter()
            .find(|(_, key, _)| key == &address1.to_vec());
        let address2_result = proved_values
            .iter()
            .find(|(_, key, _)| key == &address2.to_vec());
        let unknown_result = proved_values
            .iter()
            .find(|(_, key, _)| key == &unknown_address.to_vec());

        assert!(
            address1_result.is_some(),
            "Should contain address1 in results"
        );
        assert!(
            address2_result.is_some(),
            "Should contain address2 in results"
        );
        assert!(
            unknown_result.is_some(),
            "Should contain unknown_address in results"
        );

        // Existing keys should have Some(element), unknown should have None
        assert!(
            address1_result.unwrap().2.is_some(),
            "address1 should have an element"
        );
        assert!(
            address2_result.unwrap().2.is_some(),
            "address2 should have an element"
        );
        assert!(
            unknown_result.unwrap().2.is_none(),
            "unknown_address should be absent (None)"
        );
    }

    #[test]
    fn test_provable_count_sum_tree_query_existing_and_nonexistent_keys_on_left() {
        // This test mirrors the platform query that queries for multiple addresses
        // including one that doesn't exist in a ProvableCountSumTree.
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        // Create a ProvableCountSumTree (like the platform address balances tree)
        db.insert(
            [TEST_LEAF].as_ref(),
            b"balances",
            Element::empty_provable_count_sum_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert tree");

        // Insert two known items (like platform addresses with balances)
        let address1 = b"address_1_xxxxxxxxxxxxxxxx"; // 26 bytes like platform addresses
        let address2 = b"address_2_xxxxxxxxxxxxxxxx";
        let unknown_address = b"aa_unknown_address_xxxxxxxxx";

        let item_value1 = b"some_data_for_address_1".to_vec();
        let item_value2 = b"some_data_for_address_2".to_vec();
        let sum_value1: i64 = 1000000;
        let sum_value2: i64 = 2000000;

        db.insert(
            [TEST_LEAF, b"balances"].as_ref(),
            address1,
            Element::new_item_with_sum_item(item_value1, sum_value1),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert address 1");

        db.insert(
            [TEST_LEAF, b"balances"].as_ref(),
            address2,
            Element::new_item_with_sum_item(item_value2, sum_value2),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert address 2");

        // Query for all three keys (two existing + one non-existent)
        let mut query = Query::new();
        query.insert_key(address1.to_vec());
        query.insert_key(address2.to_vec());
        query.insert_key(unknown_address.to_vec());

        // Use sized query with limit (required for absence proof verification)
        let path_query = PathQuery::new(
            vec![TEST_LEAF.to_vec(), b"balances".to_vec()],
            SizedQuery::new(query, Some(100), None),
        );

        // Generate proof
        let proof = db
            .prove_query(&path_query, None, grove_version)
            .unwrap()
            .expect("should generate proof for mixed existing/nonexistent keys");

        // Verify the proof with absence proof verification
        let (root_hash, proved_values) =
            GroveDb::verify_query_with_absence_proof(&proof, &path_query, grove_version)
                .expect("should verify proof with mixed existing/nonexistent keys");

        let actual_root_hash = db
            .root_hash(None, grove_version)
            .unwrap()
            .expect("should get root hash");

        assert_eq!(root_hash, actual_root_hash, "Root hash should match");

        // Should have 3 results (2 existing + 1 absent)
        assert_eq!(
            proved_values.len(),
            3,
            "Should have 3 results (2 existing + 1 absent)"
        );

        // Verify the proved values contain our keys with correct presence/absence
        let address1_result = proved_values
            .iter()
            .find(|(_, key, _)| key == &address1.to_vec());
        let address2_result = proved_values
            .iter()
            .find(|(_, key, _)| key == &address2.to_vec());
        let unknown_result = proved_values
            .iter()
            .find(|(_, key, _)| key == &unknown_address.to_vec());

        assert!(
            address1_result.is_some(),
            "Should contain address1 in results"
        );
        assert!(
            address2_result.is_some(),
            "Should contain address2 in results"
        );
        assert!(
            unknown_result.is_some(),
            "Should contain unknown_address in results"
        );

        // Existing keys should have Some(element), unknown should have None
        assert!(
            address1_result.unwrap().2.is_some(),
            "address1 should have an element"
        );
        assert!(
            address2_result.unwrap().2.is_some(),
            "address2 should have an element"
        );
        assert!(
            unknown_result.unwrap().2.is_none(),
            "unknown_address should be absent (None)"
        );
    }

    #[test]
    fn test_provable_count_sum_tree_query_existing_and_nonexistent_keys_in_middle() {
        // This test mirrors the platform query that queries for multiple addresses
        // including one that doesn't exist in a ProvableCountSumTree.
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        // Create a ProvableCountSumTree (like the platform address balances tree)
        db.insert(
            [TEST_LEAF].as_ref(),
            b"balances",
            Element::empty_provable_count_sum_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert tree");

        // Insert two known items (like platform addresses with balances)
        let address1 = b"address_1_xxxxxxxxxxxxxxxx"; // 26 bytes like platform addresses
        let address2 = b"address_3_xxxxxxxxxxxxxxxx";
        let unknown_address = b"address_2_xxxxxxxxxxxxxxxx";

        let item_value1 = b"some_data_for_address_1".to_vec();
        let item_value2 = b"some_data_for_address_3".to_vec();
        let sum_value1: i64 = 1000000;
        let sum_value2: i64 = 2000000;

        db.insert(
            [TEST_LEAF, b"balances"].as_ref(),
            address1,
            Element::new_item_with_sum_item(item_value1, sum_value1),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert address 1");

        db.insert(
            [TEST_LEAF, b"balances"].as_ref(),
            address2,
            Element::new_item_with_sum_item(item_value2, sum_value2),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert address 2");

        // Query for all three keys (two existing + one non-existent)
        let mut query = Query::new();
        query.insert_key(address1.to_vec());
        query.insert_key(address2.to_vec());
        query.insert_key(unknown_address.to_vec());

        // Use sized query with limit (required for absence proof verification)
        let path_query = PathQuery::new(
            vec![TEST_LEAF.to_vec(), b"balances".to_vec()],
            SizedQuery::new(query, Some(100), None),
        );

        // Generate proof
        let proof = db
            .prove_query(&path_query, None, grove_version)
            .unwrap()
            .expect("should generate proof for mixed existing/nonexistent keys");

        // Verify the proof with absence proof verification
        let (root_hash, proved_values) =
            GroveDb::verify_query_with_absence_proof(&proof, &path_query, grove_version)
                .expect("should verify proof with mixed existing/nonexistent keys");

        let actual_root_hash = db
            .root_hash(None, grove_version)
            .unwrap()
            .expect("should get root hash");

        assert_eq!(root_hash, actual_root_hash, "Root hash should match");

        // Should have 3 results (2 existing + 1 absent)
        assert_eq!(
            proved_values.len(),
            3,
            "Should have 3 results (2 existing + 1 absent)"
        );

        // Verify the proved values contain our keys with correct presence/absence
        let address1_result = proved_values
            .iter()
            .find(|(_, key, _)| key == &address1.to_vec());
        let address2_result = proved_values
            .iter()
            .find(|(_, key, _)| key == &address2.to_vec());
        let unknown_result = proved_values
            .iter()
            .find(|(_, key, _)| key == &unknown_address.to_vec());

        assert!(
            address1_result.is_some(),
            "Should contain address1 in results"
        );
        assert!(
            address2_result.is_some(),
            "Should contain address2 in results"
        );
        assert!(
            unknown_result.is_some(),
            "Should contain unknown_address in results"
        );

        // Existing keys should have Some(element), unknown should have None
        assert!(
            address1_result.unwrap().2.is_some(),
            "address1 should have an element"
        );
        assert!(
            address2_result.unwrap().2.is_some(),
            "address2 should have an element"
        );
        assert!(
            unknown_result.unwrap().2.is_none(),
            "unknown_address should be absent (None)"
        );
    }

    #[test]
    fn test_provable_count_sum_tree_absence_proof_uses_kvdigest_count() {
        // This test verifies that absence proofs in ProvableCountSumTree use
        // KVDigestCount nodes (not just KVDigest) so that the count information
        // is available for hash verification.
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        // Create a ProvableCountSumTree with multiple items
        db.insert(
            [TEST_LEAF].as_ref(),
            b"counted_tree",
            Element::empty_provable_count_sum_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert tree");

        // Insert items: "aaa", "ccc", "eee" (leaving gaps for absence proofs)
        for (key, value) in [
            (b"aaa".as_slice(), 10i64),
            (b"ccc".as_slice(), 20i64),
            (b"eee".as_slice(), 30i64),
        ] {
            db.insert(
                [TEST_LEAF, b"counted_tree"].as_ref(),
                key,
                Element::new_sum_item(value),
                None,
                None,
                grove_version,
            )
            .unwrap()
            .expect("should insert item");
        }

        // Query for a non-existent key "bbb" (between "aaa" and "ccc")
        // This should generate an absence proof with KVDigestCount nodes
        let mut query = Query::new();
        query.insert_key(b"bbb".to_vec());
        let path_query =
            PathQuery::new_unsized(vec![TEST_LEAF.to_vec(), b"counted_tree".to_vec()], query);

        // Generate proof
        let proof = db
            .prove_query(&path_query, None, grove_version)
            .unwrap()
            .expect("should generate absence proof");

        // Verify the proof works
        let (root_hash, proved_values) =
            GroveDb::verify_query_raw(&proof, &path_query, grove_version)
                .expect("should verify absence proof");

        let actual_root_hash = db
            .root_hash(None, grove_version)
            .unwrap()
            .expect("should get root hash");

        assert_eq!(root_hash, actual_root_hash, "Root hash should match");
        assert_eq!(
            proved_values.len(),
            0,
            "Should have no proved values for absence"
        );

        // Now inspect the proof to verify KVDigestCount is used
        // Parse the GroveDB proof to get the merk layer proof
        let grove_proof: GroveDBProof =
            bincode::decode_from_slice(&proof, bincode::config::standard())
                .expect("should decode proof")
                .0;

        // Helper function to check if any op contains KVDigestCount
        fn has_kvdigest_count(ops: &[Op]) -> bool {
            ops.iter().any(|op| {
                matches!(
                    op,
                    Op::Push(Node::KVDigestCount(..)) | Op::PushInverted(Node::KVDigestCount(..))
                )
            })
        }

        // Helper function to check if any op contains plain KVDigest (without count)
        fn has_plain_kvdigest(ops: &[Op]) -> bool {
            ops.iter().any(|op| {
                matches!(
                    op,
                    Op::Push(Node::KVDigest(..)) | Op::PushInverted(Node::KVDigest(..))
                )
            })
        }

        // Extract ops from the proof layers
        match grove_proof {
            GroveDBProof::V0(proof_v0) => {
                // Check the inner merk proofs for KVDigestCount usage
                let mut found_kvdigest_count = false;
                let mut found_plain_kvdigest = false;

                // Check the root layer and lower layers
                fn check_layer_proof(
                    layer: &crate::operations::proof::LayerProof,
                    found_kvdigest_count: &mut bool,
                    found_plain_kvdigest: &mut bool,
                ) {
                    // Decode the merk proof ops
                    let decoder = Decoder::new(&layer.merk_proof);
                    let ops: Vec<Op> = decoder.collect::<Result<Vec<_>, _>>().unwrap_or_default();

                    if has_kvdigest_count(&ops) {
                        *found_kvdigest_count = true;
                    }
                    if has_plain_kvdigest(&ops) {
                        *found_plain_kvdigest = true;
                    }

                    // Recursively check lower layers
                    for (_, lower_layer) in &layer.lower_layers {
                        check_layer_proof(lower_layer, found_kvdigest_count, found_plain_kvdigest);
                    }
                }

                check_layer_proof(
                    &proof_v0.root_layer,
                    &mut found_kvdigest_count,
                    &mut found_plain_kvdigest,
                );

                // For ProvableCountSumTree absence proofs, we should have KVDigestCount
                // and NOT plain KVDigest nodes in the counted tree layer
                assert!(
                    found_kvdigest_count,
                    "Absence proof should contain KVDigestCount nodes for ProvableCountSumTree"
                );
                // Note: Plain KVDigest may still exist in non-counted parent
                // layers (e.g., root) So we don't assert
                // !found_plain_kvdigest here
            }
        }
    }
}
