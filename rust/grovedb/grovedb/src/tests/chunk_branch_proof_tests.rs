//! Chunk branch proof tests

#[cfg(test)]
mod tests {
    use blake3::Hasher;
    use grovedb_merk::proofs::branch::depth::{
        calculate_chunk_depths, calculate_max_tree_depth_from_count,
    };
    use grovedb_version::version::GroveVersion;
    use rand::{rngs::StdRng, Rng, SeedableRng};

    use crate::{
        query::PathTrunkChunkQuery,
        tests::{common::EMPTY_PATH, make_empty_grovedb},
        Element, GroveDb,
    };

    #[test]
    fn test_branch_proof_after_trunk() {
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

        // First, get the trunk proof with max_depth=5
        // tree_depth=9 for 100 elements, so chunk_depths=[5, 4]
        let max_depth: u8 = 5;
        let tree_depth = calculate_max_tree_depth_from_count(100);
        let chunk_depths = calculate_chunk_depths(tree_depth, max_depth);

        assert_eq!(chunk_depths, vec![5, 4], "chunk depths should be [5, 4]");

        let trunk_query = PathTrunkChunkQuery::new(vec![b"count_sum_tree".to_vec()], max_depth);

        // Generate the trunk proof
        let trunk_proof = db
            .prove_trunk_chunk(&trunk_query, grove_version)
            .unwrap()
            .expect("successful trunk proof generation");

        // Verify the trunk proof and get leaf keys
        let (root_hash, trunk_result) =
            GroveDb::verify_trunk_chunk_proof(&trunk_proof, &trunk_query, grove_version)
                .expect("successful trunk proof verification");

        assert_ne!(root_hash, [0u8; 32], "root hash should not be all zeros");

        // Trunk should have 2^5-1=31 elements
        assert_eq!(
            trunk_result.elements.len(),
            31,
            "trunk should have 31 elements"
        );

        // Should have leaf keys for the next level of chunks
        assert!(
            !trunk_result.leaf_keys.is_empty(),
            "should have leaf keys for branch queries"
        );

        // The leaf keys are the keys we can use for branch queries
        // Each leaf key represents a subtree that was truncated
        let leaf_keys = &trunk_result.leaf_keys;

        // For a tree of depth 5, we should have 2^5=32 leaf positions
        // (though not all may be filled depending on tree structure)
        assert!(
            leaf_keys.len() <= 32,
            "should have at most 32 leaf keys, got {}",
            leaf_keys.len()
        );

        // Now query each branch using the leaf keys and their expected hashes
        let mut total_elements_from_branches = 0;
        let remaining_depth = chunk_depths[1]; // Should be 4

        for (leaf_key, leaf_info) in leaf_keys {
            use crate::query::PathBranchChunkQuery;

            let branch_query = PathBranchChunkQuery::new(
                vec![b"count_sum_tree".to_vec()],
                leaf_key.clone(),
                remaining_depth,
            );

            let branch_proof = db
                .prove_branch_chunk(&branch_query, grove_version)
                .unwrap()
                .expect("successful branch proof generation");

            // Pass the expected hash from the trunk proof's leaf_keys
            let branch_result = GroveDb::verify_branch_chunk_proof(
                &branch_proof,
                &branch_query,
                leaf_info.hash,
                grove_version,
            )
            .expect("successful branch proof verification");

            // Branch should have a valid root hash that matches the expected hash
            assert_eq!(
                branch_result.branch_root_hash, leaf_info.hash,
                "branch root hash should match expected hash from trunk"
            );

            // Branch should have elements
            assert!(
                !branch_result.elements.is_empty(),
                "branch at key {:?} should have elements",
                leaf_key
            );

            println!(
                "Branch {}: {} elements",
                hex::encode(&leaf_key[..4]),
                branch_result.elements.len()
            );

            total_elements_from_branches += branch_result.elements.len();
        }

        // Total elements from trunk + all branches - overlap should equal 100
        // The overlap is because each branch's root node is also counted as a leaf key
        // in the trunk
        let overlap = leaf_keys.len();
        let total_elements = trunk_result.elements.len() + total_elements_from_branches - overlap;
        assert_eq!(
            total_elements,
            100,
            "trunk ({}) + branches ({}) - overlap ({}) should equal 100 total elements",
            trunk_result.elements.len(),
            total_elements_from_branches,
            overlap
        );
    }

    #[test]
    fn test_branch_proof_after_trunk_provable_count_sum_tree() {
        let grove_version = GroveVersion::latest();
        let db = make_empty_grovedb();

        // Use a seeded RNG for reproducibility
        let mut rng = StdRng::seed_from_u64(12345);

        // First insert a subtree at root level to test one level deep
        db.insert(
            EMPTY_PATH,
            b"data",
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("successful data subtree insert");

        // Insert ProvableCountSumTree one level deep (under "data")
        // ProvableCountSumTree: COUNT is in the hash, SUM is tracked but not in hash
        db.insert(
            &[b"data"],
            b"provable_count_sum_tree",
            Element::empty_provable_count_sum_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("successful provable_count_sum_tree insert");

        // Insert 100 SumItems into the ProvableCountSumTree
        for i in 0u32..100 {
            let mut hasher = Hasher::new();
            hasher.update(&i.to_be_bytes());
            let key: [u8; 32] = *hasher.finalize().as_bytes();
            let sum_value: i64 = rng.random_range(0..=10);

            db.insert(
                &[b"data".as_slice(), b"provable_count_sum_tree".as_slice()],
                &key,
                Element::new_sum_item(sum_value),
                None,
                None,
                grove_version,
            )
            .unwrap()
            .expect("successful sum_item insert");
        }

        // First, get the trunk proof with max_depth=5
        // tree_depth=9 for 100 elements, so chunk_depths=[5, 4]
        let max_depth: u8 = 5;
        let tree_depth = calculate_max_tree_depth_from_count(100);
        let chunk_depths = calculate_chunk_depths(tree_depth, max_depth);

        assert_eq!(chunk_depths, vec![5, 4], "chunk depths should be [5, 4]");

        // Path is now two levels deep: ["data", "provable_count_sum_tree"]
        let trunk_query = PathTrunkChunkQuery::new(
            vec![b"data".to_vec(), b"provable_count_sum_tree".to_vec()],
            max_depth,
        );

        // Generate the trunk proof
        let trunk_proof = db
            .prove_trunk_chunk(&trunk_query, grove_version)
            .unwrap()
            .expect("successful trunk proof generation");

        // Verify the trunk proof and get leaf keys
        let (root_hash, trunk_result) =
            GroveDb::verify_trunk_chunk_proof(&trunk_proof, &trunk_query, grove_version)
                .expect("successful trunk proof verification");

        assert_ne!(root_hash, [0u8; 32], "root hash should not be all zeros");

        // Trunk should have 2^5-1=31 elements
        assert_eq!(
            trunk_result.elements.len(),
            31,
            "trunk should have 31 elements"
        );

        // Should have leaf keys for the next level of chunks
        assert!(
            !trunk_result.leaf_keys.is_empty(),
            "should have leaf keys for branch queries"
        );

        // The leaf keys are the keys we can use for branch queries
        // Each leaf key represents a subtree that was truncated
        let leaf_keys = &trunk_result.leaf_keys;

        // For a tree of depth 5, we should have 2^5=32 leaf positions
        // (though not all may be filled depending on tree structure)
        assert!(
            leaf_keys.len() <= 32,
            "should have at most 32 leaf keys, got {}",
            leaf_keys.len()
        );

        // Now query each branch using the leaf keys and their expected hashes
        let mut total_elements_from_branches = 0;
        let remaining_depth = chunk_depths[1]; // Should be 4

        for (leaf_key, leaf_info) in leaf_keys {
            use crate::query::PathBranchChunkQuery;

            // Path is now two levels deep for branch queries too
            let branch_query = PathBranchChunkQuery::new(
                vec![b"data".to_vec(), b"provable_count_sum_tree".to_vec()],
                leaf_key.clone(),
                remaining_depth,
            );

            let branch_proof = db
                .prove_branch_chunk(&branch_query, grove_version)
                .unwrap()
                .expect("successful branch proof generation");

            // Pass the expected hash from the trunk proof's leaf_keys
            let branch_result = GroveDb::verify_branch_chunk_proof(
                &branch_proof,
                &branch_query,
                leaf_info.hash,
                grove_version,
            )
            .expect("successful branch proof verification");

            // Branch should have a valid root hash that matches the expected hash
            assert_eq!(
                branch_result.branch_root_hash, leaf_info.hash,
                "branch root hash should match expected hash from trunk"
            );

            // Branch should have elements
            assert!(
                !branch_result.elements.is_empty(),
                "branch at key {:?} should have elements",
                leaf_key
            );

            println!(
                "Branch {}: {} elements",
                hex::encode(&leaf_key[..4]),
                branch_result.elements.len()
            );

            total_elements_from_branches += branch_result.elements.len();
        }

        // Total elements from trunk + all branches - overlap should equal 100
        // The overlap is because each branch's root node is also counted as a leaf key
        // in the trunk
        let overlap = leaf_keys.len();
        let total_elements = trunk_result.elements.len() + total_elements_from_branches - overlap;
        assert_eq!(
            total_elements,
            100,
            "trunk ({}) + branches ({}) - overlap ({}) should equal 100 total elements",
            trunk_result.elements.len(),
            total_elements_from_branches,
            overlap
        );
    }
}
