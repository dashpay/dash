//! Helpers
//! Implements helper functions in Element

use grovedb_version::{check_grovedb_v0, version::GroveVersion};
use integer_encoding::VarInt;

use crate::{
    element::{Element, ElementFlags},
    error::ElementError,
    reference_path::{path_from_reference_path_type, ReferencePathType},
};

impl Element {
    /// Decoded the integer value in the SumItem element type, returns 0 for
    /// everything else
    pub fn sum_value_or_default(&self) -> i64 {
        match self {
            Element::SumItem(sum_value, _)
            | Element::ItemWithSumItem(_, sum_value, _)
            | Element::SumTree(_, sum_value, _)
            | Element::CountSumTree(_, _, sum_value, _)
            | Element::ProvableCountSumTree(_, _, sum_value, _) => *sum_value,
            _ => 0,
        }
    }

    /// Decoded the integer value in the CountTree element type, returns 1 for
    /// everything else
    pub fn count_value_or_default(&self) -> u64 {
        match self {
            Element::CountTree(_, count_value, _)
            | Element::CountSumTree(_, count_value, ..)
            | Element::ProvableCountTree(_, count_value, _)
            | Element::ProvableCountSumTree(_, count_value, ..) => *count_value,
            _ => 1,
        }
    }

    /// Decoded the count and sum values from the element type, returns (1, 0)
    /// for elements without count/sum semantics
    pub fn count_sum_value_or_default(&self) -> (u64, i64) {
        match self {
            Element::SumItem(sum_value, _)
            | Element::ItemWithSumItem(_, sum_value, _)
            | Element::SumTree(_, sum_value, _) => (1, *sum_value),
            Element::CountTree(_, count_value, _) => (*count_value, 0),
            Element::CountSumTree(_, count_value, sum_value, _)
            | Element::ProvableCountSumTree(_, count_value, sum_value, _) => {
                (*count_value, *sum_value)
            }
            Element::ProvableCountTree(_, count_value, _) => (*count_value, 0),
            _ => (1, 0),
        }
    }

    /// Decoded the integer value in the SumItem element type, returns 0 for
    /// everything else
    pub fn big_sum_value_or_default(&self) -> i128 {
        match self {
            Element::SumItem(sum_value, _)
            | Element::ItemWithSumItem(_, sum_value, _)
            | Element::SumTree(_, sum_value, _)
            | Element::CountSumTree(_, _, sum_value, _)
            | Element::ProvableCountSumTree(_, _, sum_value, _) => *sum_value as i128,
            Element::BigSumTree(_, sum_value, _) => *sum_value,
            _ => 0,
        }
    }

    /// Decoded the integer value in the SumItem element type
    pub fn as_sum_item_value(&self) -> Result<i64, ElementError> {
        match self {
            Element::SumItem(value, _) => Ok(*value),
            Element::ItemWithSumItem(_, value, _) => Ok(*value),
            _ => Err(ElementError::WrongElementType("expected a sum item")),
        }
    }

    /// Decoded the integer value in the SumItem element type
    pub fn into_sum_item_value(self) -> Result<i64, ElementError> {
        match self {
            Element::SumItem(value, _) => Ok(value),
            Element::ItemWithSumItem(_, value, _) => Ok(value),
            _ => Err(ElementError::WrongElementType("expected a sum item")),
        }
    }

    /// Decoded the integer value in the SumTree element type
    pub fn as_sum_tree_value(&self) -> Result<i64, ElementError> {
        match self {
            Element::SumTree(_, value, _) => Ok(*value),
            _ => Err(ElementError::WrongElementType("expected a sum tree")),
        }
    }

    /// Decoded the integer value in the SumTree element type
    pub fn into_sum_tree_value(self) -> Result<i64, ElementError> {
        match self {
            Element::SumTree(_, value, _) => Ok(value),
            _ => Err(ElementError::WrongElementType("expected a sum tree")),
        }
    }

    /// Gives the item value in the Item element type
    pub fn as_item_bytes(&self) -> Result<&[u8], ElementError> {
        match self {
            Element::Item(value, _) => Ok(value),
            Element::ItemWithSumItem(value, ..) => Ok(value),
            _ => Err(ElementError::WrongElementType("expected an item")),
        }
    }

    /// Gives the item value in the Item element type
    pub fn into_item_bytes(self) -> Result<Vec<u8>, ElementError> {
        match self {
            Element::Item(value, _) => Ok(value),
            Element::ItemWithSumItem(value, ..) => Ok(value),
            _ => Err(ElementError::WrongElementType("expected an item")),
        }
    }

    /// Gives the reference path type in the Reference element type
    pub fn into_reference_path_type(self) -> Result<ReferencePathType, ElementError> {
        match self {
            Element::Reference(value, ..) => Ok(value),
            _ => Err(ElementError::WrongElementType("expected a reference")),
        }
    }

    /// Check if the element is a sum tree
    pub fn is_sum_tree(&self) -> bool {
        matches!(self, Element::SumTree(..))
    }

    /// Check if the element is a big sum tree
    pub fn is_big_sum_tree(&self) -> bool {
        matches!(self, Element::BigSumTree(..))
    }

    /// Check if the element is a tree but not a sum tree
    pub fn is_basic_tree(&self) -> bool {
        matches!(self, Element::Tree(..))
    }

    /// Check if the element is a tree
    pub fn is_any_tree(&self) -> bool {
        matches!(
            self,
            Element::SumTree(..)
                | Element::Tree(..)
                | Element::BigSumTree(..)
                | Element::CountTree(..)
                | Element::CountSumTree(..)
                | Element::ProvableCountTree(..)
                | Element::ProvableCountSumTree(..)
        )
    }

    /// Check if the element is a reference
    pub fn is_reference(&self) -> bool {
        matches!(self, Element::Reference(..))
    }

    /// Check if the element is an item
    pub fn is_any_item(&self) -> bool {
        matches!(
            self,
            Element::Item(..) | Element::SumItem(..) | Element::ItemWithSumItem(..)
        )
    }

    /// Check if the element is an item
    pub fn is_basic_item(&self) -> bool {
        matches!(self, Element::Item(..))
    }

    /// Check if the element is an item
    pub fn has_basic_item(&self) -> bool {
        matches!(self, Element::Item(..) | Element::ItemWithSumItem(..))
    }

    /// Check if the element is a sum item
    pub fn is_sum_item(&self) -> bool {
        matches!(self, Element::SumItem(..) | Element::ItemWithSumItem(..))
    }

    /// Check if the element is a sum item
    pub fn is_item_with_sum_item(&self) -> bool {
        matches!(self, Element::ItemWithSumItem(..))
    }

    /// Grab the optional flag stored in an element
    pub fn get_flags(&self) -> &Option<ElementFlags> {
        match self {
            Element::Tree(_, flags)
            | Element::Item(_, flags)
            | Element::Reference(_, _, flags)
            | Element::SumTree(.., flags)
            | Element::BigSumTree(.., flags)
            | Element::CountTree(.., flags)
            | Element::SumItem(_, flags)
            | Element::CountSumTree(.., flags)
            | Element::ProvableCountTree(.., flags)
            | Element::ProvableCountSumTree(.., flags)
            | Element::ItemWithSumItem(.., flags) => flags,
        }
    }

    /// Grab the optional flag stored in an element
    pub fn get_flags_owned(self) -> Option<ElementFlags> {
        match self {
            Element::Tree(_, flags)
            | Element::Item(_, flags)
            | Element::Reference(_, _, flags)
            | Element::SumTree(.., flags)
            | Element::BigSumTree(.., flags)
            | Element::CountTree(.., flags)
            | Element::SumItem(_, flags)
            | Element::CountSumTree(.., flags)
            | Element::ProvableCountTree(.., flags)
            | Element::ProvableCountSumTree(.., flags)
            | Element::ItemWithSumItem(.., flags) => flags,
        }
    }

    /// Grab the optional flag stored in an element as mutable
    pub fn get_flags_mut(&mut self) -> &mut Option<ElementFlags> {
        match self {
            Element::Tree(_, flags)
            | Element::Item(_, flags)
            | Element::Reference(_, _, flags)
            | Element::SumTree(.., flags)
            | Element::BigSumTree(.., flags)
            | Element::CountTree(.., flags)
            | Element::SumItem(_, flags)
            | Element::CountSumTree(.., flags)
            | Element::ProvableCountTree(.., flags)
            | Element::ProvableCountSumTree(.., flags)
            | Element::ItemWithSumItem(.., flags) => flags,
        }
    }

    /// Sets the optional flag stored in an element
    pub fn set_flags(&mut self, new_flags: Option<ElementFlags>) {
        match self {
            Element::Tree(_, flags)
            | Element::Item(_, flags)
            | Element::Reference(_, _, flags)
            | Element::SumTree(.., flags)
            | Element::BigSumTree(.., flags)
            | Element::CountTree(.., flags)
            | Element::SumItem(_, flags)
            | Element::CountSumTree(.., flags)
            | Element::ProvableCountTree(.., flags)
            | Element::ProvableCountSumTree(.., flags)
            | Element::ItemWithSumItem(.., flags) => *flags = new_flags,
        }
    }

    /// Get the required item space
    pub fn required_item_space(
        len: u32,
        flag_len: u32,
        grove_version: &GroveVersion,
    ) -> Result<u32, ElementError> {
        check_grovedb_v0!(
            "required_item_space",
            grove_version.grovedb_versions.element.required_item_space
        );
        Ok(len + len.required_space() as u32 + flag_len + flag_len.required_space() as u32 + 1)
    }

    /// Convert the reference to an absolute reference
    pub fn convert_if_reference_to_absolute_reference(
        self,
        path: &[&[u8]],
        key: Option<&[u8]>,
    ) -> Result<Element, ElementError> {
        // Convert any non-absolute reference type to an absolute one
        // we do this here because references are aggregated first then followed later
        // to follow non-absolute references, we need the path they are stored at
        // this information is lost during the aggregation phase.
        Ok(match &self {
            Element::Reference(reference_path_type, max_hop, flags) => match reference_path_type {
                ReferencePathType::AbsolutePathReference(..) => self,
                _ => {
                    // Element is a reference and is not absolute.
                    // build the stored path for this reference
                    let absolute_path =
                        path_from_reference_path_type(reference_path_type.clone(), path, key)?;
                    // return an absolute reference that contains this info
                    Element::Reference(
                        ReferencePathType::AbsolutePathReference(absolute_path),
                        *max_hop,
                        flags.clone(),
                    )
                }
            },
            _ => self,
        })
    }
}
