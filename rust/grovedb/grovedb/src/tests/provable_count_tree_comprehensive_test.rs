//! Comprehensive tests for ProvableCountTree functionality

#[cfg(test)]
mod tests {
    use grovedb_merk::proofs::{Decoder, Node, Op};
    use grovedb_version::version::GroveVersion;

    use crate::{
        batch::QualifiedGroveDbOp, operations::proof::util::ProvedPathKeyValue,
        query_result_type::QueryResultType, tests::make_test_grovedb, Element, GroveDb, PathQuery,
        Query, SizedQuery,
    };

    #[test]
    fn test_provable_count_tree_hash_changes_with_count() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        // Create a ProvableCountTree
        db.insert(
            &[] as &[&[u8]],
            b"count_tree",
            Element::empty_provable_count_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert provable count tree");

        // Get initial root hash
        let initial_hash = db
            .root_hash(None, grove_version)
            .unwrap()
            .expect("should get root hash");

        // Insert one item
        db.insert(
            &[b"count_tree"],
            b"item1",
            Element::new_item(b"value1".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item");

        let hash_after_one = db
            .root_hash(None, grove_version)
            .unwrap()
            .expect("should get root hash");

        // Insert second item
        db.insert(
            &[b"count_tree"],
            b"item2",
            Element::new_item(b"value2".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item");

        let hash_after_two = db
            .root_hash(None, grove_version)
            .unwrap()
            .expect("should get root hash");

        // All hashes should be different
        assert_ne!(
            initial_hash, hash_after_one,
            "Hash should change after first insert"
        );
        assert_ne!(
            hash_after_one, hash_after_two,
            "Hash should change after second insert"
        );
        assert_ne!(
            initial_hash, hash_after_two,
            "Hash should be different from initial"
        );
    }

    #[test]
    fn test_provable_count_tree_proof_contains_count_nodes() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        // Create a single ProvableCountTree
        db.insert(
            &[] as &[&[u8]],
            b"counts",
            Element::empty_provable_count_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert tree");

        // Insert a few items
        for i in 0..3 {
            let key = format!("item{}", i).into_bytes();
            let value = format!("value{}", i).into_bytes();
            db.insert(
                &[b"counts"],
                &key,
                Element::new_item(value),
                None,
                None,
                grove_version,
            )
            .unwrap()
            .expect("should insert item");
        }

        // Query for all items to ensure we get count nodes in the proof
        let query = Query::new(); // Empty query gets all items
        let path_query = PathQuery::new_unsized(vec![b"counts".to_vec()], query);

        let proof = db
            .prove_query(&path_query, None, grove_version)
            .unwrap()
            .expect("should generate proof");

        // First, deserialize the GroveDBProof
        let config = bincode::config::standard()
            .with_big_endian()
            .with_no_limit();
        let grovedb_proof: crate::operations::proof::GroveDBProof =
            bincode::decode_from_slice(&proof, config)
                .expect("should deserialize proof")
                .0;

        // Check if there are lower layers (which would contain the actual merk proof
        // for the ProvableCountTree)
        let has_lower_layers = match &grovedb_proof {
            crate::operations::proof::GroveDBProof::V0(proof_v0) => {
                !proof_v0.root_layer.lower_layers.is_empty()
            }
        };

        assert!(
            has_lower_layers,
            "Proof should have lower layers for ProvableCountTree"
        );

        // Extract the merk proof from the lower layer (the actual ProvableCountTree)
        let merk_proof = match &grovedb_proof {
            crate::operations::proof::GroveDBProof::V0(proof_v0) => proof_v0
                .root_layer
                .lower_layers
                .get(b"counts".as_slice())
                .expect("should have counts layer")
                .merk_proof
                .as_slice(),
        };

        // Decode proof and check for count nodes
        let decoder = Decoder::new(merk_proof);
        let mut found_count_node = false;

        for op in decoder {
            if let Ok(op) = op {
                match op {
                    Op::Push(node) | Op::PushInverted(node) => match node {
                        Node::KVCount(k, _, c) => {
                            eprintln!("Found KVCount node: key={}, count={}", hex::encode(k), c);
                            found_count_node = true;
                            break;
                        }
                        Node::KVHashCount(_, c) => {
                            eprintln!("Found KVHashCount node: count={}", c);
                            found_count_node = true;
                            break;
                        }
                        n => {
                            eprintln!("Found node: {:?}", n);
                        }
                    },
                    _ => {}
                }
            }
        }

        assert!(
            found_count_node,
            "Proof should contain at least one count node"
        );
    }

    #[test]
    fn test_provable_count_tree_batch_operations() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        // Create tree using batch
        let ops = vec![QualifiedGroveDbOp::insert_or_replace_op(
            vec![],
            b"batch_tree".to_vec(),
            Element::empty_provable_count_tree(),
        )];

        db.apply_batch(ops, None, None, grove_version)
            .unwrap()
            .expect("should apply batch");

        // Insert multiple items using batch
        let mut batch_ops = vec![];
        for i in 0..10 {
            batch_ops.push(QualifiedGroveDbOp::insert_or_replace_op(
                vec![b"batch_tree".to_vec()],
                format!("key{:02}", i).into_bytes(),
                Element::new_item(format!("value{}", i).into_bytes()),
            ));
        }

        db.apply_batch(batch_ops, None, None, grove_version)
            .unwrap()
            .expect("should apply batch");

        // Verify count through query
        let mut query = Query::new();
        query.insert_all();
        let path_query = PathQuery::new_unsized(vec![b"batch_tree".to_vec()], query);

        let (elements, _) = db
            .query_raw(
                &path_query,
                true,
                true,
                false,
                QueryResultType::QueryPathKeyElementTrioResultType,
                None,
                grove_version,
            )
            .unwrap()
            .expect("should query");

        assert_eq!(elements.len(), 10, "Should have 10 items");
    }

    #[test]
    fn test_provable_count_tree_deletion_updates_count() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        // Create tree and insert items
        db.insert(
            &[] as &[&[u8]],
            b"delete_test",
            Element::empty_provable_count_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert tree");

        // Insert 5 items
        for i in 0..5 {
            db.insert(
                &[b"delete_test"],
                &format!("item{}", i).into_bytes(),
                Element::new_item(format!("value{}", i).into_bytes()),
                None,
                None,
                grove_version,
            )
            .unwrap()
            .expect("should insert item");
        }

        let hash_with_five = db
            .root_hash(None, grove_version)
            .unwrap()
            .expect("should get root hash");

        // Delete one item
        db.delete(&[b"delete_test"], b"item2", None, None, grove_version)
            .unwrap()
            .expect("should delete item");

        let hash_with_four = db
            .root_hash(None, grove_version)
            .unwrap()
            .expect("should get root hash");

        // Delete another item
        db.delete(&[b"delete_test"], b"item4", None, None, grove_version)
            .unwrap()
            .expect("should delete item");

        let hash_with_three = db
            .root_hash(None, grove_version)
            .unwrap()
            .expect("should get root hash");

        // All hashes should be different
        assert_ne!(
            hash_with_five, hash_with_four,
            "Hash should change after first delete"
        );
        assert_ne!(
            hash_with_four, hash_with_three,
            "Hash should change after second delete"
        );
    }

    #[test]
    fn test_provable_count_tree_with_items_only() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        // Create a ProvableCountTree
        db.insert(
            &[] as &[&[u8]],
            b"item_count_tree",
            Element::empty_provable_count_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert tree");

        // Insert regular items (ProvableCountTree doesn't support sum items)
        for i in 0..5 {
            db.insert(
                &[b"item_count_tree"],
                &format!("item{}", i).into_bytes(),
                Element::new_item(format!("value{}", i).into_bytes()),
                None,
                None,
                grove_version,
            )
            .unwrap()
            .expect("should insert item");
        }

        // Query and verify
        let mut query = Query::new();
        query.insert_all();
        let path_query = PathQuery::new_unsized(vec![b"item_count_tree".to_vec()], query);

        let proof = db
            .prove_query(&path_query, None, grove_version)
            .unwrap()
            .expect("should generate proof");

        // Verify proof
        let (_root_hash, proved_path_key_optional_values) =
            GroveDb::verify_query_raw(&proof, &path_query, grove_version)
                .expect("should verify proof");

        assert_eq!(
            proved_path_key_optional_values.len(),
            5,
            "Should have 5 items in proof"
        );
    }

    #[test]
    fn test_provable_count_tree_proof_verification() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        // Create tree structure
        db.insert(
            &[] as &[&[u8]],
            b"verified",
            Element::empty_provable_count_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert tree");

        // Insert test data
        let test_data = vec![
            (b"alice" as &[u8], b"data1" as &[u8]),
            (b"bob" as &[u8], b"data2" as &[u8]),
            (b"carol" as &[u8], b"data3" as &[u8]),
            (b"dave" as &[u8], b"data4" as &[u8]),
        ];

        for (key, value) in &test_data {
            db.insert(
                &[b"verified"],
                *key,
                Element::new_item(value.to_vec()),
                None,
                None,
                grove_version,
            )
            .unwrap()
            .expect("should insert item");
        }

        // Generate proof for specific keys
        let mut query = Query::new();
        query.insert_key(b"alice".to_vec());
        query.insert_key(b"carol".to_vec());

        let path_query = PathQuery::new_unsized(vec![b"verified".to_vec()], query);

        let proof = db
            .prove_query(&path_query, None, grove_version)
            .unwrap()
            .expect("should generate proof");

        // Verify the proof
        let (root_hash, proved_values) =
            GroveDb::verify_query_raw(&proof, &path_query, grove_version)
                .expect("should verify proof");

        // Check root hash matches
        let actual_root_hash = db
            .root_hash(None, grove_version)
            .unwrap()
            .expect("should get root hash");

        assert_eq!(root_hash, actual_root_hash, "Root hash should match");

        // Check proved values
        assert_eq!(proved_values.len(), 2, "Should have 2 proved values");

        // Verify the values are correct
        for proved_value in proved_values {
            let ProvedPathKeyValue {
                path, key, value, ..
            } = proved_value;
            assert_eq!(path, vec![b"verified".to_vec()]);
            if key == b"alice" {
                // The value is a serialized Element, so we need to deserialize it
                let element =
                    Element::deserialize(&value, grove_version).expect("should deserialize");
                assert_eq!(element, Element::new_item(b"data1".to_vec()));
            } else if key == b"carol" {
                // The value is a serialized Element, so we need to deserialize it
                let element =
                    Element::deserialize(&value, grove_version).expect("should deserialize");
                assert_eq!(element, Element::new_item(b"data3".to_vec()));
            } else {
                panic!("Unexpected key in proof");
            }
        }
    }

    #[test]
    fn test_provable_count_tree_empty_proof() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        // Create empty tree
        db.insert(
            &[] as &[&[u8]],
            b"empty",
            Element::empty_provable_count_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert tree");

        // Query empty tree
        let mut query = Query::new();
        query.insert_all();
        let path_query = PathQuery::new_unsized(vec![b"empty".to_vec()], query);

        let proof = db
            .prove_query(&path_query, None, grove_version)
            .unwrap()
            .expect("should generate proof");

        // Verify empty proof
        let (root_hash, proved_values) =
            GroveDb::verify_query_raw(&proof, &path_query, grove_version)
                .expect("should verify proof");

        assert_eq!(
            proved_values.len(),
            0,
            "Should have no values in empty tree"
        );

        let actual_root_hash = db
            .root_hash(None, grove_version)
            .unwrap()
            .expect("should get root hash");

        assert_eq!(
            root_hash, actual_root_hash,
            "Root hash should match for empty tree"
        );
    }

    #[test]
    fn test_provable_count_tree_range_query() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        // Create tree
        db.insert(
            &[] as &[&[u8]],
            b"range_test",
            Element::empty_provable_count_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert tree");

        // Insert items with predictable keys
        for i in 0..20 {
            let key = format!("item_{:02}", i).into_bytes();
            let value = format!("value_{}", i).into_bytes();
            db.insert(
                &[b"range_test"],
                &key,
                Element::new_item(value),
                None,
                None,
                grove_version,
            )
            .unwrap()
            .expect("should insert item");
        }

        // Range query from item_05 to item_15
        let mut query = Query::new();
        query.insert_range_inclusive(b"item_05".to_vec()..=b"item_15".to_vec());

        let path_query = PathQuery::new_unsized(vec![b"range_test".to_vec()], query);

        let proof = db
            .prove_query(&path_query, None, grove_version)
            .unwrap()
            .expect("should generate proof");

        // Verify the proof
        let (root_hash, proved_values) =
            GroveDb::verify_query_raw(&proof, &path_query, grove_version)
                .expect("should verify proof");

        // Should have items 05 through 15 (11 items)
        assert_eq!(proved_values.len(), 11, "Should have 11 items in range");

        // Verify root hash
        let actual_root_hash = db
            .root_hash(None, grove_version)
            .unwrap()
            .expect("should get root hash");

        assert_eq!(root_hash, actual_root_hash, "Root hash should match");
    }

    #[test]
    fn test_provable_count_tree_with_limit() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        // Create tree
        db.insert(
            &[] as &[&[u8]],
            b"limit_test",
            Element::empty_provable_count_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert tree");

        // Insert 20 items
        for i in 0..20 {
            let key = format!("key_{:02}", i).into_bytes();
            let value = format!("value_{}", i).into_bytes();
            db.insert(
                &[b"limit_test"],
                &key,
                Element::new_item(value),
                None,
                None,
                grove_version,
            )
            .unwrap()
            .expect("should insert item");
        }

        // Query with limit
        let mut query = Query::new_with_direction(false); // ascending
        query.insert_all();

        let sized_query = SizedQuery::new(query, Some(5), None);
        let path_query = PathQuery::new(vec![b"limit_test".to_vec()], sized_query);

        let proof = db
            .prove_query(&path_query, None, grove_version)
            .unwrap()
            .expect("should generate proof");

        // Verify the proof
        let (root_hash, proved_values) =
            GroveDb::verify_query_raw(&proof, &path_query, grove_version)
                .expect("should verify proof");

        assert_eq!(
            proved_values.len(),
            5,
            "Should have exactly 5 items due to limit"
        );

        // Verify root hash still matches (limit doesn't affect root)
        let actual_root_hash = db
            .root_hash(None, grove_version)
            .unwrap()
            .expect("should get root hash");

        assert_eq!(root_hash, actual_root_hash, "Root hash should match");
    }

    #[test]
    fn test_provable_count_tree_conditional_query() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        // Create tree
        db.insert(
            &[] as &[&[u8]],
            b"conditional",
            Element::empty_provable_count_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert tree");

        // Insert different items
        db.insert(
            &[b"conditional"],
            b"item1",
            Element::new_item(b"value1".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item");

        db.insert(
            &[b"conditional"],
            b"item2",
            Element::new_item(b"value2".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item");

        db.insert(
            &[b"conditional"],
            b"item3",
            Element::new_item(b"value3".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item");

        db.insert(
            &[b"conditional"],
            b"item4",
            Element::new_item(b"value4".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item");

        // Query for specific keys
        let mut query = Query::new();
        query.insert_key(b"item1".to_vec());
        query.insert_key(b"item2".to_vec());

        let path_query = PathQuery::new_unsized(vec![b"conditional".to_vec()], query);

        let proof = db
            .prove_query(&path_query, None, grove_version)
            .unwrap()
            .expect("should generate proof");

        // Verify we only get the requested items
        let (_, proved_values) = GroveDb::verify_query_raw(&proof, &path_query, grove_version)
            .expect("should verify proof");

        assert_eq!(proved_values.len(), 2, "Should have exactly 2 items");
    }
}
