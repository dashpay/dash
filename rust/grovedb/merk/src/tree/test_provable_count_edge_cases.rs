//! Edge case tests for ProvableCountTree functionality

#[cfg(test)]
mod tests {
    use grovedb_version::version::GroveVersion;

    use crate::{
        tree::{AggregateData, TreeFeatureType, TreeNode},
        tree_type::TreeType,
    };

    #[test]
    fn test_provable_count_tree_zero_count() {
        let _grove_version = GroveVersion::latest();

        // Create a tree with zero count
        let tree = TreeNode::new(
            vec![1, 2, 3],
            vec![4, 5, 6],
            None,
            TreeFeatureType::ProvableCountedMerkNode(0),
        )
        .unwrap();

        // Hash should still be deterministic even with zero count
        let hash1 = tree.hash_for_link(TreeType::ProvableCountTree).unwrap();
        let hash2 = tree.hash_for_link(TreeType::ProvableCountTree).unwrap();

        assert_eq!(hash1, hash2, "Zero count hash should be deterministic");
    }

    #[test]
    fn test_provable_count_tree_max_count() {
        let _grove_version = GroveVersion::latest();

        // Create a tree with maximum u64 count
        let max_count = u64::MAX;
        let tree = TreeNode::new(
            vec![1, 2, 3],
            vec![4, 5, 6],
            None,
            TreeFeatureType::ProvableCountedMerkNode(max_count),
        )
        .unwrap();

        // Should handle max count without overflow
        let hash = tree.hash_for_link(TreeType::ProvableCountTree).unwrap();
        assert_eq!(hash.len(), 32, "Hash should be 32 bytes");

        // Aggregate data should preserve the max count
        let aggregate_data = tree.aggregate_data().unwrap();
        match aggregate_data {
            AggregateData::ProvableCount(count) => {
                assert_eq!(count, max_count, "Max count should be preserved");
            }
            _ => panic!("Expected ProvableCount aggregate data"),
        }
    }

    #[test]
    fn test_provable_count_tree_with_children() {
        let _grove_version = GroveVersion::latest();

        // Create parent with count 10
        let parent = TreeNode::new(
            vec![5],
            vec![50],
            None,
            TreeFeatureType::ProvableCountedMerkNode(10),
        )
        .unwrap();

        // Create left child with count 4
        let left_child = TreeNode::new(
            vec![3],
            vec![30],
            None,
            TreeFeatureType::ProvableCountedMerkNode(4),
        )
        .unwrap();

        // Create right child with count 5
        let right_child = TreeNode::new(
            vec![7],
            vec![70],
            None,
            TreeFeatureType::ProvableCountedMerkNode(5),
        )
        .unwrap();

        // Calculate hashes - each should be different
        let parent_hash = parent.hash_for_link(TreeType::ProvableCountTree).unwrap();
        let left_hash = left_child
            .hash_for_link(TreeType::ProvableCountTree)
            .unwrap();
        let right_hash = right_child
            .hash_for_link(TreeType::ProvableCountTree)
            .unwrap();

        assert_ne!(
            parent_hash, left_hash,
            "Parent and left child hashes should differ"
        );
        assert_ne!(
            parent_hash, right_hash,
            "Parent and right child hashes should differ"
        );
        assert_ne!(
            left_hash, right_hash,
            "Left and right child hashes should differ"
        );
    }

    #[test]
    fn test_provable_count_tree_serialization() {
        let _grove_version = GroveVersion::latest();

        // Create a tree node with specific count
        let count_value = 42u64;
        let tree = TreeNode::new(
            vec![10, 20, 30],
            vec![40, 50, 60],
            None,
            TreeFeatureType::ProvableCountedMerkNode(count_value),
        )
        .unwrap();

        // Get the aggregate data
        let aggregate_data = tree.aggregate_data().unwrap();

        // Verify it's the correct type
        match aggregate_data {
            AggregateData::ProvableCount(count) => {
                assert_eq!(count, count_value, "Count should be preserved");
            }
            _ => panic!("Expected ProvableCount aggregate data"),
        }
    }

    #[test]
    fn test_provable_count_vs_regular_count_hash_difference() {
        let _grove_version = GroveVersion::latest();
        let key = vec![1, 2, 3];
        let value = vec![4, 5, 6];
        let count = 10u64;

        // Create regular count tree node
        let regular_count = TreeNode::new(
            key.clone(),
            value.clone(),
            None,
            TreeFeatureType::CountedMerkNode(count),
        )
        .unwrap();

        // Create provable count tree node with same data
        let provable_count = TreeNode::new(
            key.clone(),
            value.clone(),
            None,
            TreeFeatureType::ProvableCountedMerkNode(count),
        )
        .unwrap();

        // Get hashes
        let regular_hash = regular_count.hash_for_link(TreeType::CountTree).unwrap();
        let provable_hash = provable_count
            .hash_for_link(TreeType::ProvableCountTree)
            .unwrap();

        // Hashes should be different even with same key/value/count
        assert_ne!(
            regular_hash, provable_hash,
            "Regular CountTree and ProvableCountTree should have different hashes"
        );
    }

    #[test]
    fn test_provable_count_tree_empty_key_value() {
        let _grove_version = GroveVersion::latest();

        // Test with empty key
        let tree1 = TreeNode::new(
            vec![],
            vec![1, 2, 3],
            None,
            TreeFeatureType::ProvableCountedMerkNode(5),
        )
        .unwrap();

        // Test with empty value
        let tree2 = TreeNode::new(
            vec![1, 2, 3],
            vec![],
            None,
            TreeFeatureType::ProvableCountedMerkNode(5),
        )
        .unwrap();

        // Test with both empty
        let tree3 = TreeNode::new(
            vec![],
            vec![],
            None,
            TreeFeatureType::ProvableCountedMerkNode(5),
        )
        .unwrap();

        // All should produce valid hashes
        let hash1 = tree1.hash_for_link(TreeType::ProvableCountTree).unwrap();
        let hash2 = tree2.hash_for_link(TreeType::ProvableCountTree).unwrap();
        let hash3 = tree3.hash_for_link(TreeType::ProvableCountTree).unwrap();

        // All hashes should be different
        assert_ne!(
            hash1, hash2,
            "Empty key vs empty value should have different hashes"
        );
        assert_ne!(
            hash1, hash3,
            "Empty key vs both empty should have different hashes"
        );
        assert_ne!(
            hash2, hash3,
            "Empty value vs both empty should have different hashes"
        );
    }

    #[test]
    fn test_provable_count_tree_count_overflow_protection() {
        let _grove_version = GroveVersion::latest();

        // Create nodes with counts that would overflow if added
        let count1 = u64::MAX - 100;
        let count2 = 200;

        let tree1 = TreeNode::new(
            vec![1],
            vec![10],
            None,
            TreeFeatureType::ProvableCountedMerkNode(count1),
        )
        .unwrap();

        let tree2 = TreeNode::new(
            vec![2],
            vec![20],
            None,
            TreeFeatureType::ProvableCountedMerkNode(count2),
        )
        .unwrap();

        // Both should handle their large counts correctly
        let hash1 = tree1.hash_for_link(TreeType::ProvableCountTree).unwrap();
        let hash2 = tree2.hash_for_link(TreeType::ProvableCountTree).unwrap();

        assert_ne!(
            hash1, hash2,
            "Different counts should produce different hashes"
        );
    }

    #[test]
    fn test_provable_count_tree_incremental_count_changes() {
        let _grove_version = GroveVersion::latest();
        let key = vec![5, 5, 5];
        let value = vec![10, 10, 10];

        let mut hashes = Vec::new();

        // Create trees with counts from 0 to 10
        for count in 0..=10 {
            let tree = TreeNode::new(
                key.clone(),
                value.clone(),
                None,
                TreeFeatureType::ProvableCountedMerkNode(count),
            )
            .unwrap();

            let hash = tree.hash_for_link(TreeType::ProvableCountTree).unwrap();
            hashes.push(hash);
        }

        // All hashes should be unique
        for i in 0..hashes.len() {
            for j in (i + 1)..hashes.len() {
                assert_ne!(
                    hashes[i], hashes[j],
                    "Count {} and count {} should have different hashes",
                    i, j
                );
            }
        }
    }

    #[test]
    fn test_provable_count_tree_type_conversion() {
        // Test TreeType to/from u8
        let tree_type = TreeType::ProvableCountTree;
        assert_eq!(tree_type as u8, 5);

        let from_u8 = TreeType::try_from(5u8).unwrap();
        assert_eq!(from_u8, TreeType::ProvableCountTree);

        // Test invalid conversion
        let invalid = TreeType::try_from(100u8);
        assert!(invalid.is_err());
    }

    #[test]
    fn test_provable_count_tree_feature_type_display() {
        let feature = TreeFeatureType::ProvableCountedMerkNode(42);
        let debug_str = format!("{:?}", feature);
        assert!(
            debug_str.contains("42"),
            "Debug output should contain count value"
        );
    }
}
