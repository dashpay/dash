//! Trunk proof tests

#[cfg(test)]
mod tests {
    use blake3::Hasher;
    use grovedb_merk::proofs::{
        branch::depth::calculate_max_tree_depth_from_count, Decoder, Node, Op,
    };
    use grovedb_version::version::GroveVersion;
    use rand::{rngs::StdRng, Rng, SeedableRng};

    use crate::{
        operations::proof::GroveDBProof,
        query::PathTrunkChunkQuery,
        tests::{common::EMPTY_PATH, make_empty_grovedb},
        Element, GroveDb,
    };

    #[test]
    fn test_trunk_proof_with_count_sum_tree() {
        let grove_version = GroveVersion::latest();
        let db = make_empty_grovedb();

        // Use a seeded RNG for reproducibility
        let mut rng = StdRng::seed_from_u64(12345);

        // Insert 3 trees at the root level
        // Tree 1: regular tree
        db.insert(
            EMPTY_PATH,
            b"tree1",
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("successful tree1 insert");

        // Tree 2: another regular tree
        db.insert(
            EMPTY_PATH,
            b"tree2",
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("successful tree2 insert");

        // Tree 3: CountSumTree - this is where we'll add our items
        db.insert(
            EMPTY_PATH,
            b"count_sum_tree",
            Element::empty_count_sum_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("successful count_sum_tree insert");

        // Insert 100 SumItems into the CountSumTree
        // Keys are random numbers 0-10 (as bytes), values are random sums 0-10
        for i in 0u32..100 {
            let key_num: u8 = rng.random_range(0..=10);
            let sum_value: i64 = rng.random_range(0..=10);

            // Create a unique key by combining the random number with the index
            let mut key = vec![key_num];
            key.extend_from_slice(&i.to_be_bytes());

            db.insert(
                &[b"count_sum_tree"],
                &key,
                Element::new_sum_item(sum_value),
                None,
                None,
                grove_version,
            )
            .unwrap()
            .expect("successful sum_item insert");
        }

        // Now test the trunk proof
        // Use max_depth=4 to test chunking (tree_depth is ~7 for 100 elements)
        let query = PathTrunkChunkQuery::new(vec![b"count_sum_tree".to_vec()], 4);

        // Generate the trunk proof
        let proof = db
            .prove_trunk_chunk(&query, grove_version)
            .unwrap()
            .expect("successful trunk proof generation");

        // Verify the trunk proof
        let (root_hash, result) = GroveDb::verify_trunk_chunk_proof(&proof, &query, grove_version)
            .expect("successful trunk proof verification");

        // Verify we got a valid root hash
        assert_ne!(root_hash, [0u8; 32], "root hash should not be all zeros");

        // Verify we got elements back
        assert!(!result.elements.is_empty(), "should have elements");

        // Verify chunk_depths is calculated correctly
        // tree_depth=9 with max_depth=4 should give [3, 3, 3]
        // (100 elements: N(9)=88 ≤ 100 < 143=N(10), so max height = 9)
        assert_eq!(
            result.max_tree_depth, 9,
            "tree depth should be 9 for 100 elements"
        );
        assert_eq!(
            result.chunk_depths,
            vec![3, 3, 3],
            "chunk depths should be [3, 3, 3] for tree_depth=9, max_depth=4"
        );

        // Verify we have the expected number of elements in the first chunk
        // First chunk has 3 levels, which should have up to 2^3-1=7 nodes
        assert!(
            result.elements.len() >= 4 && result.elements.len() <= 7,
            "should have 4-7 elements in first 3 levels, got {}",
            result.elements.len()
        );

        // Verify we have leaf keys (nodes at the truncation boundary)
        // These are keys whose children are Hash nodes
        assert!(
            !result.leaf_keys.is_empty(),
            "should have leaf keys for truncated tree"
        );

        // All elements should be SumItems
        for (key, element) in &result.elements {
            assert!(
                matches!(element, Element::SumItem(..)),
                "element at key {:?} should be SumItem, got {:?}",
                key,
                element
            );
        }

        // Verify that the lowest layer proof only contains KV and Hash nodes
        // (not KVValueHashFeatureType, KVValueHash, etc.)
        // This confirms that create_chunk uses correct node types for GroveDB elements
        let config = bincode::config::standard()
            .with_big_endian()
            .with_no_limit();
        let decoded_proof: GroveDBProof = bincode::decode_from_slice(&proof, config)
            .expect("should decode proof")
            .0;

        let GroveDBProof::V0(proof_v0) = decoded_proof;

        // Get the lowest layer proof (the count_sum_tree merk proof)
        let lowest_layer = proof_v0
            .root_layer
            .lower_layers
            .get(b"count_sum_tree".as_slice())
            .expect("should have count_sum_tree layer");

        // Decode and check the merk proof ops
        let ops: Vec<Op> = Decoder::new(&lowest_layer.merk_proof)
            .collect::<Result<Vec<_>, _>>()
            .expect("should decode merk proof");

        let mut kv_count = 0;
        let mut hash_count = 0;
        for op in &ops {
            if let Op::Push(node) = op {
                match node {
                    Node::KV(..) => kv_count += 1,
                    Node::Hash(..) => hash_count += 1,
                    other => panic!(
                        "Expected only KV or Hash nodes in trunk proof for CountSumTree with \
                         SumItems, but found {:?}. This indicates create_chunk is not using \
                         correct node types.",
                        other
                    ),
                }
            }
        }

        // Verify we have the expected KV nodes (elements) and Hash nodes (truncated
        // children) With first_chunk_depth=3: 2^3-1=7 KV nodes, 2^3=8 Hash
        // nodes
        assert_eq!(
            kv_count, 7,
            "should have 7 KV nodes for SumItems in CountSumTree (depth 3)"
        );
        assert_eq!(
            hash_count, 8,
            "should have 8 Hash nodes for truncated children at depth boundary"
        );
    }

    #[test]
    fn test_trunk_proof_full_tree_no_truncation() {
        let grove_version = GroveVersion::latest();
        let db = make_empty_grovedb();

        // Use a seeded RNG for reproducibility
        let mut rng = StdRng::seed_from_u64(12345);

        // Insert CountSumTree at root
        db.insert(
            EMPTY_PATH,
            b"count_sum_tree",
            Element::empty_count_sum_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("successful count_sum_tree insert");

        // Insert 100 ItemWithSumItems into the CountSumTree
        // Use random 32-byte keys (hash of index)
        for i in 0u32..100 {
            let mut hasher = Hasher::new();
            hasher.update(&i.to_be_bytes());
            let key: [u8; 32] = *hasher.finalize().as_bytes();
            let sum_value: i64 = rng.random_range(0..=10);
            let item_value: Vec<u8> = vec![i as u8; 10];

            db.insert(
                &[b"count_sum_tree"],
                &key,
                Element::new_item_with_sum_item(item_value, sum_value),
                None,
                None,
                grove_version,
            )
            .unwrap()
            .expect("successful item_with_sum insert");
        }

        // Use max_depth equal to the max AVL height for 100 elements
        // This should return all elements with no truncation
        let max_depth = calculate_max_tree_depth_from_count(100);
        let query = PathTrunkChunkQuery::new(vec![b"count_sum_tree".to_vec()], max_depth);

        // Generate the trunk proof
        let proof = db
            .prove_trunk_chunk(&query, grove_version)
            .unwrap()
            .expect("successful trunk proof generation");

        // Verify the trunk proof
        let (root_hash, result) = GroveDb::verify_trunk_chunk_proof(&proof, &query, grove_version)
            .expect("successful trunk proof verification");

        // Verify we got a valid root hash
        assert_ne!(root_hash, [0u8; 32], "root hash should not be all zeros");

        // With max_depth = max AVL height, we should get all 100 elements
        assert_eq!(
            result.elements.len(),
            100,
            "should have all 100 elements when max_depth >= tree_depth"
        );

        // tree_depth should match the calculated max AVL height
        assert_eq!(
            result.max_tree_depth, max_depth,
            "tree depth should match calculated max AVL height for 100 elements"
        );
        assert_eq!(
            result.chunk_depths,
            vec![max_depth],
            "chunk depths should be [max_depth] when max_depth == tree_depth"
        );

        // No leaf keys since there's no truncation
        assert!(
            result.leaf_keys.is_empty(),
            "should have no leaf keys when entire tree is returned"
        );

        // All elements should be ItemWithSumItem
        for (key, element) in &result.elements {
            assert!(
                matches!(element, Element::ItemWithSumItem(..)),
                "element at key {:?} should be ItemWithSumItem, got {:?}",
                key,
                element
            );
        }
    }

    #[test]
    fn test_trunk_proof_full_tree_some_truncation() {
        use grovedb_merk::proofs::branch::depth::calculate_chunk_depths;

        let grove_version = GroveVersion::latest();
        let db = make_empty_grovedb();

        // Use a seeded RNG for reproducibility
        let mut rng = StdRng::seed_from_u64(12345);

        // Insert CountSumTree at root
        db.insert(
            EMPTY_PATH,
            b"count_sum_tree",
            Element::empty_count_sum_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("successful count_sum_tree insert");

        // Insert 100 ItemWithSumItems into the CountSumTree
        // Use random 32-byte keys (hash of index)
        for i in 0u32..100 {
            let mut hasher = Hasher::new();
            hasher.update(&i.to_be_bytes());
            let key: [u8; 32] = *hasher.finalize().as_bytes();
            let sum_value: i64 = rng.random_range(0..=10);
            let item_value: Vec<u8> = vec![i as u8; 10];

            db.insert(
                &[b"count_sum_tree"],
                &key,
                Element::new_item_with_sum_item(item_value, sum_value),
                None,
                None,
                grove_version,
            )
            .unwrap()
            .expect("successful item_with_sum insert");
        }

        // Use max_depth=7, which is less than the max AVL height (9) for 100 elements
        // This should result in truncation
        let max_depth: u8 = 7;
        let tree_depth = calculate_max_tree_depth_from_count(100);
        let expected_chunk_depths = calculate_chunk_depths(tree_depth, max_depth);

        let query = PathTrunkChunkQuery::new(vec![b"count_sum_tree".to_vec()], max_depth);

        // Generate the trunk proof
        let proof = db
            .prove_trunk_chunk(&query, grove_version)
            .unwrap()
            .expect("successful trunk proof generation");

        // Verify the trunk proof
        let (root_hash, result) = GroveDb::verify_trunk_chunk_proof(&proof, &query, grove_version)
            .expect("successful trunk proof verification");

        // Verify we got a valid root hash
        assert_ne!(root_hash, [0u8; 32], "root hash should not be all zeros");

        // tree_depth should be 9 (max AVL height for 100 elements)
        assert_eq!(
            result.max_tree_depth, tree_depth,
            "tree depth should match calculated max AVL height"
        );

        // chunk_depths should be [5, 4] for tree_depth=9, max_depth=7
        assert_eq!(
            result.chunk_depths, expected_chunk_depths,
            "chunk depths should split evenly"
        );

        // First chunk has depth 5, so we should get 2^5-1=31 elements
        let first_chunk_depth = expected_chunk_depths[0];
        let expected_elements = (1usize << first_chunk_depth) - 1;
        assert_eq!(
            result.elements.len(),
            expected_elements,
            "should have {} elements in first chunk of depth {}",
            expected_elements,
            first_chunk_depth
        );

        // Should have leaf keys since there's truncation
        assert!(
            !result.leaf_keys.is_empty(),
            "should have leaf keys when tree is truncated"
        );

        // All elements should be ItemWithSumItem
        for (key, element) in &result.elements {
            assert!(
                matches!(element, Element::ItemWithSumItem(..)),
                "element at key {:?} should be ItemWithSumItem, got {:?}",
                key,
                element
            );
        }
    }
}
