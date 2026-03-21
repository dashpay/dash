use grovedb_costs::CostResult;
use grovedb_storage::StorageContext;
use grovedb_version::version::GroveVersion;

use crate::{
    tree::{kv::ValueDefinedCostType, Fetch, TreeNode},
    tree_type::TreeType,
    Error, Link, Merk,
};

impl<'db, S> Merk<S>
where
    S: StorageContext<'db>,
{
    pub(in crate::merk) fn source(&self) -> MerkSource<'_, S> {
        MerkSource {
            storage: &self.storage,
            tree_type: self.tree_type,
        }
    }
}

#[derive(Debug)]
pub struct MerkSource<'s, S> {
    storage: &'s S,
    tree_type: TreeType,
}

impl<S> Clone for MerkSource<'_, S> {
    fn clone(&self) -> Self {
        MerkSource {
            storage: self.storage,
            tree_type: self.tree_type,
        }
    }
}

impl<'db, S> Fetch for MerkSource<'_, S>
where
    S: StorageContext<'db>,
{
    fn fetch(
        &self,
        link: &Link,
        value_defined_cost_fn: Option<
            &impl Fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>,
        >,
        grove_version: &GroveVersion,
    ) -> CostResult<TreeNode, Error> {
        TreeNode::get(
            self.storage,
            link.key(),
            value_defined_cost_fn,
            grove_version,
        )
        .map_ok(|x| x.ok_or(Error::KeyNotFoundError("Key not found for fetch")))
        .flatten()
    }
}
