use grovedb_element::{Element, ElementFlags};

use crate::{
    Error, MaybeTree, TreeFeatureType,
    TreeFeatureType::{
        BasicMerkNode, BigSummedMerkNode, CountedMerkNode, CountedSummedMerkNode, SummedMerkNode,
    },
    TreeType,
};

pub trait ElementTreeTypeExtensions {
    /// Check if the element is a tree and return the root_tree info and tree
    /// type
    fn root_key_and_tree_type_owned(self) -> Option<(Option<Vec<u8>>, TreeType)>;

    /// Check if the element is a tree and return the root_tree info and the
    /// tree type
    fn root_key_and_tree_type(&self) -> Option<(&Option<Vec<u8>>, TreeType)>;

    /// Check if the element is a tree and return the flags and the tree type
    fn tree_flags_and_type(&self) -> Option<(&Option<ElementFlags>, TreeType)>;

    /// Check if the element is a tree and return the tree type
    fn tree_type(&self) -> Option<TreeType>;

    /// Check if the element is a tree and return the aggregate of elements in
    /// the tree
    fn tree_feature_type(&self) -> Option<TreeFeatureType>;

    /// Check if the element is a tree and return the tree type
    fn maybe_tree_type(&self) -> MaybeTree;

    /// Get the tree feature type
    fn get_feature_type(&self, parent_tree_type: TreeType) -> Result<TreeFeatureType, Error>;
}
impl ElementTreeTypeExtensions for Element {
    /// Check if the element is a tree and return the root_tree info and tree
    /// type
    fn root_key_and_tree_type_owned(self) -> Option<(Option<Vec<u8>>, TreeType)> {
        match self {
            Element::Tree(root_key, _) => Some((root_key, TreeType::NormalTree)),
            Element::SumTree(root_key, ..) => Some((root_key, TreeType::SumTree)),
            Element::BigSumTree(root_key, ..) => Some((root_key, TreeType::BigSumTree)),
            Element::CountTree(root_key, ..) => Some((root_key, TreeType::CountTree)),
            Element::CountSumTree(root_key, ..) => Some((root_key, TreeType::CountSumTree)),
            Element::ProvableCountTree(root_key, ..) => {
                Some((root_key, TreeType::ProvableCountTree))
            }
            Element::ProvableCountSumTree(root_key, ..) => {
                Some((root_key, TreeType::ProvableCountSumTree))
            }
            _ => None,
        }
    }

    /// Check if the element is a tree and return the root_tree info and the
    /// tree type
    fn root_key_and_tree_type(&self) -> Option<(&Option<Vec<u8>>, TreeType)> {
        match self {
            Element::Tree(root_key, _) => Some((root_key, TreeType::NormalTree)),
            Element::SumTree(root_key, ..) => Some((root_key, TreeType::SumTree)),
            Element::BigSumTree(root_key, ..) => Some((root_key, TreeType::BigSumTree)),
            Element::CountTree(root_key, ..) => Some((root_key, TreeType::CountTree)),
            Element::CountSumTree(root_key, ..) => Some((root_key, TreeType::CountSumTree)),
            Element::ProvableCountTree(root_key, ..) => {
                Some((root_key, TreeType::ProvableCountTree))
            }
            Element::ProvableCountSumTree(root_key, ..) => {
                Some((root_key, TreeType::ProvableCountSumTree))
            }
            _ => None,
        }
    }

    /// Check if the element is a tree and return the flags and the tree type
    fn tree_flags_and_type(&self) -> Option<(&Option<ElementFlags>, TreeType)> {
        match self {
            Element::Tree(_, flags) => Some((flags, TreeType::NormalTree)),
            Element::SumTree(_, _, flags) => Some((flags, TreeType::SumTree)),
            Element::BigSumTree(_, _, flags) => Some((flags, TreeType::BigSumTree)),
            Element::CountTree(_, _, flags) => Some((flags, TreeType::CountTree)),
            Element::CountSumTree(.., flags) => Some((flags, TreeType::CountSumTree)),
            Element::ProvableCountTree(_, _, flags) => Some((flags, TreeType::ProvableCountTree)),
            Element::ProvableCountSumTree(.., flags) => {
                Some((flags, TreeType::ProvableCountSumTree))
            }
            _ => None,
        }
    }

    /// Check if the element is a tree and return the tree type
    fn tree_type(&self) -> Option<TreeType> {
        match self {
            Element::Tree(..) => Some(TreeType::NormalTree),
            Element::SumTree(..) => Some(TreeType::SumTree),
            Element::BigSumTree(..) => Some(TreeType::BigSumTree),
            Element::CountTree(..) => Some(TreeType::CountTree),
            Element::CountSumTree(..) => Some(TreeType::CountSumTree),
            Element::ProvableCountTree(..) => Some(TreeType::ProvableCountTree),
            Element::ProvableCountSumTree(..) => Some(TreeType::ProvableCountSumTree),
            _ => None,
        }
    }

    /// Check if the element is a tree and return the aggregate of elements in
    /// the tree
    fn tree_feature_type(&self) -> Option<TreeFeatureType> {
        match self {
            Element::Tree(..) => Some(BasicMerkNode),
            Element::SumTree(_, value, _) => Some(SummedMerkNode(*value)),
            Element::BigSumTree(_, value, _) => Some(BigSummedMerkNode(*value)),
            Element::CountTree(_, value, _) => Some(CountedMerkNode(*value)),
            Element::CountSumTree(_, count, sum, _) => Some(CountedSummedMerkNode(*count, *sum)),
            Element::ProvableCountTree(_, value, _) => {
                Some(TreeFeatureType::ProvableCountedMerkNode(*value))
            }
            Element::ProvableCountSumTree(_, count, sum, _) => {
                Some(TreeFeatureType::ProvableCountedSummedMerkNode(*count, *sum))
            }
            _ => None,
        }
    }

    /// Check if the element is a tree and return the tree type
    fn maybe_tree_type(&self) -> MaybeTree {
        match self {
            Element::Tree(..) => MaybeTree::Tree(TreeType::NormalTree),
            Element::SumTree(..) => MaybeTree::Tree(TreeType::SumTree),
            Element::BigSumTree(..) => MaybeTree::Tree(TreeType::BigSumTree),
            Element::CountTree(..) => MaybeTree::Tree(TreeType::CountTree),
            Element::CountSumTree(..) => MaybeTree::Tree(TreeType::CountSumTree),
            Element::ProvableCountTree(..) => MaybeTree::Tree(TreeType::ProvableCountTree),
            Element::ProvableCountSumTree(..) => MaybeTree::Tree(TreeType::ProvableCountSumTree),
            _ => MaybeTree::NotTree,
        }
    }

    /// Get the tree feature type
    fn get_feature_type(&self, parent_tree_type: TreeType) -> Result<TreeFeatureType, Error> {
        match parent_tree_type {
            TreeType::NormalTree => Ok(BasicMerkNode),
            TreeType::SumTree => Ok(SummedMerkNode(self.sum_value_or_default())),
            TreeType::BigSumTree => Ok(BigSummedMerkNode(self.big_sum_value_or_default())),
            TreeType::CountTree => Ok(CountedMerkNode(self.count_value_or_default())),
            TreeType::CountSumTree => {
                let v = self.count_sum_value_or_default();
                Ok(CountedSummedMerkNode(v.0, v.1))
            }
            TreeType::ProvableCountTree => Ok(TreeFeatureType::ProvableCountedMerkNode(
                self.count_value_or_default(),
            )),
            TreeType::ProvableCountSumTree => {
                let v = self.count_sum_value_or_default();
                Ok(TreeFeatureType::ProvableCountedSummedMerkNode(v.0, v.1))
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use grovedb_version::version::GroveVersion;

    use super::*;
    use crate::{
        element::costs::ElementCostExtensions,
        tree::kv::ValueDefinedCostType::SpecializedValueDefinedCost,
    };

    #[test]
    fn item_with_sum_item_helpers_cover_all_behaviors() {
        let grove_version = GroveVersion::latest();
        let flags = Some(vec![1, 2, 3]);
        let element = Element::ItemWithSumItem(b"payload".to_vec(), 42, flags.clone());

        assert!(element.is_any_item());
        assert!(element.has_basic_item());
        assert!(element.is_sum_item());
        assert!(element.is_item_with_sum_item());
        assert_eq!(element.sum_value_or_default(), 42);
        assert_eq!(element.count_sum_value_or_default(), (1, 42));
        assert_eq!(element.big_sum_value_or_default(), 42);
        assert_eq!(element.as_item_bytes().unwrap(), b"payload");
        assert_eq!(
            element.clone().into_item_bytes().unwrap(),
            b"payload".to_vec()
        );
        assert_eq!(element.as_sum_item_value().unwrap(), 42);
        assert_eq!(element.clone().into_sum_item_value().unwrap(), 42);
        assert_eq!(element.get_flags(), &flags);

        let serialized = element.serialize(grove_version).expect("serialize element");
        let deserialized =
            Element::deserialize(&serialized, grove_version).expect("deserialize element");
        assert_eq!(deserialized, element);

        let explicit_cost = element.value_defined_cost(grove_version).unwrap();
        let derived_cost =
            Element::value_defined_cost_for_serialized_value(&serialized, grove_version)
                .expect("cost for serialized element");
        match (explicit_cost, derived_cost) {
            (SpecializedValueDefinedCost(explicit), SpecializedValueDefinedCost(derived)) => {
                assert!(explicit > 0);
                assert_eq!(explicit, derived);
            }
            _ => panic!("unexpected cost type"),
        }
    }
}
