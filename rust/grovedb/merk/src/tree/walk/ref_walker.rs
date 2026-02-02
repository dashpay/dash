//! Merk reference walker

#[cfg(feature = "minimal")]
use std::cmp::Ordering;

#[cfg(feature = "minimal")]
use grovedb_costs::{CostResult, CostsExt, OperationCost};
use grovedb_version::version::GroveVersion;

#[cfg(feature = "minimal")]
use super::{
    super::{Link, TreeNode},
    Fetch,
};
use crate::tree::kv::ValueDefinedCostType;
#[cfg(feature = "minimal")]
use crate::Error;

#[cfg(feature = "minimal")]
/// Allows read-only traversal of a `Tree`, fetching from the given source when
/// traversing to a pruned node. The fetched nodes are then retained in memory
/// until they (possibly) get pruned on the next commit.
///
/// Only finalized trees may be walked (trees which have had `commit` called
/// since the last update).
pub struct RefWalker<'a, S>
where
    S: Fetch + Sized + Clone,
{
    tree: &'a mut TreeNode,
    source: S,
}

#[cfg(feature = "minimal")]
impl<'a, S> RefWalker<'a, S>
where
    S: Fetch + Sized + Clone,
{
    /// Creates a `RefWalker` with the given tree and source.
    pub fn new(tree: &'a mut TreeNode, source: S) -> Self {
        // TODO: check if tree has modified links, panic if so
        RefWalker { tree, source }
    }

    /// Gets an immutable reference to the `Tree` wrapped by this `RefWalker`.
    pub fn tree(&self) -> &TreeNode {
        self.tree
    }

    /// Traverses to the child on the given side (if any), fetching from the
    /// source if pruned. When fetching, the link is upgraded from
    /// `Link::Reference` to `Link::Loaded`.
    pub fn walk<V>(
        &mut self,
        left: bool,
        value_defined_cost_fn: Option<&V>,
        grove_version: &GroveVersion,
    ) -> CostResult<Option<RefWalker<'_, S>>, Error>
    where
        V: Fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>,
    {
        let link = match self.tree.link(left) {
            None => return Ok(None).wrap_with_cost(Default::default()),
            Some(link) => link,
        };

        let mut cost = OperationCost::default();
        match link {
            Link::Reference { .. } => {
                let load_res = self
                    .tree
                    .load(left, &self.source, value_defined_cost_fn, grove_version)
                    .unwrap_add_cost(&mut cost);
                if let Err(e) = load_res {
                    return Err(e).wrap_with_cost(cost);
                }
            }
            Link::Modified { .. } => panic!("Cannot traverse Link::Modified"),
            Link::Uncommitted { .. } | Link::Loaded { .. } => {}
        }

        let child = self.tree.child_mut(left).unwrap();
        Ok(Some(RefWalker::new(child, self.source.clone()))).wrap_with_cost(cost)
    }

    /// Finds the traversal path (sequence of left/right) to reach a specific
    /// key.
    ///
    /// Traverses the tree from the current position, comparing the target key
    /// with each node's key and recording the path taken until the key is
    /// found.
    ///
    /// # Arguments
    /// * `target_key` - The key to find
    /// * `grove_version` - The grove version for compatibility
    ///
    /// # Returns
    /// * `Ok(Some(path))` - If the key was found, with the traversal path
    ///   (true=left, false=right)
    /// * `Ok(None)` - If the key was not found
    pub fn find_key_path(
        &mut self,
        target_key: &[u8],
        grove_version: &GroveVersion,
    ) -> CostResult<Option<Vec<bool>>, Error> {
        let mut cost = OperationCost::default();
        let mut path = Vec::new();

        self.find_key_path_internal(target_key, grove_version, &mut path, &mut cost)
            .map(|found| if found { Some(path) } else { None })
            .wrap_with_cost(cost)
    }

    fn find_key_path_internal(
        &mut self,
        target_key: &[u8],
        grove_version: &GroveVersion,
        path: &mut Vec<bool>,
        cost: &mut OperationCost,
    ) -> Result<bool, Error> {
        let current_key = self.tree.key();
        match target_key.cmp(current_key) {
            Ordering::Equal => {
                // Found the key
                Ok(true)
            }
            Ordering::Less => {
                // Target is smaller, go left
                let maybe_left = self
                    .walk(
                        true,
                        None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                        grove_version,
                    )
                    .unwrap_add_cost(cost);

                match maybe_left {
                    Ok(Some(mut left_walker)) => {
                        path.push(true);
                        left_walker.find_key_path_internal(target_key, grove_version, path, cost)
                    }
                    Ok(None) => Ok(false),
                    Err(e) => Err(e),
                }
            }
            Ordering::Greater => {
                // Target is larger, go right
                let maybe_right = self
                    .walk(
                        false,
                        None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                        grove_version,
                    )
                    .unwrap_add_cost(cost);

                match maybe_right {
                    Ok(Some(mut right_walker)) => {
                        path.push(false);
                        right_walker.find_key_path_internal(target_key, grove_version, path, cost)
                    }
                    Ok(None) => Ok(false),
                    Err(e) => Err(e),
                }
            }
        }
    }
}
