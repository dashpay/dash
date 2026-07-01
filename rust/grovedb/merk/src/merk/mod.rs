// MIT LICENSE
//
// Copyright (c) 2021 Dash Core Group
//
// Permission is hereby granted, free of charge, to any
// person obtaining a copy of this software and associated
// documentation files (the "Software"), to deal in the
// Software without restriction, including without
// limitation the rights to use, copy, modify, merge,
// publish, distribute, sublicense, and/or sell copies of
// the Software, and to permit persons to whom the Software
// is furnished to do so, subject to the following
// conditions:
//
// The above copyright notice and this permission notice
// shall be included in all copies or substantial portions
// of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF
// ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED
// TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
// PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT
// SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
// CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
// OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR
// IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.

//! Merk

pub mod chunks;
pub(crate) mod defaults;

pub mod options;

pub mod apply;
pub mod clear;
pub mod committer;
pub mod get;
pub mod open;
pub mod prove;
pub mod restore;
pub mod source;

use std::{
    cell::Cell,
    collections::{BTreeMap, BTreeSet, LinkedList},
    fmt,
};

use committer::MerkCommitter;
use grovedb_costs::{
    cost_return_on_error, cost_return_on_error_default, cost_return_on_error_no_add,
    storage_cost::key_value_cost::KeyValueStorageCost, ChildrenSizesWithValue, CostContext,
    CostResult, CostsExt, FeatureSumLength, OperationCost, TreeCostType,
};
use grovedb_storage::{self, Batch, RawIterator, StorageContext};
use grovedb_version::version::GroveVersion;
use source::MerkSource;

use crate::{
    error::Error,
    merk::{defaults::ROOT_KEY_KEY, options::MerkOptions},
    proofs::{
        branch::{
            calculate_chunk_depths, calculate_chunk_depths_with_minimum,
            calculate_max_tree_depth_from_count, BranchQueryResult, TrunkQueryResult,
        },
        chunk::{
            chunk::{LEFT, RIGHT},
            util::traversal_instruction_as_vec_bytes,
        },
        query::query_item::QueryItem,
        Query,
    },
    tree::{
        kv::ValueDefinedCostType, AggregateData, AuxMerkBatch, CryptoHash, Fetch, Op, RefWalker,
        TreeNode, NULL_HASH,
    },
    tree_type::TreeType,
    Error::{CostsError, EdError, StorageError},
    Link,
    MerkType::{BaseMerk, LayeredMerk, StandaloneMerk},
};

/// Key update types
pub struct KeyUpdates {
    pub new_keys: BTreeSet<Vec<u8>>,
    pub updated_keys: BTreeSet<Vec<u8>>,
    pub deleted_keys: LinkedList<(Vec<u8>, KeyValueStorageCost)>,
    pub updated_root_key_from: Option<Vec<u8>>,
}

impl KeyUpdates {
    /// New KeyUpdate
    pub fn new(
        new_keys: BTreeSet<Vec<u8>>,
        updated_keys: BTreeSet<Vec<u8>>,
        deleted_keys: LinkedList<(Vec<u8>, KeyValueStorageCost)>,
        updated_root_key_from: Option<Vec<u8>>,
    ) -> Self {
        Self {
            new_keys,
            updated_keys,
            deleted_keys,
            updated_root_key_from,
        }
    }
}

/// Type alias for simple function signature
pub type BatchValue = (
    Vec<u8>,
    Option<(TreeCostType, FeatureSumLength)>,
    ChildrenSizesWithValue,
    KeyValueStorageCost,
);

/// Root hash key and sum
pub type RootHashKeyAndAggregateData = (CryptoHash, Option<Vec<u8>>, AggregateData);

/// KVIterator allows you to lazily iterate over each kv pair of a subtree
pub struct KVIterator<'a, I: RawIterator> {
    raw_iter: I,
    _query: &'a Query,
    left_to_right: bool,
    query_iterator: Box<dyn Iterator<Item = &'a QueryItem> + 'a>,
    current_query_item: Option<&'a QueryItem>,
}

impl<'a, I: RawIterator> KVIterator<'a, I> {
    /// New iterator
    pub fn new(raw_iter: I, query: &'a Query) -> CostContext<Self> {
        let mut cost = OperationCost::default();
        let mut iterator = KVIterator {
            raw_iter,
            _query: query,
            left_to_right: query.left_to_right,
            current_query_item: None,
            query_iterator: query.directional_iter(query.left_to_right),
        };
        iterator.seek().unwrap_add_cost(&mut cost);
        iterator.wrap_with_cost(cost)
    }

    /// Returns the current node the iter points to if it's valid for the given
    /// query item returns None otherwise
    fn get_kv(&mut self, query_item: &QueryItem) -> CostContext<Option<(Vec<u8>, Vec<u8>)>> {
        let mut cost = OperationCost::default();

        if query_item
            .iter_is_valid_for_type(&self.raw_iter, None, self.left_to_right)
            .unwrap_add_cost(&mut cost)
        {
            let kv = (
                self.raw_iter
                    .key()
                    .unwrap_add_cost(&mut cost)
                    .expect("key must exist as iter is valid")
                    .to_vec(),
                self.raw_iter
                    .value()
                    .unwrap_add_cost(&mut cost)
                    .expect("value must exists as iter is valid")
                    .to_vec(),
            );
            if self.left_to_right {
                self.raw_iter.next().unwrap_add_cost(&mut cost)
            } else {
                self.raw_iter.prev().unwrap_add_cost(&mut cost)
            }
            Some(kv).wrap_with_cost(cost)
        } else {
            None.wrap_with_cost(cost)
        }
    }

    /// Moves the iter to the start of the next query item
    fn seek(&mut self) -> CostContext<()> {
        let mut cost = OperationCost::default();

        self.current_query_item = self.query_iterator.next();
        if let Some(query_item) = self.current_query_item {
            query_item
                .seek_for_iter(&mut self.raw_iter, self.left_to_right)
                .unwrap_add_cost(&mut cost);
        }

        ().wrap_with_cost(cost)
    }
}

// Cannot be an Iterator as it should return cost
impl<I: RawIterator> KVIterator<'_, I> {
    /// Next key-value
    pub fn next_kv(&mut self) -> CostContext<Option<(Vec<u8>, Vec<u8>)>> {
        let mut cost = OperationCost::default();

        if let Some(query_item) = self.current_query_item {
            let kv_pair = self.get_kv(query_item).unwrap_add_cost(&mut cost);

            if kv_pair.is_some() {
                kv_pair.wrap_with_cost(cost)
            } else {
                self.seek().unwrap_add_cost(&mut cost);
                self.next_kv().add_cost(cost)
            }
        } else {
            None.wrap_with_cost(cost)
        }
    }
}

#[derive(PartialEq, Eq)]
/// Merk types
pub enum MerkType {
    /// A StandaloneMerk has it's root key storage on a field and pays for root
    /// key updates
    StandaloneMerk,
    /// A BaseMerk has it's root key storage on a field but does not pay for
    /// when these keys change
    BaseMerk,
    /// A LayeredMerk has it's root key storage inside a parent merk
    LayeredMerk,
}

impl fmt::Display for MerkType {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        let description = match self {
            MerkType::StandaloneMerk => "StandaloneMerk",
            MerkType::BaseMerk => "BaseMerk",
            MerkType::LayeredMerk => "LayeredMerk",
        };
        write!(f, "{}", description)
    }
}

impl MerkType {
    /// Returns bool
    pub(crate) fn requires_root_storage_update(&self) -> bool {
        match self {
            StandaloneMerk => true,
            BaseMerk => true,
            LayeredMerk => false,
        }
    }
}

#[cfg(any(feature = "minimal", feature = "verify"))]
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub enum NodeType {
    NormalNode,
    SumNode,
    BigSumNode,
    CountNode,
    CountSumNode,
    ProvableCountNode,
    ProvableCountSumNode,
}

impl NodeType {
    pub const fn feature_len(&self) -> u32 {
        match self {
            NodeType::NormalNode => 1,
            NodeType::SumNode => 9,
            NodeType::BigSumNode => 17,
            NodeType::CountNode => 9,
            NodeType::CountSumNode => 17,
            NodeType::ProvableCountNode => 9,
            NodeType::ProvableCountSumNode => 17, // count (varint) + sum (varint)
        }
    }

    pub const fn cost(&self) -> u32 {
        match self {
            NodeType::NormalNode => 0,
            NodeType::SumNode => 8,
            NodeType::BigSumNode => 16,
            NodeType::CountNode => 8,
            NodeType::CountSumNode => 16,
            NodeType::ProvableCountNode => 8,
            NodeType::ProvableCountSumNode => 16, // count (8) + sum (8)
        }
    }
}

/// A handle to a Merkle key/value store backed by RocksDB.
pub struct Merk<S> {
    pub(crate) tree: Cell<Option<TreeNode>>,
    pub(crate) root_tree_key: Cell<Option<Vec<u8>>>,
    /// Storage
    pub storage: S,
    /// Merk type
    pub merk_type: MerkType,
    /// The tree type
    pub tree_type: TreeType,
}

impl<S> fmt::Debug for Merk<S> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.debug_struct("Merk").finish()
    }
}

// key, maybe value, maybe child reference hooks, maybe key value storage costs
pub type UseTreeMutResult = CostResult<
    Vec<(
        Vec<u8>,
        Option<(TreeCostType, FeatureSumLength)>,
        ChildrenSizesWithValue,
        KeyValueStorageCost,
    )>,
    Error,
>;

impl<'db, S> Merk<S>
where
    S: StorageContext<'db>,
{
    /// Returns the root hash of the tree (a digest for the entire store which
    /// proofs can be checked against). If the tree is empty, returns the null
    /// hash (zero-filled).
    pub fn root_hash(&self) -> CostContext<CryptoHash> {
        let tree_type = self.tree_type;
        self.use_tree(|tree| {
            tree.map_or(NULL_HASH.wrap_with_cost(Default::default()), |tree| {
                tree.hash_for_link(tree_type)
            })
        })
    }

    /// Returns if the merk has a root tree set
    pub fn has_root_key(&self) -> bool {
        let tree = self.tree.take();
        let res = tree.is_some();
        self.tree.set(tree);
        res
    }

    /// Returns the total aggregate data in the Merk tree
    pub fn aggregate_data(&self) -> Result<AggregateData, Error> {
        self.use_tree(|tree| match tree {
            None => Ok(AggregateData::NoAggregateData),
            Some(tree) => tree.aggregate_data(),
        })
    }

    /// Returns the height of the Merk tree
    pub fn height(&self) -> Option<u8> {
        self.use_tree(|tree| tree.map(|tree| tree.height()))
    }

    /// Returns the root non-prefixed key of the tree. If the tree is empty,
    /// None.
    pub fn root_key(&self) -> Option<Vec<u8>> {
        self.use_tree(|tree| tree.map(|tree| tree.key().to_vec()))
    }

    /// Returns the root hash and non-prefixed key of the tree.
    pub fn root_hash_key_and_aggregate_data(
        &self,
    ) -> CostResult<RootHashKeyAndAggregateData, Error> {
        let tree_type = self.tree_type;
        self.use_tree(|tree| match tree {
            None => Ok((NULL_HASH, None, AggregateData::NoAggregateData))
                .wrap_with_cost(Default::default()),
            Some(tree) => {
                let aggregate_data = cost_return_on_error_default!(tree.aggregate_data());
                tree.hash_for_link(tree_type)
                    .map(|hash| Ok((hash, Some(tree.key().to_vec()), aggregate_data)))
            }
        })
    }

    /// Commit tree changes
    pub fn commit<K>(
        &mut self,
        key_updates: KeyUpdates,
        aux: &AuxMerkBatch<K>,
        options: Option<MerkOptions>,
        old_specialized_cost: &impl Fn(&Vec<u8>, &Vec<u8>) -> Result<u32, Error>,
    ) -> CostResult<(), Error>
    where
        K: AsRef<[u8]>,
    {
        let mut cost = OperationCost::default();
        let options = options.unwrap_or_default();
        let mut batch = self.storage.new_batch();
        let to_batch_wrapped = self.use_tree_mut(|maybe_tree| -> UseTreeMutResult {
            // TODO: concurrent commit
            let mut inner_cost = OperationCost::default();

            if let Some(tree) = maybe_tree {
                // TODO: configurable committer
                let mut committer = MerkCommitter::new(tree.height(), 100);
                cost_return_on_error!(
                    &mut inner_cost,
                    tree.commit(&mut committer, old_specialized_cost)
                );

                let tree_key = tree.key();
                // if they are a base merk we should update the root key
                if self.merk_type.requires_root_storage_update() {
                    // there are two situation where we want to put the root key
                    // it was updated from something else
                    // or it is part of new keys
                    if key_updates.updated_root_key_from.is_some()
                        || key_updates.new_keys.contains(tree_key)
                    {
                        let costs = if self.merk_type == StandaloneMerk {
                            // if we are a standalone merk we want real costs
                            Some(KeyValueStorageCost::for_updated_root_cost(
                                key_updates
                                    .updated_root_key_from
                                    .as_ref()
                                    .map(|k| k.len() as u32),
                                tree_key.len() as u32,
                            ))
                        } else {
                            // if we are a base merk we estimate these costs are free
                            // This None does not guarantee they are free though
                            None
                        };

                        // update pointer to root node
                        cost_return_on_error_no_add!(
                            inner_cost,
                            batch
                                .put_root(ROOT_KEY_KEY, tree_key, costs)
                                .map_err(CostsError)
                        );
                    }
                }

                Ok(committer.batch)
            } else {
                if self.merk_type.requires_root_storage_update() {
                    // empty tree, delete pointer to root
                    let cost = if options.base_root_storage_is_free {
                        Some(KeyValueStorageCost::default()) // don't pay for
                                                             // root costs
                    } else {
                        None // means it will be calculated
                    };
                    batch.delete_root(ROOT_KEY_KEY, cost);
                }

                Ok(vec![])
            }
            .wrap_with_cost(inner_cost)
        });

        let mut to_batch = cost_return_on_error!(&mut cost, to_batch_wrapped);

        // TODO: move this to MerkCommitter impl?
        for (key, maybe_cost) in key_updates.deleted_keys {
            to_batch.push((key, None, None, maybe_cost));
        }
        to_batch.sort_by(|a, b| a.0.cmp(&b.0));
        for (key, maybe_sum_tree_cost, maybe_value, storage_cost) in to_batch {
            if let Some((value, left_size, right_size)) = maybe_value {
                cost_return_on_error_no_add!(
                    cost,
                    batch
                        .put(
                            &key,
                            &value,
                            Some((maybe_sum_tree_cost, left_size, right_size)),
                            Some(storage_cost)
                        )
                        .map_err(CostsError)
                );
            } else {
                batch.delete(&key, Some(storage_cost));
            }
        }

        for (key, value, storage_cost) in aux {
            match value {
                Op::Put(value, ..) => cost_return_on_error_no_add!(
                    cost,
                    batch
                        .put_aux(key, value, storage_cost.clone())
                        .map_err(CostsError)
                ),
                Op::Delete => batch.delete_aux(key, storage_cost.clone()),
                _ => {
                    cost_return_on_error_no_add!(
                        cost,
                        Err(Error::InvalidOperation(
                            "only put and delete allowed for aux storage"
                        ))
                    );
                }
            };
        }

        // write to db
        self.storage
            .commit_batch(batch)
            .map_err(StorageError)
            .add_cost(cost)
    }

    /// Walk
    pub fn walk<'s, T>(&'s self, f: impl FnOnce(Option<RefWalker<MerkSource<'s, S>>>) -> T) -> T {
        let mut tree = self.tree.take();
        let maybe_walker = tree
            .as_mut()
            .map(|tree| RefWalker::new(tree, self.source()));
        let res = f(maybe_walker);
        self.tree.set(tree);
        res
    }

    /// Checks if it's an empty tree
    pub fn is_empty_tree(&self) -> CostContext<bool> {
        let mut iter = self.storage.raw_iter();
        iter.seek_to_first().flat_map(|_| iter.valid().map(|x| !x))
    }

    /// Checks if it's an empty tree excluding exceptions
    pub fn is_empty_tree_except(&self, mut except_keys: BTreeSet<&[u8]>) -> CostContext<bool> {
        let mut cost = OperationCost::default();

        let mut iter = self.storage.raw_iter();
        iter.seek_to_first().unwrap_add_cost(&mut cost);
        while let Some(key) = iter.key().unwrap_add_cost(&mut cost) {
            if except_keys.take(key).is_none() {
                return false.wrap_with_cost(cost);
            }
            iter.next().unwrap_add_cost(&mut cost)
        }
        true.wrap_with_cost(cost)
    }

    /// Use tree
    pub(crate) fn use_tree<T>(&self, f: impl FnOnce(Option<&TreeNode>) -> T) -> T {
        let tree = self.tree.take();
        let res = f(tree.as_ref());
        self.tree.set(tree);
        res
    }

    fn use_tree_mut<T>(&self, mut f: impl FnMut(Option<&mut TreeNode>) -> T) -> T {
        let mut tree = self.tree.take();
        let res = f(tree.as_mut());
        self.tree.set(tree);
        res
    }

    /// Sets the tree's top node (base) key
    /// The base root key should only be used if the Merk tree is independent
    /// Meaning that it doesn't have a parent Merk
    pub fn set_base_root_key(&mut self, key: Option<Vec<u8>>) -> CostResult<(), Error> {
        if let Some(key) = key {
            self.storage
                .put_root(ROOT_KEY_KEY, key.as_slice(), None)
                .map_err(Error::StorageError) // todo: maybe
                                              // change None?
        } else {
            self.storage
                .delete_root(ROOT_KEY_KEY, None)
                .map_err(Error::StorageError) // todo: maybe
                                              // change None?
        }
    }

    /// Loads the Merk from the base root key
    /// The base root key should only be used if the Merk tree is independent
    /// Meaning that it doesn't have a parent Merk
    pub(crate) fn load_base_root(
        &mut self,
        value_defined_cost_fn: Option<
            impl Fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>,
        >,
        grove_version: &GroveVersion,
    ) -> CostResult<(), Error> {
        self.storage
            .get_root(ROOT_KEY_KEY)
            .map(|root_result| root_result.map_err(Error::StorageError))
            .flat_map_ok(|tree_root_key_opt| {
                // In case of successful seek for root key check if it exists
                if let Some(tree_root_key) = tree_root_key_opt {
                    // Trying to build a tree out of it, costs will be accumulated because
                    // `Tree::get` returns `CostContext` and this call happens inside `flat_map_ok`.
                    TreeNode::get(
                        &self.storage,
                        tree_root_key,
                        value_defined_cost_fn,
                        grove_version,
                    )
                    .map_ok(|tree| {
                        if let Some(t) = tree.as_ref() {
                            self.root_tree_key = Cell::new(Some(t.key().to_vec()));
                        }
                        self.tree = Cell::new(tree);
                    })
                } else {
                    Ok(()).wrap_with_cost(Default::default())
                }
            })
    }

    /// Loads the Merk from it's parent root key
    /// The base root key should only be used if the Merk tree is independent
    /// Meaning that it doesn't have a parent Merk
    pub(crate) fn load_root(
        &mut self,
        value_defined_cost_fn: Option<
            impl Fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>,
        >,
        grove_version: &GroveVersion,
    ) -> CostResult<(), Error> {
        // In case of successful seek for root key check if it exists
        if let Some(tree_root_key) = self.root_tree_key.get_mut() {
            // Trying to build a tree out of it, costs will be accumulated because
            // `Tree::get` returns `CostContext` and this call happens inside `flat_map_ok`.
            TreeNode::get(
                &self.storage,
                tree_root_key,
                value_defined_cost_fn,
                grove_version,
            )
            .map_ok(|tree| {
                self.tree = Cell::new(tree);
            })
        } else {
            // The tree is empty
            Ok(()).wrap_with_cost(Default::default())
        }
    }

    /// Verifies the correctness of a merk tree
    /// hash values are computed correctly, heights are accurate and links
    /// consistent with backing store.
    // TODO: define the return types
    pub fn verify(
        &self,
        skip_sum_checks: bool,
        grove_version: &GroveVersion,
    ) -> (BTreeMap<Vec<u8>, CryptoHash>, BTreeMap<Vec<u8>, Vec<u8>>) {
        let tree = self.tree.take();

        let mut bad_link_map: BTreeMap<Vec<u8>, CryptoHash> = BTreeMap::new();
        let mut parent_keys: BTreeMap<Vec<u8>, Vec<u8>> = BTreeMap::new();
        let mut root_traversal_instruction = vec![];

        // TODO: remove clone
        self.verify_tree(
            // TODO: handle unwrap
            &tree.clone().unwrap(),
            &mut root_traversal_instruction,
            &mut bad_link_map,
            &mut parent_keys,
            skip_sum_checks,
            grove_version,
        );
        self.tree.set(tree);

        (bad_link_map, parent_keys)
    }

    fn verify_tree(
        &self,
        tree: &TreeNode,
        traversal_instruction: &mut Vec<bool>,
        bad_link_map: &mut BTreeMap<Vec<u8>, CryptoHash>,
        parent_keys: &mut BTreeMap<Vec<u8>, Vec<u8>>,
        skip_sum_checks: bool,
        grove_version: &GroveVersion,
    ) {
        if let Some(link) = tree.link(LEFT) {
            traversal_instruction.push(LEFT);
            self.verify_link(
                link,
                tree.key(),
                traversal_instruction,
                bad_link_map,
                parent_keys,
                skip_sum_checks,
                grove_version,
            );
            traversal_instruction.pop();
        }

        if let Some(link) = tree.link(RIGHT) {
            traversal_instruction.push(RIGHT);
            self.verify_link(
                link,
                tree.key(),
                traversal_instruction,
                bad_link_map,
                parent_keys,
                skip_sum_checks,
                grove_version,
            );
            traversal_instruction.pop();
        }
    }

    fn verify_link(
        &self,
        link: &Link,
        parent_key: &[u8],
        traversal_instruction: &mut Vec<bool>,
        bad_link_map: &mut BTreeMap<Vec<u8>, CryptoHash>,
        parent_keys: &mut BTreeMap<Vec<u8>, Vec<u8>>,
        skip_sum_checks: bool,
        grove_version: &GroveVersion,
    ) {
        let (hash, key, aggregate_data) = match link {
            Link::Reference {
                hash,
                key,
                aggregate_data,
                ..
            } => (hash.to_owned(), key.to_owned(), aggregate_data.to_owned()),
            Link::Modified { tree, .. } => (
                tree.hash().unwrap(),
                tree.key().to_vec(),
                tree.aggregate_data().unwrap(),
            ),
            Link::Loaded {
                hash,
                child_heights: _,
                aggregate_data,
                tree,
            } => (
                hash.to_owned(),
                tree.key().to_vec(),
                aggregate_data.to_owned(),
            ),
            _ => todo!(),
        };

        let instruction_id = traversal_instruction_as_vec_bytes(traversal_instruction);
        let node = TreeNode::get(
            &self.storage,
            key,
            None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
            grove_version,
        )
        .unwrap();

        if node.is_err() {
            bad_link_map.insert(instruction_id.to_vec(), hash);
            parent_keys.insert(instruction_id.to_vec(), parent_key.to_vec());
            return;
        }

        let node = node.unwrap();
        if node.is_none() {
            bad_link_map.insert(instruction_id.to_vec(), hash);
            parent_keys.insert(instruction_id.to_vec(), parent_key.to_vec());
            return;
        }

        let node = node.unwrap();
        if node.hash().unwrap() != hash {
            bad_link_map.insert(instruction_id.to_vec(), hash);
            parent_keys.insert(instruction_id.to_vec(), parent_key.to_vec());
            return;
        }

        // Need to skip this when restoring a sum tree
        if !skip_sum_checks && node.aggregate_data().unwrap() != aggregate_data {
            bad_link_map.insert(instruction_id.to_vec(), hash);
            parent_keys.insert(instruction_id.to_vec(), parent_key.to_vec());
            return;
        }

        // TODO: check child heights
        // all checks passed, recurse
        self.verify_tree(
            &node,
            traversal_instruction,
            bad_link_map,
            parent_keys,
            skip_sum_checks,
            grove_version,
        );
    }

    /// Performs a trunk query on a count-based tree.
    ///
    /// A trunk query retrieves the top N levels of the tree, with optimal depth
    /// splitting for efficient chunked retrieval of large trees.
    ///
    /// # Arguments
    /// * `max_depth` - Maximum depth per chunk for splitting
    /// * `min_depth` - Optional minimum depth per chunk (for privacy control).
    ///   When provided for ProvableCountTree or ProvableCountSumTree, the first
    ///   chunk depth will be clamped to at least this value, preventing
    ///   information leakage about small subtrees.
    /// * `grove_version` - The grove version for compatibility
    ///
    /// # Returns
    /// A `TrunkQueryResult` containing the proof, chunk depths, tree depth, and
    /// root hash.
    ///
    /// # Errors
    /// Returns an error if:
    /// - The tree type doesn't support count (not CountTree, CountSumTree,
    ///   ProvableCountTree, or ProvableCountSumTree)
    /// - The tree is empty
    pub fn trunk_query(
        &self,
        max_depth: u8,
        min_depth: Option<u8>,
        grove_version: &GroveVersion,
    ) -> CostResult<TrunkQueryResult, Error> {
        let mut cost = OperationCost::default();

        // Verify tree type supports count
        let supports_count = matches!(
            self.tree_type,
            TreeType::CountTree
                | TreeType::CountSumTree
                | TreeType::ProvableCountTree
                | TreeType::ProvableCountSumTree
        );
        if !supports_count {
            return Err(Error::InvalidOperation(
                "trunk_query requires a count tree (CountTree, CountSumTree, ProvableCountTree, \
                 or ProvableCountSumTree)",
            ))
            .wrap_with_cost(cost);
        }

        // Get count from aggregate data
        let aggregate_data = cost_return_on_error_no_add!(cost, self.aggregate_data());
        let count = aggregate_data.as_count_u64();

        if count == 0 {
            return Err(Error::InvalidOperation(
                "trunk_query cannot be performed on an empty tree",
            ))
            .wrap_with_cost(cost);
        }

        // calculate the tree depth
        let tree_depth = calculate_max_tree_depth_from_count(count);

        // For provable count trees with min_depth, use
        // calculate_chunk_depths_with_minimum to ensure privacy by using a
        // minimum depth even for small subtrees
        let is_provable_count_tree = matches!(
            self.tree_type,
            TreeType::ProvableCountTree | TreeType::ProvableCountSumTree
        );
        let chunk_depths = if let Some(min) = min_depth {
            if is_provable_count_tree {
                calculate_chunk_depths_with_minimum(tree_depth, max_depth, min)
            } else {
                calculate_chunk_depths(tree_depth, max_depth)
            }
        } else {
            calculate_chunk_depths(tree_depth, max_depth)
        };

        // Generate proof using create_chunk
        let tree_type = self.tree_type;
        let proof_cost_result = self.walk(|maybe_walker| match maybe_walker {
            None => Err(Error::InvalidOperation(
                "trunk_query cannot be performed on an empty tree",
            ))
            .wrap_with_cost(OperationCost::default()),
            Some(mut walker) => {
                walker.create_chunk(chunk_depths[0] as usize, tree_type, grove_version)
            }
        });

        let proof = match proof_cost_result.unwrap_add_cost(&mut cost) {
            Ok(p) => p,
            Err(e) => return Err(e).wrap_with_cost(cost),
        };

        Ok(TrunkQueryResult {
            proof,
            chunk_depths,
            tree_depth,
        })
        .wrap_with_cost(cost)
    }

    /// Performs a branch query on any tree type.
    ///
    /// A branch query navigates to a specific key in the tree and returns
    /// the subtree rooted at that key, up to a specified depth.
    ///
    /// # Arguments
    /// * `target_key` - The key to navigate to (the root of the returned
    ///   branch)
    /// * `depth` - The depth of the subtree to return
    /// * `grove_version` - The grove version for compatibility
    ///
    /// # Returns
    /// A `BranchQueryResult` containing the proof, branch root key, returned
    /// depth, and branch root hash.
    ///
    /// # Errors
    /// Returns an error if:
    /// - The tree is empty
    /// - The target key is not found
    pub fn branch_query(
        &self,
        target_key: &[u8],
        depth: u8,
        grove_version: &GroveVersion,
    ) -> CostResult<BranchQueryResult, Error> {
        let mut cost = OperationCost::default();

        let result = self.walk(|maybe_walker| {
            let mut walker = match maybe_walker {
                None => {
                    return Err(Error::InvalidOperation(
                        "branch_query cannot be performed on an empty tree",
                    ))
                }
                Some(w) => w,
            };

            // First, find the path to the target key
            let find_result = walker
                .find_key_path(target_key, grove_version)
                .unwrap_add_cost(&mut cost);

            let traversal_path = match find_result {
                Ok(Some(path)) => path,
                Ok(None) => {
                    return Err(Error::PathKeyNotFound(format!(
                        "key {} not found in tree",
                        hex::encode(target_key)
                    )))
                }
                Err(e) => return Err(e),
            };

            // Navigate to the key and get its hash using recursion
            fn get_hash_at_path<S: Fetch + Sized + Clone>(
                walker: &mut RefWalker<'_, S>,
                path: &[bool],
                grove_version: &GroveVersion,
                cost: &mut OperationCost,
            ) -> Result<CryptoHash, Error> {
                if path.is_empty() {
                    return Ok(walker.tree().hash().unwrap_add_cost(cost));
                }

                let go_left = path[0];
                let child_result = walker
                    .walk(
                        go_left,
                        None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                        grove_version,
                    )
                    .unwrap_add_cost(cost);

                match child_result {
                    Ok(Some(mut child)) => {
                        get_hash_at_path(&mut child, &path[1..], grove_version, cost)
                    }
                    Ok(None) => Err(Error::InternalError(
                        "inconsistent tree state during branch_query",
                    )),
                    Err(e) => Err(e),
                }
            }

            let branch_root_hash =
                get_hash_at_path(&mut walker, &traversal_path, grove_version, &mut cost)?;

            // Use traverse_and_build_chunk to generate proof at the target location
            // Note: We need to get a fresh walker since the previous one was consumed
            Ok((traversal_path, branch_root_hash))
        });

        let (traversal_path, branch_root_hash) = match result {
            Ok(r) => r,
            Err(e) => return Err(e).wrap_with_cost(cost),
        };

        // Now use traverse_and_build_chunk to generate the proof
        let tree_type = self.tree_type;
        let proof_cost_result = self.walk(|maybe_walker| match maybe_walker {
            None => Err(Error::InvalidOperation(
                "branch_query cannot be performed on an empty tree",
            ))
            .wrap_with_cost(OperationCost::default()),
            Some(mut walker) => walker.traverse_and_build_chunk(
                &traversal_path,
                depth as usize,
                tree_type,
                grove_version,
            ),
        });

        let proof = match proof_cost_result.unwrap_add_cost(&mut cost) {
            Ok(p) => p,
            Err(e) => return Err(e).wrap_with_cost(cost),
        };

        Ok(BranchQueryResult {
            proof,
            branch_root_key: target_key.to_vec(),
            returned_depth: depth,
            branch_root_hash,
        })
        .wrap_with_cost(cost)
    }
}

fn fetch_node<'db>(
    db: &impl StorageContext<'db>,
    key: &[u8],
    value_defined_cost_fn: Option<impl Fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
    grove_version: &GroveVersion,
) -> Result<Option<TreeNode>, Error> {
    let bytes = db.get(key).unwrap().map_err(StorageError)?; // TODO: get_pinned ?
    if let Some(bytes) = bytes {
        Ok(Some(
            TreeNode::decode(key.to_vec(), &bytes, value_defined_cost_fn, grove_version)
                .map_err(EdError)?,
        ))
    } else {
        Ok(None)
    }
}

// // TODO: get rid of Fetch/source and use GroveDB storage_cost abstraction

#[cfg(test)]
mod test {

    use grovedb_path::SubtreePath;
    use grovedb_storage::{
        rocksdb_storage::{PrefixedRocksDbTransactionContext, RocksDbStorage},
        RawIterator, Storage, StorageBatch, StorageContext,
    };
    use grovedb_version::version::GroveVersion;
    use tempfile::TempDir;

    use super::{Merk, RefWalker};
    use crate::{
        merk::source::MerkSource, test_utils::*, tree::kv::ValueDefinedCostType,
        tree_type::TreeType, Op, TreeFeatureType::BasicMerkNode,
    };
    // TODO: Close and then reopen test

    fn assert_invariants(merk: &TempMerk) {
        merk.use_tree(|maybe_tree| {
            let tree = maybe_tree.expect("expected tree");
            assert_tree_invariants(tree);
        })
    }

    #[test]
    fn simple_insert_apply() {
        let grove_version = GroveVersion::latest();
        let batch_size = 20;
        let mut merk = TempMerk::new(grove_version);
        let batch = make_batch_seq(0..batch_size);
        merk.apply::<_, Vec<_>>(&batch, &[], None, grove_version)
            .unwrap()
            .expect("apply failed");

        assert_invariants(&merk);
        assert_eq!(
            merk.root_hash().unwrap(),
            [
                126, 168, 96, 201, 59, 225, 123, 33, 206, 154, 87, 23, 139, 143, 136, 52, 103, 9,
                218, 90, 71, 153, 240, 47, 227, 168, 1, 104, 239, 237, 140, 147
            ]
        );
    }

    #[test]
    fn tree_height() {
        let grove_version = GroveVersion::latest();
        let mut merk = TempMerk::new(grove_version);
        let batch = make_batch_seq(0..1);
        merk.apply::<_, Vec<_>>(&batch, &[], None, grove_version)
            .unwrap()
            .expect("apply failed");
        assert_eq!(merk.height(), Some(1));

        // height 2
        let mut merk = TempMerk::new(grove_version);
        let batch = make_batch_seq(0..2);
        merk.apply::<_, Vec<_>>(&batch, &[], None, grove_version)
            .unwrap()
            .expect("apply failed");
        assert_eq!(merk.height(), Some(2));

        // height 5
        // 2^5 - 1 = 31 (max number of elements in tree of height 5)
        let mut merk = TempMerk::new(grove_version);
        let batch = make_batch_seq(0..31);
        merk.apply::<_, Vec<_>>(&batch, &[], None, grove_version)
            .unwrap()
            .expect("apply failed");
        assert_eq!(merk.height(), Some(5));

        // should still be height 5 for 29 elements
        let mut merk = TempMerk::new(grove_version);
        let batch = make_batch_seq(0..29);
        merk.apply::<_, Vec<_>>(&batch, &[], None, grove_version)
            .unwrap()
            .expect("apply failed");
        assert_eq!(merk.height(), Some(5));
    }

    #[test]
    fn insert_uncached() {
        let grove_version = GroveVersion::latest();
        let batch_size = 20;
        let mut merk = TempMerk::new(grove_version);

        let batch = make_batch_seq(0..batch_size);
        merk.apply::<_, Vec<_>>(&batch, &[], None, grove_version)
            .unwrap()
            .expect("apply failed");
        assert_invariants(&merk);

        let batch = make_batch_seq(batch_size..(batch_size * 2));
        merk.apply::<_, Vec<_>>(&batch, &[], None, grove_version)
            .unwrap()
            .expect("apply failed");
        assert_invariants(&merk);
    }

    #[test]
    fn insert_two() {
        let grove_version = GroveVersion::latest();
        let tree_size = 2;
        let batch_size = 1;
        let mut merk = TempMerk::new(grove_version);

        for i in 0..(tree_size / batch_size) {
            let batch = make_batch_rand(batch_size, i);
            merk.apply::<_, Vec<_>>(&batch, &[], None, grove_version)
                .unwrap()
                .expect("apply failed");
        }
    }

    #[test]
    fn insert_rand() {
        let grove_version = GroveVersion::latest();
        let tree_size = 40;
        let batch_size = 4;
        let mut merk = TempMerk::new(grove_version);

        for i in 0..(tree_size / batch_size) {
            println!("i:{i}");
            let batch = make_batch_rand(batch_size, i);
            merk.apply::<_, Vec<_>>(&batch, &[], None, grove_version)
                .unwrap()
                .expect("apply failed");
        }
    }

    #[test]
    fn actual_deletes() {
        let grove_version = GroveVersion::latest();
        let mut merk = TempMerk::new(grove_version);

        let batch = make_batch_rand(10, 1);
        merk.apply::<_, Vec<_>>(&batch, &[], None, grove_version)
            .unwrap()
            .expect("apply failed");

        let key = batch.first().unwrap().0.clone();
        merk.apply::<_, Vec<_>>(&[(key.clone(), Op::Delete)], &[], None, grove_version)
            .unwrap()
            .unwrap();

        let value = merk.storage.get(key.as_slice()).unwrap().unwrap();
        assert!(value.is_none());
    }

    #[test]
    fn aux_data() {
        let grove_version = GroveVersion::latest();
        let mut merk = TempMerk::new(grove_version);
        merk.apply::<Vec<_>, _>(
            &[],
            &[(vec![1, 2, 3], Op::Put(vec![4, 5, 6], BasicMerkNode), None)],
            None,
            grove_version,
        )
        .unwrap()
        .expect("apply failed");
        merk.commit(grove_version);

        let val = merk.get_aux(&[1, 2, 3]).unwrap().unwrap();
        assert_eq!(val, Some(vec![4, 5, 6]));
    }

    #[test]
    fn get_not_found() {
        let grove_version = GroveVersion::latest();
        let mut merk = TempMerk::new(grove_version);

        // no root
        assert!(merk
            .get(
                &[1, 2, 3],
                true,
                None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                grove_version
            )
            .unwrap()
            .unwrap()
            .is_none());

        // cached
        merk.apply::<_, Vec<_>>(
            &[(vec![5, 5, 5], Op::Put(vec![], BasicMerkNode))],
            &[],
            None,
            grove_version,
        )
        .unwrap()
        .unwrap();
        assert!(merk
            .get(
                &[1, 2, 3],
                true,
                None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                grove_version
            )
            .unwrap()
            .unwrap()
            .is_none());

        // uncached
        merk.apply::<_, Vec<_>>(
            &[
                (vec![0, 0, 0], Op::Put(vec![], BasicMerkNode)),
                (vec![1, 1, 1], Op::Put(vec![], BasicMerkNode)),
                (vec![2, 2, 2], Op::Put(vec![], BasicMerkNode)),
            ],
            &[],
            None,
            grove_version,
        )
        .unwrap()
        .unwrap();
        assert!(merk
            .get(
                &[3, 3, 3],
                true,
                None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                grove_version
            )
            .unwrap()
            .unwrap()
            .is_none());
    }

    // TODO: what this test should do?
    #[test]
    fn reopen_check_root_hash() {
        let grove_version = GroveVersion::latest();
        let tmp_dir = TempDir::new().expect("cannot open tempdir");
        let storage = RocksDbStorage::default_rocksdb_with_path(tmp_dir.path())
            .expect("cannot open rocksdb storage");
        let transaction = storage.start_transaction();

        let mut merk = Merk::open_base(
            storage
                .get_transactional_storage_context(SubtreePath::empty(), None, &transaction)
                .unwrap(),
            TreeType::NormalTree,
            None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
            grove_version,
        )
        .unwrap()
        .expect("cannot open merk");
        let batch = make_batch_seq(1..10);
        merk.apply::<_, Vec<_>>(batch.as_slice(), &[], None, grove_version)
            .unwrap()
            .unwrap();
        let batch = make_batch_seq(11..12);
        merk.apply::<_, Vec<_>>(batch.as_slice(), &[], None, grove_version)
            .unwrap()
            .unwrap();
    }

    #[test]
    fn test_get_node_cost() {
        let grove_version = GroveVersion::latest();
        let tmp_dir = TempDir::new().expect("cannot open tempdir");
        let storage = RocksDbStorage::default_rocksdb_with_path(tmp_dir.path())
            .expect("cannot open rocksdb storage");
        let transaction = storage.start_transaction();

        let mut merk = Merk::open_base(
            storage
                .get_transactional_storage_context(SubtreePath::empty(), None, &transaction)
                .unwrap(),
            TreeType::NormalTree,
            None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
            grove_version,
        )
        .unwrap()
        .expect("cannot open merk");
        let batch = make_batch_seq(1..10);
        merk.apply::<_, Vec<_>>(batch.as_slice(), &[], None, grove_version)
            .unwrap()
            .unwrap();
        drop(merk);
    }

    #[test]
    fn reopen() {
        let grove_version = GroveVersion::latest();
        fn collect(
            mut node: RefWalker<MerkSource<PrefixedRocksDbTransactionContext>>,
            nodes: &mut Vec<Vec<u8>>,
        ) {
            let grove_version = GroveVersion::latest();
            nodes.push(node.tree().encode());
            if let Some(c) = node
                .walk(
                    true,
                    None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                    grove_version,
                )
                .unwrap()
                .unwrap()
            {
                collect(c, nodes);
            }
            if let Some(c) = node
                .walk(
                    false,
                    None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                    grove_version,
                )
                .unwrap()
                .unwrap()
            {
                collect(c, nodes);
            }
        }

        let tmp_dir = TempDir::new().expect("cannot open tempdir");

        let original_nodes = {
            let storage = RocksDbStorage::default_rocksdb_with_path(tmp_dir.path())
                .expect("cannot open rocksdb storage");
            let batch = StorageBatch::new();
            let transaction = storage.start_transaction();

            let mut merk = Merk::open_base(
                storage
                    .get_transactional_storage_context(
                        SubtreePath::empty(),
                        Some(&batch),
                        &transaction,
                    )
                    .unwrap(),
                TreeType::NormalTree,
                None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                grove_version,
            )
            .unwrap()
            .expect("cannot open merk");
            let merk_batch = make_batch_seq(1..10_000);
            merk.apply::<_, Vec<_>>(merk_batch.as_slice(), &[], None, grove_version)
                .unwrap()
                .unwrap();

            storage
                .commit_multi_context_batch(batch, Some(&transaction))
                .unwrap()
                .expect("cannot commit batch");

            let merk = Merk::open_base(
                storage
                    .get_transactional_storage_context(SubtreePath::empty(), None, &transaction)
                    .unwrap(),
                TreeType::NormalTree,
                None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                grove_version,
            )
            .unwrap()
            .expect("cannot open merk");

            let mut tree = merk.tree.take().unwrap();
            let walker = RefWalker::new(&mut tree, merk.source());

            let mut nodes = vec![];
            collect(walker, &mut nodes);

            storage.commit_transaction(transaction).unwrap().unwrap();

            nodes
        };

        let storage = RocksDbStorage::default_rocksdb_with_path(tmp_dir.path())
            .expect("cannot open rocksdb storage");
        let transaction = storage.start_transaction();

        let merk = Merk::open_base(
            storage
                .get_transactional_storage_context(SubtreePath::empty(), None, &transaction)
                .unwrap(),
            TreeType::NormalTree,
            None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
            grove_version,
        )
        .unwrap()
        .expect("cannot open merk");
        let mut tree = merk.tree.take().unwrap();
        let walker = RefWalker::new(&mut tree, merk.source());

        let mut reopen_nodes = vec![];
        collect(walker, &mut reopen_nodes);

        assert_eq!(reopen_nodes, original_nodes);
    }

    type PrefixedStorageIter<'db, 'ctx> =
        &'ctx mut <PrefixedRocksDbTransactionContext<'db> as StorageContext<'db>>::RawIterator;

    #[test]
    fn reopen_iter() {
        let grove_version = GroveVersion::latest();
        fn collect(iter: PrefixedStorageIter<'_, '_>, nodes: &mut Vec<(Vec<u8>, Vec<u8>)>) {
            while iter.valid().unwrap() {
                nodes.push((
                    iter.key().unwrap().unwrap().to_vec(),
                    iter.value().unwrap().unwrap().to_vec(),
                ));
                iter.next().unwrap();
            }
        }
        let tmp_dir = TempDir::new().expect("cannot open tempdir");

        let original_nodes = {
            let storage = RocksDbStorage::default_rocksdb_with_path(tmp_dir.path())
                .expect("cannot open rocksdb storage");
            let batch = StorageBatch::new();
            let transaction = storage.start_transaction();

            let mut merk = Merk::open_base(
                storage
                    .get_transactional_storage_context(
                        SubtreePath::empty(),
                        Some(&batch),
                        &transaction,
                    )
                    .unwrap(),
                TreeType::NormalTree,
                None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                grove_version,
            )
            .unwrap()
            .expect("cannot open merk");
            let merk_batch = make_batch_seq(1..10_000);
            merk.apply::<_, Vec<_>>(merk_batch.as_slice(), &[], None, grove_version)
                .unwrap()
                .unwrap();

            storage
                .commit_multi_context_batch(batch, Some(&transaction))
                .unwrap()
                .expect("cannot commit batch");

            let mut nodes = vec![];
            let merk = Merk::open_base(
                storage
                    .get_transactional_storage_context(SubtreePath::empty(), None, &transaction)
                    .unwrap(),
                TreeType::NormalTree,
                None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                grove_version,
            )
            .unwrap()
            .expect("cannot open merk");
            collect(&mut merk.storage.raw_iter(), &mut nodes);

            storage.commit_transaction(transaction).unwrap().unwrap();

            nodes
        };

        let storage = RocksDbStorage::default_rocksdb_with_path(tmp_dir.path())
            .expect("cannot open rocksdb storage");
        let transaction = storage.start_transaction();
        let merk = Merk::open_base(
            storage
                .get_transactional_storage_context(SubtreePath::empty(), None, &transaction)
                .unwrap(),
            TreeType::NormalTree,
            None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
            grove_version,
        )
        .unwrap()
        .expect("cannot open merk");

        let mut reopen_nodes = vec![];
        collect(&mut merk.storage.raw_iter(), &mut reopen_nodes);

        assert_eq!(reopen_nodes, original_nodes);
    }

    #[test]
    fn update_node() {
        let grove_version = GroveVersion::latest();
        let tmp_dir = TempDir::new().expect("cannot open tempdir");
        let storage = RocksDbStorage::default_rocksdb_with_path(tmp_dir.path())
            .expect("cannot open rocksdb storage");
        let batch = StorageBatch::new();
        let transaction = storage.start_transaction();

        let mut merk = Merk::open_base(
            storage
                .get_transactional_storage_context(SubtreePath::empty(), Some(&batch), &transaction)
                .unwrap(),
            TreeType::NormalTree,
            None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
            grove_version,
        )
        .unwrap()
        .expect("cannot open merk");

        merk.apply::<_, Vec<_>>(
            &[(b"9".to_vec(), Op::Put(b"a".to_vec(), BasicMerkNode))],
            &[],
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert successfully");
        merk.apply::<_, Vec<_>>(
            &[(b"10".to_vec(), Op::Put(b"a".to_vec(), BasicMerkNode))],
            &[],
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert successfully");

        let result = merk
            .get(
                b"10".as_slice(),
                true,
                None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                grove_version,
            )
            .unwrap()
            .expect("should get successfully");
        assert_eq!(result, Some(b"a".to_vec()));

        // Update the node
        merk.apply::<_, Vec<_>>(
            &[(b"10".to_vec(), Op::Put(b"b".to_vec(), BasicMerkNode))],
            &[],
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert successfully");
        let result = merk
            .get(
                b"10".as_slice(),
                true,
                None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                grove_version,
            )
            .unwrap()
            .expect("should get successfully");
        assert_eq!(result, Some(b"b".to_vec()));

        storage
            .commit_multi_context_batch(batch, Some(&transaction))
            .unwrap()
            .expect("cannot commit batch");

        let mut merk = Merk::open_base(
            storage
                .get_transactional_storage_context(SubtreePath::empty(), None, &transaction)
                .unwrap(),
            TreeType::NormalTree,
            None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
            grove_version,
        )
        .unwrap()
        .expect("cannot open merk");

        // Update the node after dropping merk
        merk.apply::<_, Vec<_>>(
            &[(b"10".to_vec(), Op::Put(b"c".to_vec(), BasicMerkNode))],
            &[],
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert successfully");
        let result = merk
            .get(
                b"10".as_slice(),
                true,
                None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                grove_version,
            )
            .unwrap()
            .expect("should get successfully");
        assert_eq!(result, Some(b"c".to_vec()));
    }

    fn make_count_batch_seq(range: std::ops::Range<u64>) -> Vec<(Vec<u8>, crate::Op)> {
        use crate::TreeFeatureType::CountedMerkNode;
        range
            .map(|n| {
                (
                    n.to_be_bytes().to_vec(),
                    crate::Op::Put(vec![123; 60], CountedMerkNode(1)),
                )
            })
            .collect()
    }

    #[test]
    fn test_trunk_query_on_count_tree() {
        let grove_version = GroveVersion::latest();
        let mut merk = TempMerk::new_with_tree_type(grove_version, TreeType::CountTree);

        // Insert some elements to create a tree with count
        // Use CountedMerkNode feature type for count trees
        let batch = make_count_batch_seq(0..15); // 15 elements should give depth 4
        merk.apply::<_, Vec<_>>(&batch, &[], None, grove_version)
            .unwrap()
            .expect("apply failed");

        // Trunk query should succeed on count tree
        let result = merk.trunk_query(8, None, grove_version).unwrap();
        if let Err(ref e) = result {
            eprintln!("trunk_query error: {:?}", e);
        }
        assert!(
            result.is_ok(),
            "trunk_query should succeed on CountTree: {:?}",
            result.err()
        );

        let trunk_result = result.unwrap();
        assert!(!trunk_result.proof.is_empty(), "proof should not be empty");
        assert!(trunk_result.tree_depth > 0, "tree depth should be > 0");
        assert!(
            !trunk_result.chunk_depths.is_empty(),
            "chunk depths should not be empty"
        );
        // Chunk depths should sum to tree depth
        let sum: u8 = trunk_result.chunk_depths.iter().sum();
        assert_eq!(
            sum, trunk_result.tree_depth,
            "chunk depths should sum to tree depth"
        );
    }

    #[test]
    fn test_trunk_query_fails_on_normal_tree() {
        let grove_version = GroveVersion::latest();
        let mut merk = TempMerk::new(grove_version);

        // Insert some elements
        let batch = make_batch_seq(0..10);
        merk.apply::<_, Vec<_>>(&batch, &[], None, grove_version)
            .unwrap()
            .expect("apply failed");

        // Trunk query should fail on normal tree
        let result = merk.trunk_query(8, None, grove_version).unwrap();
        assert!(result.is_err(), "trunk_query should fail on NormalTree");
    }

    #[test]
    fn test_branch_query_on_normal_tree() {
        let grove_version = GroveVersion::latest();
        let mut merk = TempMerk::new(grove_version);

        // Insert elements 0-14
        let batch = make_batch_seq(0..15);
        merk.apply::<_, Vec<_>>(&batch, &[], None, grove_version)
            .unwrap()
            .expect("apply failed");

        // Query for an element that exists
        // The key for element 7 is a byte representation of 7
        let key = 7u64.to_be_bytes().to_vec();

        let result = merk.branch_query(&key, 2, grove_version).unwrap();
        assert!(
            result.is_ok(),
            "branch_query should succeed for existing key"
        );

        let branch_result = result.unwrap();
        assert!(!branch_result.proof.is_empty(), "proof should not be empty");
        assert_eq!(
            branch_result.branch_root_key, key,
            "branch root key should match target"
        );
        assert_eq!(
            branch_result.returned_depth, 2,
            "returned depth should match requested"
        );
    }

    #[test]
    fn test_branch_query_key_not_found() {
        let grove_version = GroveVersion::latest();
        let mut merk = TempMerk::new(grove_version);

        // Insert elements 0-9
        let batch = make_batch_seq(0..10);
        merk.apply::<_, Vec<_>>(&batch, &[], None, grove_version)
            .unwrap()
            .expect("apply failed");

        // Query for an element that doesn't exist
        let key = vec![255, 255, 255]; // A key that doesn't exist

        let result = merk.branch_query(&key, 2, grove_version).unwrap();
        assert!(
            result.is_err(),
            "branch_query should fail for non-existent key"
        );
    }

    #[test]
    fn test_trunk_query_empty_tree_fails() {
        let grove_version = GroveVersion::latest();
        let merk = TempMerk::new_with_tree_type(grove_version, TreeType::CountTree);

        // Trunk query should fail on empty tree
        let result = merk.trunk_query(8, None, grove_version).unwrap();
        assert!(result.is_err(), "trunk_query should fail on empty tree");
    }

    #[test]
    fn test_branch_query_empty_tree_fails() {
        let grove_version = GroveVersion::latest();
        let merk = TempMerk::new(grove_version);

        // Branch query should fail on empty tree
        let result = merk.branch_query(&[1, 2, 3], 2, grove_version).unwrap();
        assert!(result.is_err(), "branch_query should fail on empty tree");
    }
}
