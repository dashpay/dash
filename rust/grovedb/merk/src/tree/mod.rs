//! Merk trees

#[cfg(feature = "minimal")]
mod commit;
#[cfg(feature = "minimal")]
mod debug;
#[cfg(feature = "minimal")]
mod encoding;
#[cfg(feature = "minimal")]
mod fuzz_tests;
#[cfg(any(feature = "minimal", feature = "verify"))]
pub mod hash;
#[cfg(feature = "minimal")]
mod iter;
#[cfg(feature = "minimal")]
mod just_in_time_value_update;
#[cfg(feature = "minimal")]
pub mod kv;
#[cfg(feature = "minimal")]
mod link;
#[cfg(feature = "minimal")]
mod ops;
#[cfg(any(feature = "minimal", feature = "verify"))]
mod tree_feature_type;
#[cfg(feature = "minimal")]
mod walk;

#[cfg(feature = "minimal")]
use std::cmp::{max, Ordering};

#[cfg(feature = "minimal")]
pub use commit::{Commit, NoopCommit};
#[cfg(feature = "minimal")]
use ed::{Decode, Encode, Terminated};
#[cfg(feature = "minimal")]
use grovedb_costs::{
    cost_return_on_error, cost_return_on_error_default, cost_return_on_error_no_add,
    storage_cost::{
        key_value_cost::KeyValueStorageCost,
        removal::{StorageRemovedBytes, StorageRemovedBytes::BasicStorageRemoval},
        StorageCost,
    },
    CostContext, CostResult, CostsExt, OperationCost,
};
#[cfg(feature = "minimal")]
use grovedb_version::version::GroveVersion;
#[cfg(any(feature = "minimal", feature = "verify"))]
pub use hash::{
    combine_hash, kv_digest_to_kv_hash, kv_hash, node_hash, node_hash_with_count, value_hash,
    CryptoHash, HASH_LENGTH, NULL_HASH,
};
#[cfg(feature = "minimal")]
pub use hash::{HASH_BLOCK_SIZE, HASH_BLOCK_SIZE_U32, HASH_LENGTH_U32, HASH_LENGTH_U32_X2};
#[cfg(feature = "minimal")]
use integer_encoding::VarInt;
#[cfg(feature = "minimal")]
use kv::KV;
#[cfg(feature = "minimal")]
pub use link::Link;
#[cfg(feature = "minimal")]
pub use ops::{AuxMerkBatch, BatchEntry, MerkBatch, Op, PanicSource};
#[cfg(feature = "minimal")]
pub use tree_feature_type::AggregateData;
#[cfg(any(feature = "minimal", feature = "verify"))]
pub use tree_feature_type::TreeFeatureType;
#[cfg(feature = "minimal")]
pub use walk::{Fetch, RefWalker, Walker};

#[cfg(feature = "minimal")]
use crate::merk::NodeType;
#[cfg(feature = "minimal")]
use crate::tree::hash::HASH_LENGTH_X2;
#[cfg(feature = "minimal")]
use crate::tree::kv::ValueDefinedCostType;
#[cfg(feature = "minimal")]
use crate::tree::kv::ValueDefinedCostType::{LayeredValueDefinedCost, SpecializedValueDefinedCost};
#[cfg(feature = "minimal")]
use crate::{error::Error, tree_type::TreeType, Error::Overflow};
// TODO: remove need for `TreeInner`, and just use `Box<Self>` receiver for
// relevant methods

#[cfg(feature = "minimal")]
/// The fields of the `Tree` type, stored on the heap.
#[derive(Clone, Encode, Decode, Debug, PartialEq)]
pub struct TreeNodeInner {
    pub(crate) left: Option<Link>,
    pub(crate) right: Option<Link>,
    pub(crate) kv: KV,
}

#[cfg(feature = "minimal")]
impl TreeNodeInner {
    /// Get the value as owned of the key value struct
    pub fn value_as_owned(self) -> Vec<u8> {
        self.kv.value
    }

    /// Get the value as owned of the key value struct
    pub fn value_as_owned_with_feature(self) -> (Vec<u8>, TreeFeatureType) {
        (self.kv.value, self.kv.feature_type)
    }

    /// Get the value as slice of the key value struct
    pub fn value_as_slice(&self) -> &[u8] {
        self.kv.value.as_slice()
    }

    /// Get the key as owned of the key value struct
    pub fn key_as_owned(self) -> Vec<u8> {
        self.kv.key
    }

    /// Get the key as slice of the key value struct
    pub fn key_as_slice(&self) -> &[u8] {
        self.kv.key.as_slice()
    }
}

#[cfg(feature = "minimal")]
impl Terminated for Box<TreeNodeInner> {}

#[cfg(feature = "minimal")]
/// A binary AVL tree data structure, with Merkle hashes.
///
/// Trees' inner fields are stored on the heap so that nodes can recursively
/// link to each other, and so we can detach nodes from their parents, then
/// reattach without allocating or freeing heap memory.
#[derive(Clone, PartialEq)]
pub struct TreeNode {
    pub(crate) inner: Box<TreeNodeInner>,
    pub(crate) old_value: Option<Vec<u8>>,
    pub(crate) known_storage_cost: Option<KeyValueStorageCost>,
}

#[cfg(feature = "minimal")]
impl TreeNode {
    /// Creates a new `Tree` with the given key and value, and no children.
    ///
    /// Hashes the key/value pair and initializes the `kv_hash` field.
    pub fn new(
        key: Vec<u8>,
        value: Vec<u8>,
        value_defined_cost: Option<ValueDefinedCostType>,
        feature_type: TreeFeatureType,
    ) -> CostContext<Self> {
        KV::new(key, value, value_defined_cost, feature_type).map(|kv| Self {
            inner: Box::new(TreeNodeInner {
                kv,
                left: None,
                right: None,
            }),
            old_value: None,
            known_storage_cost: None,
        })
    }

    /// Creates a new `Tree` given an inner tree
    pub fn new_with_tree_inner(inner_tree: TreeNodeInner) -> Self {
        let old_value = inner_tree.kv.value.clone();
        Self {
            inner: Box::new(inner_tree),
            old_value: Some(old_value),
            known_storage_cost: None,
        }
    }

    /// the node type
    pub fn node_type(&self) -> NodeType {
        self.inner.kv.feature_type.node_type()
    }

    pub fn storage_cost_for_update(current_value_byte_cost: u32, old_cost: u32) -> StorageCost {
        let mut value_storage_cost = StorageCost {
            ..Default::default()
        };

        // Update `StorageCost` for value
        match old_cost.cmp(&current_value_byte_cost) {
            Ordering::Equal => {
                value_storage_cost.replaced_bytes += old_cost;
            }
            Ordering::Greater => {
                // old size is greater than current size, storage_cost will be freed
                value_storage_cost.replaced_bytes += current_value_byte_cost;
                value_storage_cost.removed_bytes +=
                    BasicStorageRemoval(old_cost - current_value_byte_cost);
            }
            Ordering::Less => {
                // current size is greater than old size, storage_cost will be created
                // this also handles the case where the tree.old_size = 0
                value_storage_cost.replaced_bytes += old_cost;
                value_storage_cost.added_bytes += current_value_byte_cost - old_cost;
            }
        }
        value_storage_cost
    }

    /// Compare current value byte cost with old cost and return
    /// current value byte cost with updated `KeyValueStorageCost`
    pub fn kv_with_parent_hook_size_and_storage_cost_from_old_cost(
        &self,
        current_value_byte_cost: u32,
        old_cost: u32,
    ) -> Result<(u32, KeyValueStorageCost), Error> {
        let key_storage_cost = StorageCost {
            ..Default::default()
        };
        let value_storage_cost = Self::storage_cost_for_update(current_value_byte_cost, old_cost);

        let key_value_storage_cost = KeyValueStorageCost {
            key_storage_cost, // the key storage cost is added later
            value_storage_cost,
            new_node: self.old_value.is_none(),
            needs_value_verification: self.inner.kv.value_defined_cost.is_none(),
        };

        Ok((current_value_byte_cost, key_value_storage_cost))
    }

    /// Get current value byte cost and old value byte cost and
    /// compare and return current value byte cost with updated
    /// `KeyValueStorageCost`
    pub fn kv_with_parent_hook_size_and_storage_cost(
        &self,
        old_tree_cost: &impl Fn(&Vec<u8>, &Vec<u8>) -> Result<u32, Error>,
    ) -> Result<(u32, KeyValueStorageCost), Error> {
        let current_value_byte_cost = self.value_encoding_length_with_parent_to_child_reference();

        let old_cost = if let Some(old_value) = self.old_value.as_ref() {
            old_tree_cost(self.key_as_ref(), old_value)
        } else {
            Ok(0) // there was no old value, hence old cost would be 0
        }?;

        self.kv_with_parent_hook_size_and_storage_cost_from_old_cost(
            current_value_byte_cost,
            old_cost,
        )
    }

    /// The point of this function is to get the cost change when we create a
    /// temp value that's a partial merger between the old value and the new
    /// value. Basically it is the new value with the old values flags
    /// For example if we had an old value "Sam" with 40 bytes of flags
    /// and a new value "Samuel" with 2 bytes of flags, the cost is probably
    /// going to go up, As when we merge we will have Samuel with at least
    /// 40 bytes of flags/
    pub fn kv_with_parent_hook_size_and_storage_cost_change_for_value(
        &self,
        old_tree_cost: &impl Fn(&Vec<u8>, &Vec<u8>) -> Result<u32, Error>,
        value: Option<Vec<u8>>,
    ) -> Result<(u32, KeyValueStorageCost), Error> {
        let current_value_byte_cost = if let Some(value_cost) = &self.inner.kv.value_defined_cost {
            self.inner.kv.predefined_value_byte_cost_size(value_cost)
        } else if let Some(value) = value {
            let key_len = self.inner.kv.key.len() as u32;
            let value_len =
                HASH_LENGTH_X2 + value.len() + self.inner.kv.feature_type.encoding_cost();
            KV::value_byte_cost_size_for_key_and_value_lengths(
                key_len,
                value_len as u32,
                self.inner.kv.feature_type.node_type(),
            )
        } else {
            self.inner.kv.value_byte_cost_size()
        };

        let old_cost = if let Some(old_value) = self.old_value.as_ref() {
            old_tree_cost(self.key_as_ref(), old_value)
        } else {
            Ok(0) // there was no old value, hence old cost would be 0
        }?;

        self.kv_with_parent_hook_size_and_storage_cost_from_old_cost(
            current_value_byte_cost,
            old_cost,
        )
    }

    /// Creates a new `Tree` with the given key, value and value hash, and no
    /// children.
    ///
    /// Hashes the key/value pair and initializes the `kv_hash` field.
    pub fn new_with_value_hash(
        key: Vec<u8>,
        value: Vec<u8>,
        value_hash: CryptoHash,
        feature_type: TreeFeatureType,
    ) -> CostContext<Self> {
        KV::new_with_value_hash(key, value, value_hash, feature_type).map(|kv| Self {
            inner: Box::new(TreeNodeInner {
                kv,
                left: None,
                right: None,
            }),
            old_value: None,
            known_storage_cost: None,
        })
    }

    /// Creates a new `Tree` with the given key, value and value hash, and no
    /// children.
    /// Sets the tree's value_hash = hash(value, supplied_value_hash)
    pub fn new_with_combined_value_hash(
        key: Vec<u8>,
        value: Vec<u8>,
        value_hash: CryptoHash,
        feature_type: TreeFeatureType,
    ) -> CostContext<Self> {
        KV::new_with_combined_value_hash(key, value, value_hash, feature_type).map(|kv| Self {
            inner: Box::new(TreeNodeInner {
                kv,
                left: None,
                right: None,
            }),
            old_value: None,
            known_storage_cost: None,
        })
    }

    /// Creates a new `Tree` with the given key, value, value cost and value
    /// hash, and no children.
    /// Sets the tree's value_hash = hash(value, supplied_value_hash)
    pub fn new_with_layered_value_hash(
        key: Vec<u8>,
        value: Vec<u8>,
        value_cost: u32,
        value_hash: CryptoHash,
        feature_type: TreeFeatureType,
    ) -> CostContext<Self> {
        KV::new_with_layered_value_hash(key, value, value_cost, value_hash, feature_type).map(
            |kv| Self {
                inner: Box::new(TreeNodeInner {
                    kv,
                    left: None,
                    right: None,
                }),
                old_value: None,
                known_storage_cost: None,
            },
        )
    }

    /// Creates a `Tree` by supplying all the raw struct fields (mainly useful
    /// for testing). The `kv_hash` and `Link`s are not ensured to be correct.
    pub fn from_fields(
        key: Vec<u8>,
        value: Vec<u8>,
        kv_hash: CryptoHash,
        left: Option<Link>,
        right: Option<Link>,
        feature_type: TreeFeatureType,
    ) -> CostContext<Self> {
        value_hash(value.as_slice()).map(|vh| Self {
            inner: Box::new(TreeNodeInner {
                kv: KV::from_fields(key, value, kv_hash, vh, feature_type),
                left,
                right,
            }),
            old_value: None,
            known_storage_cost: None,
        })
    }

    /// Returns the root node's key as a slice.
    #[inline]
    pub fn key(&self) -> &[u8] {
        self.inner.kv.key()
    }

    /// Returns the root node's feature type
    #[inline]
    pub fn feature_type(&self) -> TreeFeatureType {
        self.inner.kv.feature_type
    }

    /// Returns the root node's key as a slice.
    #[inline]
    pub fn key_as_ref(&self) -> &Vec<u8> {
        self.inner.kv.key_as_ref()
    }

    /// Set key of Tree
    pub fn set_key(&mut self, key: Vec<u8>) {
        self.inner.kv.key = key;
    }

    /// Set value of Tree
    pub fn set_value(&mut self, value: Vec<u8>) {
        self.inner.kv.value = value;
    }

    /// Consumes the tree and returns its root node's key, without having to
    /// clone or allocate.
    #[inline]
    pub fn take_key(self) -> Vec<u8> {
        self.inner.kv.take_key()
    }

    /// Returns the root node's value as a ref.
    #[inline]
    pub fn value_ref(&self) -> &Vec<u8> {
        self.inner.kv.value.as_ref()
    }

    /// Returns the root node's value as a ref.
    #[inline]
    pub fn value_mut_ref(&mut self) -> &mut Vec<u8> {
        &mut self.inner.kv.value
    }

    /// Returns the root node's value as a slice.
    #[inline]
    pub fn value_as_slice(&self) -> &[u8] {
        self.inner.kv.value_as_slice()
    }

    /// Returns the hash of the root node's key/value pair.
    #[inline]
    pub const fn kv_hash(&self) -> &CryptoHash {
        self.inner.kv.hash()
    }

    /// Returns the hash of the node's valu
    #[inline]
    pub const fn value_hash(&self) -> &CryptoHash {
        self.inner.kv.value_hash()
    }

    /// Returns a reference to the root node's `Link` on the given side, if any.
    /// If there is no child, returns `None`.
    #[inline]
    pub const fn link(&self, left: bool) -> Option<&Link> {
        if left {
            self.inner.left.as_ref()
        } else {
            self.inner.right.as_ref()
        }
    }

    /// Returns a mutable reference to the root node's `Link` on the given side,
    /// if any. If there is no child, returns `None`.
    #[inline]
    pub fn link_mut(&mut self, left: bool) -> Option<&mut Link> {
        if left {
            self.inner.left.as_mut()
        } else {
            self.inner.right.as_mut()
        }
    }

    /// Returns a the size of node's child key and sum on the given side, if
    /// any. If there is no child, returns `None`.
    pub fn child_ref_and_sum_size(&self, left: bool) -> Option<(u32, u32)> {
        self.link(left).map(|link| {
            (
                // 36 = 32 Hash + 1 key length + 2 child heights + 1 feature type
                link.key().len() as u32 + 36,
                match link.aggregate_data() {
                    AggregateData::NoAggregateData => 0,
                    AggregateData::Sum(s) => s.encode_var_vec().len() as u32,
                    AggregateData::BigSum(_) => 16_u32,
                    AggregateData::Count(c) => c.encode_var_vec().len() as u32,
                    AggregateData::CountAndSum(c, s) => {
                        s.encode_var_vec().len() as u32 + c.encode_var_vec().len() as u32
                    }
                    AggregateData::ProvableCount(c) => c.encode_var_vec().len() as u32,
                    AggregateData::ProvableCountAndSum(c, s) => {
                        s.encode_var_vec().len() as u32 + c.encode_var_vec().len() as u32
                    }
                },
            )
        })
    }

    /// Returns a reference to the root node's child on the given side, if any.
    /// If there is no child, returns `None`.
    #[inline]
    pub const fn child(&self, left: bool) -> Option<&Self> {
        match self.link(left) {
            None => None,
            Some(link) => link.tree(),
        }
    }

    /// Returns a mutable reference to the root node's child on the given side,
    /// if any. If there is no child, returns `None`.
    #[inline]
    pub fn child_mut(&mut self, left: bool) -> Option<&mut Self> {
        match self.slot_mut(left).as_mut() {
            None => None,
            Some(Link::Reference { .. }) => None,
            Some(Link::Modified { tree, .. }) => Some(tree),
            Some(Link::Uncommitted { tree, .. }) => Some(tree),
            Some(Link::Loaded { tree, .. }) => Some(tree),
        }
    }

    /// Returns the hash of the root node's child on the given side, if any. If
    /// there is no child, returns the null hash (zero-filled).
    #[inline]
    pub const fn child_hash(&self, left: bool) -> &CryptoHash {
        match self.link(left) {
            Some(link) => link.hash(),
            _ => &NULL_HASH,
        }
    }

    /// Returns the sum of the root node's child on the given side, if any. If
    /// there is no child, returns 0.
    #[inline]
    pub fn child_aggregate_sum_data_as_i64(&self, left: bool) -> Result<i64, Error> {
        match self.link(left) {
            Some(link) => match link.aggregate_data() {
                AggregateData::NoAggregateData => Ok(0),
                AggregateData::Sum(s) => Ok(s),
                AggregateData::BigSum(_) => Err(Error::BigSumTreeUnderNormalSumTree(
                    "for aggregate data as i64".to_string(),
                )),
                AggregateData::Count(_) => Ok(0),
                AggregateData::CountAndSum(_, s) => Ok(s),
                AggregateData::ProvableCount(_) => Ok(0),
                AggregateData::ProvableCountAndSum(_, s) => Ok(s),
            },
            _ => Ok(0),
        }
    }

    /// Returns the sum of the root node's child on the given side, if any. If
    /// there is no child, returns 0.
    #[inline]
    pub fn child_aggregate_count_data_as_u64(&self, left: bool) -> Result<u64, Error> {
        match self.link(left) {
            Some(link) => match link.aggregate_data() {
                AggregateData::NoAggregateData => Ok(0),
                AggregateData::Sum(_) => Ok(0),
                AggregateData::BigSum(_) => Ok(0),
                AggregateData::Count(c) => Ok(c),
                AggregateData::CountAndSum(c, _) => Ok(c),
                AggregateData::ProvableCount(c) => Ok(c),
                AggregateData::ProvableCountAndSum(c, _) => Ok(c),
            },
            _ => Ok(0),
        }
    }

    /// Returns the sum of the root node's child on the given side, if any. If
    /// there is no child, returns 0.
    #[inline]
    pub fn child_aggregate_sum_data_as_i128(&self, left: bool) -> i128 {
        match self.link(left) {
            Some(link) => match link.aggregate_data() {
                AggregateData::NoAggregateData => 0,
                AggregateData::Sum(s) => s as i128,
                AggregateData::BigSum(s) => s,
                AggregateData::Count(_) => 0,
                AggregateData::CountAndSum(_, s) => s as i128,
                AggregateData::ProvableCount(_) => 0,
                AggregateData::ProvableCountAndSum(_, s) => s as i128,
            },
            _ => 0,
        }
    }

    /// Computes and returns the hash of the root node.
    #[inline]
    pub fn hash(&self) -> CostContext<CryptoHash> {
        // TODO: should we compute node hash as we already have a node hash?
        node_hash(
            self.inner.kv.hash(),
            self.child_hash(true),
            self.child_hash(false),
        )
    }

    /// Computes and returns the hash of the root node, including aggregate data
    /// for ProvableCountTree and ProvableCountSumTree.
    #[inline]
    pub fn hash_for_link(&self, tree_type: TreeType) -> CostContext<CryptoHash> {
        match tree_type {
            TreeType::ProvableCountTree => {
                // For ProvableCountTree, include the aggregate count in the hash
                let aggregate_data = self
                    .aggregate_data()
                    .unwrap_or(AggregateData::NoAggregateData);
                if let AggregateData::ProvableCount(count) = aggregate_data {
                    node_hash_with_count(
                        self.inner.kv.hash(),
                        self.child_hash(true),
                        self.child_hash(false),
                        count,
                    )
                } else {
                    // Fallback to regular hash if aggregate data is unexpected
                    self.hash()
                }
            }
            TreeType::ProvableCountSumTree => {
                // For ProvableCountSumTree, include only the count in the hash (not the sum)
                let aggregate_data = self
                    .aggregate_data()
                    .unwrap_or(AggregateData::NoAggregateData);
                if let AggregateData::ProvableCountAndSum(count, _) = aggregate_data {
                    node_hash_with_count(
                        self.inner.kv.hash(),
                        self.child_hash(true),
                        self.child_hash(false),
                        count,
                    )
                } else {
                    // Fallback to regular hash if aggregate data is unexpected
                    self.hash()
                }
            }
            _ => self.hash(),
        }
    }

    /// Computes and returns the hash of the root node.
    #[inline]
    pub fn aggregate_data(&self) -> Result<AggregateData, Error> {
        match self.inner.kv.feature_type {
            TreeFeatureType::BasicMerkNode => Ok(AggregateData::NoAggregateData),
            TreeFeatureType::SummedMerkNode(value) => {
                let left = self.child_aggregate_sum_data_as_i64(true)?;
                let right = self.child_aggregate_sum_data_as_i64(false)?;
                value
                    .checked_add(left)
                    .and_then(|a| a.checked_add(right))
                    .ok_or(Overflow("sum is overflowing"))
                    .map(AggregateData::Sum)
            }
            TreeFeatureType::BigSummedMerkNode(value) => value
                .checked_add(self.child_aggregate_sum_data_as_i128(true))
                .and_then(|a| a.checked_add(self.child_aggregate_sum_data_as_i128(false)))
                .ok_or(Overflow("big sum is overflowing"))
                .map(AggregateData::BigSum),
            TreeFeatureType::CountedMerkNode(value) => {
                let left = self.child_aggregate_count_data_as_u64(true)?;
                let right = self.child_aggregate_count_data_as_u64(false)?;
                value
                    .checked_add(left)
                    .and_then(|a| a.checked_add(right))
                    .ok_or(Overflow("count is overflowing"))
                    .map(AggregateData::Count)
            }
            TreeFeatureType::CountedSummedMerkNode(count_value, sum_value) => {
                let left_count = self.child_aggregate_count_data_as_u64(true)?;
                let right_count = self.child_aggregate_count_data_as_u64(false)?;
                let left_sum = self.child_aggregate_sum_data_as_i64(true)?;
                let right_sum = self.child_aggregate_sum_data_as_i64(false)?;
                let aggregated_count_value = count_value
                    .checked_add(left_count)
                    .and_then(|a| a.checked_add(right_count))
                    .ok_or(Overflow("count is overflowing"))?;

                let aggregated_sum_value = sum_value
                    .checked_add(left_sum)
                    .and_then(|a| a.checked_add(right_sum))
                    .ok_or(Overflow("count is overflowing"))?;

                Ok(AggregateData::CountAndSum(
                    aggregated_count_value,
                    aggregated_sum_value,
                ))
            }
            TreeFeatureType::ProvableCountedMerkNode(value) => {
                let left = self.child_aggregate_count_data_as_u64(true)?;
                let right = self.child_aggregate_count_data_as_u64(false)?;
                value
                    .checked_add(left)
                    .and_then(|a| a.checked_add(right))
                    .ok_or(Overflow("count is overflowing"))
                    .map(AggregateData::ProvableCount)
            }
            TreeFeatureType::ProvableCountedSummedMerkNode(count_value, sum_value) => {
                // Aggregate both count and sum from children
                let left_count = self.child_aggregate_count_data_as_u64(true)?;
                let right_count = self.child_aggregate_count_data_as_u64(false)?;
                let left_sum = self.child_aggregate_sum_data_as_i64(true)?;
                let right_sum = self.child_aggregate_sum_data_as_i64(false)?;

                let aggregated_count_value = count_value
                    .checked_add(left_count)
                    .and_then(|a| a.checked_add(right_count))
                    .ok_or(Overflow("count is overflowing"))?;

                let aggregated_sum_value = sum_value
                    .checked_add(left_sum)
                    .and_then(|a| a.checked_add(right_sum))
                    .ok_or(Overflow("sum is overflowing"))?;

                Ok(AggregateData::ProvableCountAndSum(
                    aggregated_count_value,
                    aggregated_sum_value,
                ))
            }
        }
    }

    /// Returns the number of pending writes for the child on the given side, if
    /// any. If there is no child, returns 0.
    #[inline]
    pub const fn child_pending_writes(&self, left: bool) -> usize {
        match self.link(left) {
            Some(Link::Modified { pending_writes, .. }) => *pending_writes,
            _ => 0,
        }
    }

    /// Returns the height of the child on the given side, if any. If there is
    /// no child, returns 0.
    #[inline]
    pub const fn child_height(&self, left: bool) -> u8 {
        match self.link(left) {
            Some(child) => child.height(),
            _ => 0,
        }
    }

    #[inline]
    /// Return the child heights of self
    pub const fn child_heights(&self) -> (u8, u8) {
        (self.child_height(true), self.child_height(false))
    }

    /// Returns the height of the tree (the number of levels). For example, a
    /// single node has height 1, a node with a single descendant has height 2,
    /// etc.
    #[inline]
    pub fn height(&self) -> u8 {
        1 + max(self.child_height(true), self.child_height(false))
    }

    /// Returns the balance factor of the root node. This is the difference
    /// between the height of the right child (if any) and the height of the
    /// left child (if any). For example, a balance factor of 2 means the right
    /// subtree is 2 levels taller than the left subtree.
    #[inline]
    pub const fn balance_factor(&self) -> i8 {
        let left_height = self.child_height(true) as i8;
        let right_height = self.child_height(false) as i8;
        right_height - left_height
    }

    /// Attaches the child (if any) to the root node on the given side. Creates
    /// a `Link` of variant `Link::Modified` which contains the child.
    ///
    /// Panics if there is already a child on the given side.
    #[inline]
    pub fn attach(mut self, left: bool, maybe_child: Option<Self>) -> Self {
        debug_assert_ne!(
            Some(self.key()),
            maybe_child.as_ref().map(|c| c.key()),
            "Tried to attach tree with same key"
        );

        // let parent = std::str::from_utf8(self.key());
        // if maybe_child.is_some(){
        //     let child = std::str::from_utf8(maybe_child.as_ref().unwrap().key());
        //     println!("attaching {} to {}", child.unwrap(), parent.unwrap());
        // } else {
        //     println!("attaching nothing to {}", parent.unwrap());
        // }

        let slot = self.slot_mut(left);

        if slot.is_some() {
            panic!(
                "Tried to attach to {} tree slot, but it is already Some",
                side_to_str(left)
            );
        }
        *slot = Link::maybe_from_modified_tree(maybe_child);

        self
    }

    /// Detaches the child on the given side (if any) from the root node, and
    /// returns `(root_node, maybe_child)`.
    ///
    /// One will usually want to reattach (see `attach`) a child on the same
    /// side after applying some operation to the detached child.
    #[inline]
    pub fn detach(mut self, left: bool) -> (Self, Option<Self>) {
        let maybe_child = match self.slot_mut(left).take() {
            None => None,
            Some(Link::Reference { .. }) => None,
            Some(Link::Modified { tree, .. }) => Some(tree),
            Some(Link::Uncommitted { tree, .. }) => Some(tree),
            Some(Link::Loaded { tree, .. }) => Some(tree),
        };
        // println!("detaching {}",
        // std::str::from_utf8(maybe_child.as_ref().unwrap().key()).unwrap());

        (self, maybe_child)
    }

    /// Detaches the child on the given side from the root node, and
    /// returns `(root_node, child)`.
    ///
    /// Panics if there is no child on the given side.
    ///
    /// One will usually want to reattach (see `attach`) a child on the same
    /// side after applying some operation to the detached child.
    #[inline]
    pub fn detach_expect(self, left: bool) -> (Self, Self) {
        let (parent, maybe_child) = self.detach(left);

        if let Some(child) = maybe_child {
            (parent, child)
        } else {
            panic!(
                "Expected tree to have {} child, but got None",
                side_to_str(left)
            );
        }
    }

    /// Detaches the child on the given side and passes it into `f`, which must
    /// return a new child (either the same child, a new child to take its
    /// place, or `None` to explicitly keep the slot empty).
    ///
    /// This is the same as `detach`, but with the function interface to enforce
    /// at compile-time that an explicit final child value is returned. This is
    /// less error prone that detaching with `detach` and reattaching with
    /// `attach`.
    #[inline]
    pub fn walk<F>(self, left: bool, f: F) -> Self
    where
        F: FnOnce(Option<Self>) -> Option<Self>,
    {
        let (tree, maybe_child) = self.detach(left);
        tree.attach(left, f(maybe_child))
    }

    /// Like `walk`, but panics if there is no child on the given side.
    #[inline]
    pub fn walk_expect<F>(self, left: bool, f: F) -> Self
    where
        F: FnOnce(Self) -> Option<Self>,
    {
        let (tree, child) = self.detach_expect(left);
        tree.attach(left, f(child))
    }

    /// Returns a mutable reference to the child slot for the given side.
    #[inline]
    pub(crate) fn slot_mut(&mut self, left: bool) -> &mut Option<Link> {
        if left {
            &mut self.inner.left
        } else {
            &mut self.inner.right
        }
    }

    /// Replaces the root node's value with the given value and returns the
    /// modified `Tree`.
    #[inline]
    pub fn put_value(
        mut self,
        value: Vec<u8>,
        feature_type: TreeFeatureType,
        old_specialized_cost: &impl Fn(&Vec<u8>, &Vec<u8>) -> Result<u32, Error>,
        get_temp_new_value_with_old_flags: &impl Fn(
            &Vec<u8>,
            &Vec<u8>,
        ) -> Result<Option<Vec<u8>>, Error>,
        update_tree_value_based_on_costs: &mut impl FnMut(
            &StorageCost,
            &Vec<u8>,
            &mut Vec<u8>,
        ) -> Result<
            (bool, Option<ValueDefinedCostType>),
            Error,
        >,
        section_removal_bytes: &mut impl FnMut(
            &Vec<u8>,
            u32,
            u32,
        ) -> Result<
            (StorageRemovedBytes, StorageRemovedBytes),
            Error,
        >,
    ) -> CostResult<Self, Error> {
        let mut cost = OperationCost::default();

        self.inner.kv = self.inner.kv.put_value_no_update_of_hashes(value);
        self.inner.kv.feature_type = feature_type;

        if self.old_value.is_some() {
            // we are replacing a value
            // in this case there is a possibility that the client would want to update the
            // element flags based on the change of values
            cost_return_on_error_no_add!(
                cost,
                self.just_in_time_tree_node_value_update(
                    old_specialized_cost,
                    get_temp_new_value_with_old_flags,
                    update_tree_value_based_on_costs,
                    section_removal_bytes
                )
            );
        }

        self.inner.kv = self.inner.kv.update_hashes().unwrap_add_cost(&mut cost);
        Ok(self).wrap_with_cost(cost)
    }

    /// Replaces the root node's value with the given value and returns the
    /// modified `Tree`.
    #[inline]
    pub fn put_value_with_fixed_cost(
        mut self,
        value: Vec<u8>,
        value_fixed_cost: u32,
        feature_type: TreeFeatureType,
        old_specialized_cost: &impl Fn(&Vec<u8>, &Vec<u8>) -> Result<u32, Error>,
        get_temp_new_value_with_old_flags: &impl Fn(
            &Vec<u8>,
            &Vec<u8>,
        ) -> Result<Option<Vec<u8>>, Error>,
        update_tree_value_based_on_costs: &mut impl FnMut(
            &StorageCost,
            &Vec<u8>,
            &mut Vec<u8>,
        ) -> Result<
            (bool, Option<ValueDefinedCostType>),
            Error,
        >,
        section_removal_bytes: &mut impl FnMut(
            &Vec<u8>,
            u32,
            u32,
        ) -> Result<
            (StorageRemovedBytes, StorageRemovedBytes),
            Error,
        >,
    ) -> CostResult<Self, Error> {
        let mut cost = OperationCost::default();
        self.inner.kv = self.inner.kv.put_value_with_fixed_cost_no_update_of_hashes(
            value,
            SpecializedValueDefinedCost(value_fixed_cost),
        );
        self.inner.kv.feature_type = feature_type;

        if self.old_value.is_some() {
            // we are replacing a value
            // in this case there is a possibility that the client would want to update the
            // element flags based on the change of values
            cost_return_on_error_no_add!(
                cost,
                self.just_in_time_tree_node_value_update(
                    old_specialized_cost,
                    get_temp_new_value_with_old_flags,
                    update_tree_value_based_on_costs,
                    section_removal_bytes
                )
            );
        }

        self.inner.kv = self.inner.kv.update_hashes().unwrap_add_cost(&mut cost);
        Ok(self).wrap_with_cost(cost)
    }

    /// Replaces the root node's value with the given value and value hash
    /// and returns the modified `Tree`.
    #[inline]
    pub fn put_value_and_reference_value_hash(
        mut self,
        value: Vec<u8>,
        value_hash: CryptoHash,
        feature_type: TreeFeatureType,
        old_specialized_cost: &impl Fn(&Vec<u8>, &Vec<u8>) -> Result<u32, Error>,
        get_temp_new_value_with_old_flags: &impl Fn(
            &Vec<u8>,
            &Vec<u8>,
        ) -> Result<Option<Vec<u8>>, Error>,
        update_tree_value_based_on_costs: &mut impl FnMut(
            &StorageCost,
            &Vec<u8>,
            &mut Vec<u8>,
        ) -> Result<
            (bool, Option<ValueDefinedCostType>),
            Error,
        >,
        section_removal_bytes: &mut impl FnMut(
            &Vec<u8>,
            u32,
            u32,
        ) -> Result<
            (StorageRemovedBytes, StorageRemovedBytes),
            Error,
        >,
    ) -> CostResult<Self, Error> {
        let mut cost = OperationCost::default();

        self.inner.kv = self.inner.kv.put_value_no_update_of_hashes(value);
        self.inner.kv.feature_type = feature_type;

        if self.old_value.is_some() {
            // we are replacing a value
            // in this case there is a possibility that the client would want to update the
            // element flags based on the change of values
            cost_return_on_error_no_add!(
                cost,
                self.just_in_time_tree_node_value_update(
                    old_specialized_cost,
                    get_temp_new_value_with_old_flags,
                    update_tree_value_based_on_costs,
                    section_removal_bytes
                )
            );
        }

        self.inner.kv = self
            .inner
            .kv
            .update_hashes_using_reference_value_hash(value_hash)
            .unwrap_add_cost(&mut cost);
        Ok(self).wrap_with_cost(cost)
    }

    /// Replaces the root node's value with the given value and value hash
    /// and returns the modified `Tree`.
    #[inline]
    pub fn put_value_with_reference_value_hash_and_value_cost(
        mut self,
        value: Vec<u8>,
        value_hash: CryptoHash,
        value_cost: u32,
        feature_type: TreeFeatureType,
        old_specialized_cost: &impl Fn(&Vec<u8>, &Vec<u8>) -> Result<u32, Error>,
        get_temp_new_value_with_old_flags: &impl Fn(
            &Vec<u8>,
            &Vec<u8>,
        ) -> Result<Option<Vec<u8>>, Error>,
        update_tree_value_based_on_costs: &mut impl FnMut(
            &StorageCost,
            &Vec<u8>,
            &mut Vec<u8>,
        ) -> Result<
            (bool, Option<ValueDefinedCostType>),
            Error,
        >,
        section_removal_bytes: &mut impl FnMut(
            &Vec<u8>,
            u32,
            u32,
        ) -> Result<
            (StorageRemovedBytes, StorageRemovedBytes),
            Error,
        >,
    ) -> CostResult<Self, Error> {
        let mut cost = OperationCost::default();

        self.inner.kv = self.inner.kv.put_value_with_fixed_cost_no_update_of_hashes(
            value,
            LayeredValueDefinedCost(value_cost),
        );
        self.inner.kv.feature_type = feature_type;

        if self.old_value.is_some() {
            // we are replacing a value
            // in this case there is a possibility that the client would want to update the
            // element flags based on the change of values
            cost_return_on_error_no_add!(
                cost,
                self.just_in_time_tree_node_value_update(
                    old_specialized_cost,
                    get_temp_new_value_with_old_flags,
                    update_tree_value_based_on_costs,
                    section_removal_bytes
                )
            );
        }

        self.inner.kv = self
            .inner
            .kv
            .update_hashes_using_reference_value_hash(value_hash)
            .unwrap_add_cost(&mut cost);
        Ok(self).wrap_with_cost(cost)
    }

    // TODO: add compute_hashes method

    /// Called to finalize modifications to a tree, recompute its hashes, and
    /// write the updated nodes to a backing store.
    ///
    /// Traverses through the tree, computing hashes for all modified links and
    /// replacing them with `Link::Loaded` variants, writes out all changes to
    /// the given `Commit` object's `write` method, and calls the its `prune`
    /// method to test whether or not to keep or prune nodes from memory.
    pub fn commit<C: Commit>(
        &mut self,
        c: &mut C,
        old_specialized_cost: &impl Fn(&Vec<u8>, &Vec<u8>) -> Result<u32, Error>,
    ) -> CostResult<(), Error> {
        // TODO: make this method less ugly
        // TODO: call write in-order for better performance in writing batch to db?

        // println!("about to commit {}", std::str::from_utf8(self.key()).unwrap());
        let mut cost = OperationCost::default();

        if let Some(Link::Modified { .. }) = self.inner.left {
            // println!("left is modified");
            if let Some(Link::Modified {
                mut tree,
                child_heights,
                ..
            }) = self.inner.left.take()
            {
                // println!("key is {}", std::str::from_utf8(tree.key()).unwrap());
                cost_return_on_error!(&mut cost, tree.commit(c, old_specialized_cost,));
                let aggregate_data = cost_return_on_error_default!(tree.aggregate_data());

                // Use special hash for ProvableCountTree and ProvableCountSumTree
                let hash = match &aggregate_data {
                    AggregateData::ProvableCount(count) => node_hash_with_count(
                        tree.inner.kv.hash(),
                        tree.child_hash(true),
                        tree.child_hash(false),
                        *count,
                    )
                    .unwrap_add_cost(&mut cost),
                    AggregateData::ProvableCountAndSum(count, _) => node_hash_with_count(
                        tree.inner.kv.hash(),
                        tree.child_hash(true),
                        tree.child_hash(false),
                        *count,
                    )
                    .unwrap_add_cost(&mut cost),
                    _ => tree.hash().unwrap_add_cost(&mut cost),
                };
                self.inner.left = Some(Link::Loaded {
                    hash,
                    tree,
                    child_heights,
                    aggregate_data,
                });
            } else {
                unreachable!()
            }
        }

        if let Some(Link::Modified { .. }) = self.inner.right {
            // println!("right is modified");
            if let Some(Link::Modified {
                mut tree,
                child_heights,
                ..
            }) = self.inner.right.take()
            {
                // println!("key is {}", std::str::from_utf8(tree.key()).unwrap());
                cost_return_on_error!(&mut cost, tree.commit(c, old_specialized_cost,));
                let aggregate_data = cost_return_on_error_default!(tree.aggregate_data());
                // Use special hash for ProvableCountTree and ProvableCountSumTree
                let hash = match &aggregate_data {
                    AggregateData::ProvableCount(count) => node_hash_with_count(
                        tree.inner.kv.hash(),
                        tree.child_hash(true),
                        tree.child_hash(false),
                        *count,
                    )
                    .unwrap_add_cost(&mut cost),
                    AggregateData::ProvableCountAndSum(count, _) => node_hash_with_count(
                        tree.inner.kv.hash(),
                        tree.child_hash(true),
                        tree.child_hash(false),
                        *count,
                    )
                    .unwrap_add_cost(&mut cost),
                    _ => tree.hash().unwrap_add_cost(&mut cost),
                };
                self.inner.right = Some(Link::Loaded {
                    hash,
                    tree,
                    child_heights,
                    aggregate_data,
                });
            } else {
                unreachable!()
            }
        }

        cost_return_on_error_no_add!(cost, c.write(self, old_specialized_cost,));

        // println!("done committing {}", std::str::from_utf8(self.key()).unwrap());

        let (prune_left, prune_right) = c.prune(self);
        if prune_left {
            self.inner.left = self.inner.left.take().map(|link| link.into_reference());
        }
        if prune_right {
            self.inner.right = self.inner.right.take().map(|link| link.into_reference());
        }

        Ok(()).wrap_with_cost(cost)
    }

    /// Fetches the child on the given side using the given data source, and
    /// places it in the child slot (upgrading the link from `Link::Reference`
    /// to `Link::Loaded`).
    pub fn load<S: Fetch, V>(
        &mut self,
        left: bool,
        source: &S,
        value_defined_cost_fn: Option<&V>,
        grove_version: &GroveVersion,
    ) -> CostResult<(), Error>
    where
        V: Fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>,
    {
        // TODO: return Err instead of panic?
        let link = self.link(left).expect("Expected link");
        let (child_heights, hash, aggregate_data) = match link {
            Link::Reference {
                child_heights,
                hash,
                aggregate_data,
                ..
            } => (child_heights, hash, aggregate_data),
            _ => panic!("Expected Some(Link::Reference)"),
        };

        let mut cost = OperationCost::default();
        let tree = cost_return_on_error!(
            &mut cost,
            source.fetch(link, value_defined_cost_fn, grove_version)
        );
        debug_assert_eq!(tree.key(), link.key());
        *self.slot_mut(left) = Some(Link::Loaded {
            tree,
            hash: *hash,
            child_heights: *child_heights,
            aggregate_data: *aggregate_data,
        });
        Ok(()).wrap_with_cost(cost)
    }
}

#[cfg(feature = "minimal")]
/// Convert side (left or right) to string
pub const fn side_to_str(left: bool) -> &'static str {
    if left {
        "left"
    } else {
        "right"
    }
}

#[cfg(feature = "minimal")]
#[cfg(test)]
mod test_provable_count;
#[cfg(test)]
mod test_provable_count_edge_cases;

#[cfg(feature = "minimal")]
#[cfg(test)]
mod test {

    use super::{commit::NoopCommit, hash::NULL_HASH, AggregateData, TreeNode};
    use crate::tree::{
        tree_feature_type::TreeFeatureType::SummedMerkNode, TreeFeatureType::BasicMerkNode,
    };

    #[test]
    fn build_tree() {
        let tree = TreeNode::new(vec![1], vec![101], None, BasicMerkNode).unwrap();
        assert_eq!(tree.key(), &[1]);
        assert_eq!(tree.value_as_slice(), &[101]);
        assert!(tree.child(true).is_none());
        assert!(tree.child(false).is_none());

        let tree = tree.attach(true, None);
        assert!(tree.child(true).is_none());
        assert!(tree.child(false).is_none());

        let tree = tree.attach(
            true,
            Some(TreeNode::new(vec![2], vec![102], None, BasicMerkNode).unwrap()),
        );
        assert_eq!(tree.key(), &[1]);
        assert_eq!(tree.child(true).unwrap().key(), &[2]);
        assert!(tree.child(false).is_none());

        let tree = TreeNode::new(vec![3], vec![103], None, BasicMerkNode)
            .unwrap()
            .attach(false, Some(tree));
        assert_eq!(tree.key(), &[3]);
        assert_eq!(tree.child(false).unwrap().key(), &[1]);
        assert!(tree.child(true).is_none());
    }

    #[should_panic]
    #[test]
    fn attach_existing() {
        TreeNode::new(vec![0], vec![1], None, BasicMerkNode)
            .unwrap()
            .attach(
                true,
                Some(TreeNode::new(vec![2], vec![3], None, BasicMerkNode).unwrap()),
            )
            .attach(
                true,
                Some(TreeNode::new(vec![4], vec![5], None, BasicMerkNode).unwrap()),
            );
    }

    #[test]
    fn modify() {
        let tree = TreeNode::new(vec![0], vec![1], None, BasicMerkNode)
            .unwrap()
            .attach(
                true,
                Some(TreeNode::new(vec![2], vec![3], None, BasicMerkNode).unwrap()),
            )
            .attach(
                false,
                Some(TreeNode::new(vec![4], vec![5], None, BasicMerkNode).unwrap()),
            );

        let tree = tree.walk(true, |left_opt| {
            assert_eq!(left_opt.as_ref().unwrap().key(), &[2]);
            None
        });
        assert!(tree.child(true).is_none());
        assert!(tree.child(false).is_some());

        let tree = tree.walk(true, |left_opt| {
            assert!(left_opt.is_none());
            Some(TreeNode::new(vec![2], vec![3], None, BasicMerkNode).unwrap())
        });
        assert_eq!(tree.link(true).unwrap().key(), &[2]);

        let tree = tree.walk_expect(false, |right| {
            assert_eq!(right.key(), &[4]);
            None
        });
        assert!(tree.child(true).is_some());
        assert!(tree.child(false).is_none());
    }

    #[test]
    fn child_and_link() {
        let mut tree = TreeNode::new(vec![0], vec![1], None, BasicMerkNode)
            .unwrap()
            .attach(
                true,
                Some(TreeNode::new(vec![2], vec![3], None, BasicMerkNode).unwrap()),
            );
        assert!(tree.link(true).expect("expected link").is_modified());
        assert!(tree.child(true).is_some());
        assert!(tree.link(false).is_none());
        assert!(tree.child(false).is_none());

        tree.commit(&mut NoopCommit {}, &|_, _| Ok(0))
            .unwrap()
            .expect("commit failed");
        assert!(tree.link(true).expect("expected link").is_stored());
        assert!(tree.child(true).is_some());

        // tree.link(true).prune(true);
        // assert!(tree.link(true).expect("expected link").is_pruned());
        // assert!(tree.child(true).is_none());

        let tree = tree.walk(true, |_| None);
        assert!(tree.link(true).is_none());
        assert!(tree.child(true).is_none());
    }

    #[test]
    fn child_hash() {
        let mut tree = TreeNode::new(vec![0], vec![1], None, BasicMerkNode)
            .unwrap()
            .attach(
                true,
                Some(TreeNode::new(vec![2], vec![3], None, BasicMerkNode).unwrap()),
            );
        tree.commit(&mut NoopCommit {}, &|_, _| Ok(0))
            .unwrap()
            .expect("commit failed");
        assert_eq!(
            tree.child_hash(true),
            &[
                132, 211, 39, 192, 19, 164, 57, 106, 128, 9, 35, 145, 86, 12, 57, 192, 239, 69,
                113, 148, 33, 220, 206, 207, 237, 199, 214, 241, 97, 144, 224, 185
            ]
        );
        assert_eq!(tree.child_hash(false), &NULL_HASH);
    }

    #[test]
    fn hash() {
        let tree = TreeNode::new(vec![0], vec![1], None, BasicMerkNode).unwrap();
        assert_eq!(
            tree.hash().unwrap(),
            [
                10, 108, 153, 163, 54, 173, 62, 155, 228, 204, 102, 172, 158, 203, 197, 126, 230,
                234, 97, 110, 227, 208, 64, 21, 65, 8, 82, 2, 241, 122, 66, 207
            ]
        );
    }

    #[test]
    fn child_pending_writes() {
        let tree = TreeNode::new(vec![0], vec![1], None, BasicMerkNode).unwrap();
        assert_eq!(tree.child_pending_writes(true), 0);
        assert_eq!(tree.child_pending_writes(false), 0);

        let tree = tree.attach(
            true,
            Some(TreeNode::new(vec![2], vec![3], None, BasicMerkNode).unwrap()),
        );
        assert_eq!(tree.child_pending_writes(true), 1);
        assert_eq!(tree.child_pending_writes(false), 0);
    }

    #[test]
    fn height_and_balance() {
        let tree = TreeNode::new(vec![0], vec![1], None, BasicMerkNode).unwrap();
        assert_eq!(tree.height(), 1);
        assert_eq!(tree.child_height(true), 0);
        assert_eq!(tree.child_height(false), 0);
        assert_eq!(tree.balance_factor(), 0);

        let tree = tree.attach(
            true,
            Some(TreeNode::new(vec![2], vec![3], None, BasicMerkNode).unwrap()),
        );
        assert_eq!(tree.height(), 2);
        assert_eq!(tree.child_height(true), 1);
        assert_eq!(tree.child_height(false), 0);
        assert_eq!(tree.balance_factor(), -1);

        let (tree, maybe_child) = tree.detach(true);
        let tree = tree.attach(false, maybe_child);
        assert_eq!(tree.height(), 2);
        assert_eq!(tree.child_height(true), 0);
        assert_eq!(tree.child_height(false), 1);
        assert_eq!(tree.balance_factor(), 1);
    }

    #[test]
    fn commit() {
        let mut tree = TreeNode::new(vec![0], vec![1], None, BasicMerkNode)
            .unwrap()
            .attach(
                false,
                Some(TreeNode::new(vec![2], vec![3], None, BasicMerkNode).unwrap()),
            );
        tree.commit(&mut NoopCommit {}, &|_, _| Ok(0))
            .unwrap()
            .expect("commit failed");

        assert!(tree.link(false).expect("expected link").is_stored());
    }

    #[test]
    fn sum_tree() {
        let mut tree = TreeNode::new(vec![0], vec![1], None, SummedMerkNode(3))
            .unwrap()
            .attach(
                false,
                Some(TreeNode::new(vec![2], vec![3], None, SummedMerkNode(5)).unwrap()),
            );
        tree.commit(&mut NoopCommit {}, &|_, _| Ok(0))
            .unwrap()
            .expect("commit failed");

        assert_eq!(
            AggregateData::Sum(8),
            tree.aggregate_data()
                .expect("expected to get sum from tree")
        );
    }
}
