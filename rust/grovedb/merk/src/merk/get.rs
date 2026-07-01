use grovedb_costs::{CostContext, CostResult, CostsExt, OperationCost};
use grovedb_storage::StorageContext;
use grovedb_version::version::GroveVersion;

use crate::{
    tree::{kv::ValueDefinedCostType, TreeNode},
    CryptoHash, Error,
    Error::StorageError,
    Merk, TreeFeatureType,
};

impl<'db, S> Merk<S>
where
    S: StorageContext<'db>,
{
    /// Gets an auxiliary value.
    pub fn get_aux(&self, key: &[u8]) -> CostResult<Option<Vec<u8>>, Error> {
        self.storage.get_aux(key).map_err(StorageError)
    }

    /// Returns if the value at the given key exists
    ///
    /// Note that this is essentially the same as a normal RocksDB `get`, so
    /// should be a fast operation and has almost no tree overhead.
    pub fn exists(
        &self,
        key: &[u8],
        value_defined_cost_fn: Option<
            impl Fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>,
        >,
        grove_version: &GroveVersion,
    ) -> CostResult<bool, Error> {
        self.has_node_direct(key, value_defined_cost_fn, grove_version)
    }

    /// Returns if the value at the given key exists
    ///
    /// Note that this is essentially the same as a normal RocksDB `get`, so
    /// should be a fast operation and has almost no tree overhead.
    /// Contrary to a simple exists, this traverses the tree and can be faster
    /// if the tree is cached, but slower if it is not
    pub fn exists_by_traversing_tree(
        &self,
        key: &[u8],
        value_defined_cost_fn: Option<
            impl Fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>,
        >,
        grove_version: &GroveVersion,
    ) -> CostResult<bool, Error> {
        self.has_node(key, value_defined_cost_fn, grove_version)
    }

    /// Gets a value for the given key. If the key is not found, `None` is
    /// returned.
    ///
    /// Note that this is essentially the same as a normal RocksDB `get`, so
    /// should be a fast operation and has almost no tree overhead.
    pub fn get(
        &self,
        key: &[u8],
        allow_cache: bool,
        value_defined_cost_fn: Option<
            impl Fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>,
        >,
        grove_version: &GroveVersion,
    ) -> CostResult<Option<Vec<u8>>, Error> {
        if allow_cache {
            self.get_node_fn(
                key,
                |node| {
                    node.value_as_slice()
                        .to_vec()
                        .wrap_with_cost(Default::default())
                },
                value_defined_cost_fn,
                grove_version,
            )
        } else {
            self.get_node_direct_fn(
                key,
                |node| {
                    node.value_as_slice()
                        .to_vec()
                        .wrap_with_cost(Default::default())
                },
                value_defined_cost_fn,
                grove_version,
            )
        }
    }

    /// Returns the feature type for the node at the given key.
    pub fn get_feature_type(
        &self,
        key: &[u8],
        allow_cache: bool,
        value_defined_cost_fn: Option<
            impl Fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>,
        >,
        grove_version: &GroveVersion,
    ) -> CostResult<Option<TreeFeatureType>, Error> {
        if allow_cache {
            self.get_node_fn(
                key,
                |node| node.feature_type().wrap_with_cost(Default::default()),
                value_defined_cost_fn,
                grove_version,
            )
        } else {
            self.get_node_direct_fn(
                key,
                |node| node.feature_type().wrap_with_cost(Default::default()),
                value_defined_cost_fn,
                grove_version,
            )
        }
    }

    /// Gets a hash of a node by a given key, `None` is returned in case
    /// when node not found by the key.
    pub fn get_hash(
        &self,
        key: &[u8],
        allow_cache: bool,
        value_defined_cost_fn: Option<
            impl Fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>,
        >,
        grove_version: &GroveVersion,
    ) -> CostResult<Option<CryptoHash>, Error> {
        if allow_cache {
            self.get_node_fn(
                key,
                |node| node.hash(),
                value_defined_cost_fn,
                grove_version,
            )
        } else {
            self.get_node_direct_fn(
                key,
                |node| node.hash(),
                value_defined_cost_fn,
                grove_version,
            )
        }
    }

    /// Gets the value hash of a node by a given key, `None` is returned in case
    /// when node not found by the key.
    pub fn get_value_hash(
        &self,
        key: &[u8],
        allow_cache: bool,
        value_defined_cost_fn: Option<
            impl Fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>,
        >,
        grove_version: &GroveVersion,
    ) -> CostResult<Option<CryptoHash>, Error> {
        if allow_cache {
            self.get_node_fn(
                key,
                |node| (*node.value_hash()).wrap_with_cost(OperationCost::default()),
                value_defined_cost_fn,
                grove_version,
            )
        } else {
            self.get_node_direct_fn(
                key,
                |node| (*node.value_hash()).wrap_with_cost(OperationCost::default()),
                value_defined_cost_fn,
                grove_version,
            )
        }
    }

    /// Gets a hash of a node by a given key, `None` is returned in case
    /// when node not found by the key.
    pub fn get_kv_hash(
        &self,
        key: &[u8],
        allow_cache: bool,
        value_defined_cost_fn: Option<
            impl Fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>,
        >,
        grove_version: &GroveVersion,
    ) -> CostResult<Option<CryptoHash>, Error> {
        if allow_cache {
            self.get_node_fn(
                key,
                |node| (*node.inner.kv.hash()).wrap_with_cost(OperationCost::default()),
                value_defined_cost_fn,
                grove_version,
            )
        } else {
            self.get_node_direct_fn(
                key,
                |node| (*node.inner.kv.hash()).wrap_with_cost(OperationCost::default()),
                value_defined_cost_fn,
                grove_version,
            )
        }
    }

    /// Gets the value and value hash of a node by a given key, `None` is
    /// returned in case when node not found by the key.
    pub fn get_value_and_value_hash(
        &self,
        key: &[u8],
        allow_cache: bool,
        value_defined_cost_fn: Option<
            impl Fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>,
        >,
        grove_version: &GroveVersion,
    ) -> CostResult<Option<(Vec<u8>, CryptoHash)>, Error> {
        if allow_cache {
            self.get_node_fn(
                key,
                |node| {
                    (node.value_as_slice().to_vec(), *node.value_hash())
                        .wrap_with_cost(OperationCost::default())
                },
                value_defined_cost_fn,
                grove_version,
            )
        } else {
            self.get_node_direct_fn(
                key,
                |node| {
                    (node.value_as_slice().to_vec(), *node.value_hash())
                        .wrap_with_cost(OperationCost::default())
                },
                value_defined_cost_fn,
                grove_version,
            )
        }
    }

    /// See if a node's field exists
    fn has_node_direct(
        &self,
        key: &[u8],
        value_defined_cost_fn: Option<
            impl Fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>,
        >,
        grove_version: &GroveVersion,
    ) -> CostResult<bool, Error> {
        TreeNode::get(&self.storage, key, value_defined_cost_fn, grove_version)
            .map_ok(|x| x.is_some())
    }

    /// See if a node's field exists
    fn has_node(
        &self,
        key: &[u8],
        value_defined_cost_fn: Option<
            impl Fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>,
        >,
        grove_version: &GroveVersion,
    ) -> CostResult<bool, Error> {
        self.use_tree(move |maybe_tree| {
            let mut cursor = match maybe_tree {
                None => return Ok(false).wrap_with_cost(Default::default()), // empty tree
                Some(tree) => tree,
            };

            loop {
                if key == cursor.key() {
                    return Ok(true).wrap_with_cost(OperationCost::default());
                }

                let left = key < cursor.key();
                let link = match cursor.link(left) {
                    None => return Ok(false).wrap_with_cost(Default::default()), // not found
                    Some(link) => link,
                };

                let maybe_child = link.tree();
                match maybe_child {
                    None => {
                        // fetch from RocksDB
                        break self.has_node_direct(key, value_defined_cost_fn, grove_version);
                    }
                    Some(child) => cursor = child, // traverse to child
                }
            }
        })
    }

    /// Generic way to get a node's field
    pub(crate) fn get_node_direct_fn<T, F>(
        &self,
        key: &[u8],
        f: F,
        value_defined_cost_fn: Option<
            impl Fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>,
        >,
        grove_version: &GroveVersion,
    ) -> CostResult<Option<T>, Error>
    where
        F: FnOnce(&TreeNode) -> CostContext<T>,
    {
        TreeNode::get(&self.storage, key, value_defined_cost_fn, grove_version).flat_map_ok(
            |maybe_node| {
                let mut cost = OperationCost::default();
                Ok(maybe_node.map(|node| f(&node).unwrap_add_cost(&mut cost))).wrap_with_cost(cost)
            },
        )
    }

    /// Generic way to get a node's field
    fn get_node_fn<T, F>(
        &self,
        key: &[u8],
        f: F,
        value_defined_cost_fn: Option<
            impl Fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>,
        >,
        grove_version: &GroveVersion,
    ) -> CostResult<Option<T>, Error>
    where
        F: FnOnce(&TreeNode) -> CostContext<T>,
    {
        self.use_tree(move |maybe_tree| {
            let mut cursor = match maybe_tree {
                None => return Ok(None).wrap_with_cost(Default::default()), // empty tree
                Some(tree) => tree,
            };

            loop {
                if key == cursor.key() {
                    return f(cursor).map(|x| Ok(Some(x)));
                }

                let left = key < cursor.key();
                let link = match cursor.link(left) {
                    None => return Ok(None).wrap_with_cost(Default::default()), // not found
                    Some(link) => link,
                };

                let maybe_child = link.tree();
                match maybe_child {
                    None => {
                        // fetch from RocksDB
                        break self.get_node_direct_fn(
                            key,
                            f,
                            value_defined_cost_fn,
                            grove_version,
                        );
                    }
                    Some(child) => cursor = child, // traverse to child
                }
            }
        })
    }
}

#[cfg(test)]
mod test {
    use grovedb_version::version::GroveVersion;

    use crate::{
        test_utils::TempMerk, tree::kv::ValueDefinedCostType, Op, TreeFeatureType::BasicMerkNode,
    };

    #[test]
    fn test_has_node_with_empty_tree() {
        let grove_version = GroveVersion::latest();
        let mut merk = TempMerk::new(grove_version);

        let key = b"something";

        let result = merk
            .has_node(
                key,
                None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                grove_version,
            )
            .unwrap()
            .unwrap();

        assert!(!result);

        let batch_entry = (key, Op::Put(vec![123; 60], BasicMerkNode));

        let batch = vec![batch_entry];

        merk.apply::<_, Vec<_>>(&batch, &[], None, grove_version)
            .unwrap()
            .expect("should ...");

        let result = merk
            .has_node(
                key,
                None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                grove_version,
            )
            .unwrap()
            .unwrap();

        assert!(result);
    }
}
