use grovedb_element::Element;
use grovedb_version::{check_grovedb_v0, version::GroveVersion};
use integer_encoding::VarInt;

use crate::{
    merk::NodeType,
    tree::kv::{
        ValueDefinedCostType,
        ValueDefinedCostType::{LayeredValueDefinedCost, SpecializedValueDefinedCost},
        KV,
    },
    tree_type::{
        BIG_SUM_TREE_COST_SIZE, COUNT_SUM_TREE_COST_SIZE, COUNT_TREE_COST_SIZE, SUM_ITEM_COST_SIZE,
        SUM_TREE_COST_SIZE, TREE_COST_SIZE,
    },
    Error,
};

pub trait ElementCostExtensions {
    /// Get tree costs for a key value
    fn specialized_costs_for_key_value(
        key: &[u8],
        value: &[u8],
        node_type: NodeType,
        grove_version: &GroveVersion,
    ) -> Result<u32, Error>;

    /// Get the value defined cost for a serialized value item with sum item or
    /// sum item
    fn specialized_value_defined_cost(&self, grove_version: &GroveVersion) -> Option<u32>;

    /// Get the value defined cost for a serialized value item with a tree
    fn layered_value_defined_cost(&self, grove_version: &GroveVersion) -> Option<u32>;

    /// Get the value defined cost for a serialized value
    fn value_defined_cost(&self, grove_version: &GroveVersion) -> Option<ValueDefinedCostType>;

    /// Get the value defined cost for a serialized value
    fn value_defined_cost_for_serialized_value(
        value: &[u8],
        grove_version: &GroveVersion,
    ) -> Option<ValueDefinedCostType>;
}

pub trait ElementCostPrivateExtensions {
    /// Get tree cost for the element
    fn get_specialized_cost(&self, grove_version: &GroveVersion) -> Result<u32, Error>;
}

impl ElementCostPrivateExtensions for Element {
    /// Get tree cost for the element
    fn get_specialized_cost(&self, grove_version: &GroveVersion) -> Result<u32, Error> {
        check_grovedb_v0!(
            "get_specialized_cost",
            grove_version.grovedb_versions.element.get_specialized_cost
        );
        match self {
            Element::Tree(..) => Ok(TREE_COST_SIZE),
            Element::SumTree(..) => Ok(SUM_TREE_COST_SIZE),
            Element::BigSumTree(..) => Ok(BIG_SUM_TREE_COST_SIZE),
            Element::SumItem(..) | Element::ItemWithSumItem(..) => Ok(SUM_ITEM_COST_SIZE),
            Element::CountTree(..) => Ok(COUNT_TREE_COST_SIZE),
            Element::CountSumTree(..) => Ok(COUNT_SUM_TREE_COST_SIZE),
            Element::ProvableCountTree(..) => Ok(COUNT_TREE_COST_SIZE),
            Element::ProvableCountSumTree(..) => Ok(COUNT_SUM_TREE_COST_SIZE),
            _ => Err(Error::CorruptedCodeExecution(
                "trying to get tree cost from non tree element",
            )),
        }
    }
}

impl ElementCostExtensions for Element {
    /// Get tree costs for a key value
    fn specialized_costs_for_key_value(
        key: &[u8],
        value: &[u8],
        node_type: NodeType,
        grove_version: &GroveVersion,
    ) -> Result<u32, Error> {
        check_grovedb_v0!(
            "specialized_costs_for_key_value",
            grove_version
                .grovedb_versions
                .element
                .specialized_costs_for_key_value
        );
        // todo: we actually don't need to deserialize the whole element
        let element = Element::deserialize(value, grove_version)?;
        let cost = match element {
            Element::Tree(_, flags) => {
                let flags_len = flags.map_or(0, |flags| {
                    let flags_len = flags.len() as u32;
                    flags_len + flags_len.required_space() as u32
                });
                let value_len = TREE_COST_SIZE + flags_len;
                let key_len = key.len() as u32;
                KV::layered_value_byte_cost_size_for_key_and_value_lengths(
                    key_len, value_len, node_type,
                )
            }
            Element::SumTree(_, _sum_value, flags) => {
                let flags_len = flags.map_or(0, |flags| {
                    let flags_len = flags.len() as u32;
                    flags_len + flags_len.required_space() as u32
                });
                let value_len = SUM_TREE_COST_SIZE + flags_len;
                let key_len = key.len() as u32;
                KV::layered_value_byte_cost_size_for_key_and_value_lengths(
                    key_len, value_len, node_type,
                )
            }
            Element::BigSumTree(_, _sum_value, flags) => {
                let flags_len = flags.map_or(0, |flags| {
                    let flags_len = flags.len() as u32;
                    flags_len + flags_len.required_space() as u32
                });
                let value_len = BIG_SUM_TREE_COST_SIZE + flags_len;
                let key_len = key.len() as u32;
                KV::layered_value_byte_cost_size_for_key_and_value_lengths(
                    key_len, value_len, node_type,
                )
            }
            Element::CountTree(_, _count_value, flags) => {
                let flags_len = flags.map_or(0, |flags| {
                    let flags_len = flags.len() as u32;
                    flags_len + flags_len.required_space() as u32
                });
                let value_len = COUNT_TREE_COST_SIZE + flags_len;
                let key_len = key.len() as u32;
                KV::layered_value_byte_cost_size_for_key_and_value_lengths(
                    key_len, value_len, node_type,
                )
            }
            Element::CountSumTree(.., flags) => {
                let flags_len = flags.map_or(0, |flags| {
                    let flags_len = flags.len() as u32;
                    flags_len + flags_len.required_space() as u32
                });
                let value_len = COUNT_SUM_TREE_COST_SIZE + flags_len;
                let key_len = key.len() as u32;
                KV::layered_value_byte_cost_size_for_key_and_value_lengths(
                    key_len, value_len, node_type,
                )
            }
            Element::ProvableCountTree(_, _count_value, flags) => {
                let flags_len = flags.map_or(0, |flags| {
                    let flags_len = flags.len() as u32;
                    flags_len + flags_len.required_space() as u32
                });
                let value_len = COUNT_TREE_COST_SIZE + flags_len;
                let key_len = key.len() as u32;
                KV::layered_value_byte_cost_size_for_key_and_value_lengths(
                    key_len, value_len, node_type,
                )
            }
            Element::ProvableCountSumTree(.., flags) => {
                let flags_len = flags.map_or(0, |flags| {
                    let flags_len = flags.len() as u32;
                    flags_len + flags_len.required_space() as u32
                });
                let value_len = COUNT_SUM_TREE_COST_SIZE + flags_len;
                let key_len = key.len() as u32;
                KV::layered_value_byte_cost_size_for_key_and_value_lengths(
                    key_len, value_len, node_type,
                )
            }
            Element::SumItem(.., flags) => {
                let flags_len = flags.map_or(0, |flags| {
                    let flags_len = flags.len() as u32;
                    flags_len + flags_len.required_space() as u32
                });
                let value_len = SUM_ITEM_COST_SIZE + flags_len;
                let key_len = key.len() as u32;
                KV::node_value_byte_cost_size(key_len, value_len, node_type)
            }
            Element::ItemWithSumItem(item_value, _, flags) => {
                let item_value_len = item_value.len() as u32;
                let flags_len = flags.map_or(0, |flags| {
                    let flags_len = flags.len() as u32;
                    flags_len + flags_len.required_space() as u32
                });
                let value_len = item_value_len
                    + item_value_len.required_space() as u32
                    + SUM_ITEM_COST_SIZE
                    + flags_len;
                let key_len = key.len() as u32;
                KV::node_value_byte_cost_size(key_len, value_len, node_type)
            }
            _ => KV::node_value_byte_cost_size(key.len() as u32, value.len() as u32, node_type),
        };
        Ok(cost)
    }

    /// Get the value defined cost for a serialized value item with sum item or
    /// sum item
    fn specialized_value_defined_cost(&self, grove_version: &GroveVersion) -> Option<u32> {
        let value_cost = self.get_specialized_cost(grove_version).ok()?;

        let cost = value_cost
            + self.get_flags().as_ref().map_or(0, |flags| {
                let flags_len = flags.len() as u32;
                flags_len + flags_len.required_space() as u32
            });
        match self {
            Element::SumItem(..) => Some(cost),
            Element::ItemWithSumItem(item, ..) => {
                let item_len = item.len() as u32;
                Some(cost + item_len + item_len.required_space() as u32)
            }
            _ => None,
        }
    }

    /// Get the value defined cost for a serialized value item with a tree
    fn layered_value_defined_cost(&self, grove_version: &GroveVersion) -> Option<u32> {
        let value_cost = self.get_specialized_cost(grove_version).ok()?;

        let cost = value_cost
            + self.get_flags().as_ref().map_or(0, |flags| {
                let flags_len = flags.len() as u32;
                flags_len + flags_len.required_space() as u32
            });
        match self {
            Element::Tree(..)
            | Element::SumTree(..)
            | Element::BigSumTree(..)
            | Element::CountTree(..)
            | Element::CountSumTree(..)
            | Element::ProvableCountTree(..)
            | Element::ProvableCountSumTree(..) => Some(cost),
            _ => None,
        }
    }

    /// Get the value defined cost for a serialized value
    fn value_defined_cost(&self, grove_version: &GroveVersion) -> Option<ValueDefinedCostType> {
        let value_cost = self.get_specialized_cost(grove_version).ok()?;

        let cost = value_cost
            + self.get_flags().as_ref().map_or(0, |flags| {
                let flags_len = flags.len() as u32;
                flags_len + flags_len.required_space() as u32
            });
        match self {
            Element::Tree(..) => Some(LayeredValueDefinedCost(cost)),
            Element::SumTree(..) => Some(LayeredValueDefinedCost(cost)),
            Element::BigSumTree(..) => Some(LayeredValueDefinedCost(cost)),
            Element::CountTree(..) => Some(LayeredValueDefinedCost(cost)),
            Element::CountSumTree(..) => Some(LayeredValueDefinedCost(cost)),
            Element::ProvableCountTree(..) => Some(LayeredValueDefinedCost(cost)),
            Element::ProvableCountSumTree(..) => Some(LayeredValueDefinedCost(cost)),
            Element::SumItem(..) => Some(SpecializedValueDefinedCost(cost)),
            Element::ItemWithSumItem(item, ..) => {
                let item_len = item.len() as u32;
                Some(SpecializedValueDefinedCost(
                    cost + item_len + item_len.required_space() as u32,
                ))
            }
            _ => None,
        }
    }

    /// Get the value defined cost for a serialized value
    fn value_defined_cost_for_serialized_value(
        value: &[u8],
        grove_version: &GroveVersion,
    ) -> Option<ValueDefinedCostType> {
        let element = Element::deserialize(value, grove_version).ok()?;
        element.value_defined_cost(grove_version)
    }
}
