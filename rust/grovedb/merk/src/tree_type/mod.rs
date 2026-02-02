#[cfg(feature = "minimal")]
mod costs;
use std::fmt;

#[cfg(feature = "minimal")]
pub use costs::*;
use grovedb_element::ElementType;

#[cfg(feature = "minimal")]
use crate::merk::NodeType;
use crate::{Error, TreeFeatureType};

#[derive(Clone, Copy, PartialEq, Eq, Debug, Hash)]
pub enum MaybeTree {
    Tree(TreeType),
    NotTree,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug, Hash)]
pub enum TreeType {
    NormalTree = 0,
    SumTree = 1,
    BigSumTree = 2,
    CountTree = 3,
    CountSumTree = 4,
    ProvableCountTree = 5,
    ProvableCountSumTree = 6,
}

impl TryFrom<u8> for TreeType {
    type Error = Error;

    fn try_from(value: u8) -> Result<Self, Self::Error> {
        match value {
            0 => Ok(TreeType::NormalTree),
            1 => Ok(TreeType::SumTree),
            2 => Ok(TreeType::BigSumTree),
            3 => Ok(TreeType::CountTree),
            4 => Ok(TreeType::CountSumTree),
            5 => Ok(TreeType::ProvableCountTree),
            6 => Ok(TreeType::ProvableCountSumTree),
            n => Err(Error::UnknownTreeType(format!("got {}, max is 6", n))),
        }
    }
}

impl fmt::Display for TreeType {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let s = match *self {
            TreeType::NormalTree => "Normal Tree",
            TreeType::SumTree => "Sum Tree",
            TreeType::BigSumTree => "Big Sum Tree",
            TreeType::CountTree => "Count Tree",
            TreeType::CountSumTree => "Count Sum Tree",
            TreeType::ProvableCountTree => "Provable Count Tree",
            TreeType::ProvableCountSumTree => "Provable Count Sum Tree",
        };
        write!(f, "{}", s)
    }
}

impl TreeType {
    pub fn allows_sum_item(&self) -> bool {
        match self {
            TreeType::NormalTree => false,
            TreeType::SumTree => true,
            TreeType::BigSumTree => true,
            TreeType::CountTree => false,
            TreeType::CountSumTree => true,
            TreeType::ProvableCountTree => false,
            TreeType::ProvableCountSumTree => true, // allows sum items
        }
    }

    #[cfg(feature = "minimal")]
    pub const fn inner_node_type(&self) -> NodeType {
        match self {
            TreeType::NormalTree => NodeType::NormalNode,
            TreeType::SumTree => NodeType::SumNode,
            TreeType::BigSumTree => NodeType::BigSumNode,
            TreeType::CountTree => NodeType::CountNode,
            TreeType::CountSumTree => NodeType::CountSumNode,
            TreeType::ProvableCountTree => NodeType::ProvableCountNode,
            TreeType::ProvableCountSumTree => NodeType::ProvableCountSumNode,
        }
    }

    pub fn empty_tree_feature_type(&self) -> TreeFeatureType {
        match self {
            TreeType::NormalTree => TreeFeatureType::BasicMerkNode,
            TreeType::SumTree => TreeFeatureType::SummedMerkNode(0),
            TreeType::BigSumTree => TreeFeatureType::BigSummedMerkNode(0),
            TreeType::CountTree => TreeFeatureType::CountedMerkNode(0),
            TreeType::CountSumTree => TreeFeatureType::CountedSummedMerkNode(0, 0),
            TreeType::ProvableCountTree => TreeFeatureType::ProvableCountedMerkNode(0),
            TreeType::ProvableCountSumTree => TreeFeatureType::ProvableCountedSummedMerkNode(0, 0),
        }
    }

    /// Converts TreeType to the corresponding ElementType for proof generation.
    ///
    /// This is used to determine the correct proof node type based on
    /// the parent tree type. The returned ElementType is used with
    /// `ElementType::proof_node_type()` to select the appropriate
    /// proof node format.
    pub fn to_element_type(&self) -> Option<ElementType> {
        match self {
            TreeType::NormalTree => Some(ElementType::Tree),
            TreeType::SumTree => Some(ElementType::SumTree),
            TreeType::BigSumTree => Some(ElementType::BigSumTree),
            TreeType::CountTree => Some(ElementType::CountTree),
            TreeType::CountSumTree => Some(ElementType::CountSumTree),
            TreeType::ProvableCountTree => Some(ElementType::ProvableCountTree),
            TreeType::ProvableCountSumTree => Some(ElementType::ProvableCountSumTree),
        }
    }
}
