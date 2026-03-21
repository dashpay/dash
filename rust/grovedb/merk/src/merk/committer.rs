use crate::{
    merk::BatchValue,
    tree::{Commit, TreeNode},
    Error,
};

pub struct MerkCommitter {
    /// The batch has a key, maybe a value, with the value bytes, maybe the left
    /// child size and maybe the right child size, then the
    /// key_value_storage_cost
    pub(in crate::merk) batch: Vec<BatchValue>,
    pub(in crate::merk) height: u8,
    pub(in crate::merk) levels: u8,
}

impl MerkCommitter {
    pub(in crate::merk) fn new(height: u8, levels: u8) -> Self {
        Self {
            batch: Vec::with_capacity(10000),
            height,
            levels,
        }
    }
}

impl Commit for MerkCommitter {
    fn write(
        &mut self,
        tree: &mut TreeNode,
        old_specialized_cost: &impl Fn(&Vec<u8>, &Vec<u8>) -> Result<u32, Error>,
    ) -> Result<(), Error> {
        let tree_size = tree.encoding_length();
        let storage_costs = if let Some(storage_costs) = tree.known_storage_cost.take() {
            storage_costs
        } else {
            tree.kv_with_parent_hook_size_and_storage_cost(old_specialized_cost)?
                .1
        };

        let mut buf = Vec::with_capacity(tree_size);
        tree.encode_into(&mut buf);

        let left_child_sizes = tree.child_ref_and_sum_size(true);
        let right_child_sizes = tree.child_ref_and_sum_size(false);
        self.batch.push((
            tree.key().to_vec(),
            tree.feature_type()
                .tree_feature_specialized_type_and_length(),
            Some((buf, left_child_sizes, right_child_sizes)),
            storage_costs,
        ));
        Ok(())
    }

    fn prune(&self, tree: &TreeNode) -> (bool, bool) {
        // keep N top levels of tree
        let prune = (self.height - tree.height()) >= self.levels;
        (prune, prune)
    }
}
