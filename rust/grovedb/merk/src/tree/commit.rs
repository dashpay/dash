//! Merk tree commit

#[cfg(feature = "minimal")]
use super::TreeNode;
#[cfg(feature = "minimal")]
use crate::error::Error;

#[cfg(feature = "minimal")]
/// To be used when committing a tree (writing it to a store after applying the
/// changes).
pub trait Commit {
    /// Called once per updated node when a finalized tree is to be written to a
    /// backing store or cache.
    fn write(
        &mut self,
        tree: &mut TreeNode,
        old_specialized_cost: &impl Fn(&Vec<u8>, &Vec<u8>) -> Result<u32, Error>,
    ) -> Result<(), Error>;

    /// Called once per node after writing a node and its children. The returned
    /// tuple specifies whether or not to prune the left and right child nodes,
    /// respectively. For example, returning `(true, true)` will prune both
    /// nodes, removing them from memory.
    fn prune(&self, _tree: &TreeNode) -> (bool, bool) {
        (true, true)
    }
}

#[cfg(feature = "minimal")]
/// A `Commit` implementation which does not write to a store and does not prune
/// any nodes from the Tree. Useful when only keeping a tree in memory.
pub struct NoopCommit {}

#[cfg(feature = "minimal")]
impl Commit for NoopCommit {
    fn write(
        &mut self,
        _tree: &mut TreeNode,
        _old_specialized_cost: &impl Fn(&Vec<u8>, &Vec<u8>) -> Result<u32, Error>,
    ) -> Result<(), Error> {
        Ok(())
    }

    fn prune(&self, _tree: &TreeNode) -> (bool, bool) {
        (false, false)
    }
}
