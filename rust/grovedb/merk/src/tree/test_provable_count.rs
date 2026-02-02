//! Tests for ProvableCountTree functionality

#[cfg(test)]
mod tests {
    use grovedb_costs::OperationCost;
    use grovedb_version::version::GroveVersion;

    use crate::{
        tree::{AggregateData, TreeFeatureType, TreeNode},
        tree_type::TreeType,
    };

    #[test]
    fn test_provable_count_tree_hash_includes_count() {
        let _grove_version = GroveVersion::latest();

        // Create two trees with the same key-value pairs but different counts
        let tree1 = TreeNode::new(
            vec![1, 2, 3],
            vec![4, 5, 6],
            None,
            TreeFeatureType::ProvableCountedMerkNode(10),
        )
        .unwrap();

        let tree2 = TreeNode::new(
            vec![1, 2, 3],
            vec![4, 5, 6],
            None,
            TreeFeatureType::ProvableCountedMerkNode(20),
        )
        .unwrap();

        // Calculate hashes for both trees
        let hash1 = tree1.hash_for_link(TreeType::ProvableCountTree).unwrap();
        let hash2 = tree2.hash_for_link(TreeType::ProvableCountTree).unwrap();

        // The hashes should be different because the counts are different
        assert_ne!(hash1, hash2, "Hashes should differ when counts differ");

        // Create a regular CountTree with the same key-value
        let tree3 = TreeNode::new(
            vec![1, 2, 3],
            vec![4, 5, 6],
            None,
            TreeFeatureType::CountedMerkNode(10),
        )
        .unwrap();

        let hash3 = tree3.hash_for_link(TreeType::CountTree).unwrap();

        // The hash of a regular CountTree should differ from ProvableCountTree
        assert_ne!(
            hash1, hash3,
            "ProvableCountTree hash should differ from CountTree hash"
        );
    }

    #[test]
    fn test_aggregate_data_conversion() {
        // Test that ProvableCountedMerkNode converts to ProvableCount aggregate data
        let feature_type = TreeFeatureType::ProvableCountedMerkNode(42);
        let aggregate_data: AggregateData = feature_type.into();

        assert!(matches!(aggregate_data, AggregateData::ProvableCount(42)));
    }

    #[test]
    fn test_tree_type_conversions() {
        // Test TreeType to TreeFeatureType conversion
        let tree_type = TreeType::ProvableCountTree;
        let feature_type = tree_type.empty_tree_feature_type();

        assert!(matches!(
            feature_type,
            TreeFeatureType::ProvableCountedMerkNode(0)
        ));

        // Test TreeType to node type
        let node_type = tree_type.inner_node_type();
        assert_eq!(node_type as u8, 5); // ProvableCountNode = 5

        // Test TreeType allows sum items
        assert!(!tree_type.allows_sum_item());
    }

    #[test]
    fn test_aggregate_count_calculation() {
        let _grove_version = GroveVersion::latest();
        let _cost = OperationCost::default();

        // Create a tree with ProvableCountedMerkNode
        let tree = TreeNode::new(
            vec![5],
            vec![10],
            None,
            TreeFeatureType::ProvableCountedMerkNode(1),
        )
        .unwrap();

        // Simulate having children with counts
        // In a real scenario, these would be loaded from storage
        // For testing, we'll use the aggregate_data method directly

        let aggregate_data = tree.aggregate_data().unwrap();

        // Should return ProvableCount with the node's own count (no children yet)
        match aggregate_data {
            AggregateData::ProvableCount(count) => {
                assert_eq!(count, 1, "Count should be 1 for a single node");
            }
            _ => panic!("Expected ProvableCount aggregate data"),
        }
    }
}
