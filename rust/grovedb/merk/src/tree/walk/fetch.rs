//! Walk

#[cfg(feature = "minimal")]
use grovedb_costs::CostResult;
use grovedb_version::version::GroveVersion;

#[cfg(feature = "minimal")]
use super::super::{Link, TreeNode};
#[cfg(feature = "minimal")]
use crate::error::Error;
#[cfg(feature = "minimal")]
use crate::tree::kv::ValueDefinedCostType;

#[cfg(feature = "minimal")]
/// A source of data to be used by the tree when encountering a pruned node.
/// This typically means fetching the tree node from a backing store by its key,
/// but could also implement an in-memory cache for example.
pub trait Fetch {
    /// Called when the tree needs to fetch a node with the given `Link`. The
    /// `link` value will always be a `Link::Reference` variant.
    fn fetch(
        &self,
        link: &Link,
        value_defined_cost_fn: Option<
            &impl Fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>,
        >,
        grove_version: &GroveVersion,
    ) -> CostResult<TreeNode, Error>;
}
