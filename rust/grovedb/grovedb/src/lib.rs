//! GroveDB is a database that enables cryptographic proofs for complex queries.
//!
//! # Examples
//!
//! ## Open
//! Open an existing instance of GroveDB or create a new one at a given path.
//! ```
//! use grovedb::GroveDb;
//! use tempfile::TempDir;
//!
//! // Specify the path where you want to set up the GroveDB instance
//! let tmp_dir = TempDir::new().unwrap();
//! let path = tmp_dir.path();
//!
//! // Open a new GroveDB at the path
//! let db = GroveDb::open(&path).unwrap();
//! ```
//!
//! ## Basic Operations
//! Insert, Update, Delete and Prove elements.
//! ```
//! use grovedb::{Element, GroveDb};
//! use grovedb_version::version::GroveVersion;
//! use tempfile::TempDir;
//!
//! let grove_version = GroveVersion::latest();
//!
//! // Specify the path where you want to set up the GroveDB instance
//! let tmp_dir = TempDir::new().unwrap();
//! let path = tmp_dir.path();
//!
//! // Open a new GroveDB at the path
//! let db = GroveDb::open(&path).unwrap();
//!
//! let root_path: &[&[u8]] = &[];
//!
//! // Insert new tree to root
//! db.insert(
//!     root_path,
//!     b"tree1",
//!     Element::empty_tree(),
//!     None,
//!     None,
//!     grove_version,
//! )
//! .unwrap()
//! .expect("successful tree insert");
//!
//! // Insert key-value 1 into tree1
//! // key - hello, value - world
//! db.insert(
//!     &[b"tree1"],
//!     b"hello",
//!     Element::new_item(b"world".to_vec()),
//!     None,
//!     None,
//!     grove_version,
//! )
//! .unwrap()
//! .expect("successful key1 insert");
//!
//! // Insert key-value 2 into tree1
//! // key - grovedb, value = rocks
//! db.insert(
//!     &[b"tree1"],
//!     b"grovedb",
//!     Element::new_item(b"rocks".to_vec()),
//!     None,
//!     None,
//!     grove_version,
//! )
//! .unwrap()
//! .expect("successful key2 insert");
//!
//! // Retrieve inserted elements
//! let elem = db
//!     .get(&[b"tree1"], b"hello", None, grove_version)
//!     .unwrap()
//!     .expect("successful get");
//! assert_eq!(elem, Element::new_item(b"world".to_vec()));
//!
//! let elem = db
//!     .get(&[b"tree1"], b"grovedb", None, grove_version)
//!     .unwrap()
//!     .expect("successful get");
//! assert_eq!(elem, Element::new_item(b"rocks".to_vec()));
//!
//! // Update inserted element
//! // for non-tree elements, insertion to an already existing key updates it
//! db.insert(
//!     &[b"tree1"],
//!     b"hello",
//!     Element::new_item(b"WORLD".to_vec()),
//!     None,
//!     None,
//!     grove_version,
//! )
//! .unwrap()
//! .expect("successful update");
//!
//! // Retrieve updated element
//! let elem = db
//!     .get(&[b"tree1"], b"hello", None, grove_version)
//!     .unwrap()
//!     .expect("successful get");
//! assert_eq!(elem, Element::new_item(b"WORLD".to_vec()));
//!
//! // Deletion
//! db.delete(&[b"tree1"], b"hello", None, None, grove_version)
//!     .unwrap()
//!     .expect("successful delete");
//! let elem_result = db.get(&[b"tree1"], b"hello", None, grove_version).unwrap();
//! assert_eq!(elem_result.is_err(), true);
//!
//! // State Root
//! // Get the GroveDB root hash
//! let root_hash = db.root_hash(None, grove_version).unwrap().unwrap();
//! assert_eq!(
//!     hex::encode(root_hash),
//!     "3884be3d197ac49981e54b21ea423351fc4ccdb770aaf7cf40f5e65dc3e2e1aa"
//! );
//! ```
//!
//! For more documentation see our
//! [Architectural Decision Records](https://github.com/dashpay/grovedb/tree/master/adr) or
//! [Tutorial](https://www.grovedb.org/tutorials.html)

#[cfg(feature = "minimal")]
pub mod batch;
#[cfg(feature = "minimal")]
mod checkpoints;
#[cfg(feature = "grovedbg")]
pub mod debugger;
#[cfg(any(feature = "minimal", feature = "verify"))]
pub mod element;
#[cfg(any(feature = "minimal", feature = "verify"))]
pub mod error;
#[cfg(feature = "estimated_costs")]
mod estimated_costs;
#[cfg(feature = "minimal")]
mod merk_cache;
#[cfg(any(feature = "minimal", feature = "verify"))]
pub mod operations;
#[cfg(any(feature = "minimal", feature = "verify"))]
mod query;
#[cfg(any(feature = "minimal", feature = "verify"))]
pub mod query_result_type;
#[cfg(feature = "minimal")]
pub mod reference_path;
#[cfg(feature = "minimal")]
pub mod replication;
#[cfg(all(test, feature = "minimal"))]
mod tests;
#[cfg(feature = "minimal")]
mod util;
#[cfg(feature = "minimal")]
mod visualize;

#[cfg(feature = "grovedbg")]
use std::sync::Arc;
#[cfg(feature = "minimal")]
use std::{collections::HashMap, option::Option::None, path::Path};

#[cfg(feature = "grovedbg")]
use debugger::start_visualizer;
#[cfg(any(feature = "minimal", feature = "verify"))]
pub use element::Element;
#[cfg(any(feature = "minimal", feature = "verify"))]
pub use element::ElementFlags;
use grovedb_costs::cost_return_on_error_into;
#[cfg(feature = "minimal")]
use grovedb_costs::{
    cost_return_on_error, cost_return_on_error_no_add, CostResult, CostsExt, OperationCost,
};
#[cfg(any(feature = "minimal", feature = "verify"))]
pub use grovedb_merk::calculate_max_tree_depth_from_count;
#[cfg(feature = "minimal")]
use grovedb_merk::element::{
    costs::ElementCostExtensions, decode::ElementDecodeExtensions,
    get::ElementFetchFromStorageExtensions, insert::ElementInsertToStorageExtensions,
    tree_type::ElementTreeTypeExtensions, ElementExt,
};
#[cfg(feature = "estimated_costs")]
pub use grovedb_merk::estimated_costs::{
    average_case_costs::{
        EstimatedLayerCount, EstimatedLayerInformation, EstimatedLayerSizes, EstimatedSumTrees,
    },
    worst_case_costs::WorstCaseLayerInformation,
};
#[cfg(any(feature = "minimal", feature = "verify"))]
pub use grovedb_merk::proofs::query::query_item::QueryItem;
#[cfg(any(feature = "minimal", feature = "verify"))]
pub use grovedb_merk::proofs::query::VerifyOptions;
#[cfg(any(feature = "minimal", feature = "verify"))]
pub use grovedb_merk::proofs::Query;
#[cfg(any(feature = "minimal", feature = "verify"))]
pub use grovedb_merk::proofs::{
    encoding::Decoder as MerkProofDecoder, Node as MerkProofNode, Op as MerkProofOp,
};
#[cfg(feature = "minimal")]
use grovedb_merk::tree::kv::ValueDefinedCostType;
#[cfg(feature = "minimal")]
pub use grovedb_merk::tree::AggregateData;
#[cfg(any(feature = "minimal", feature = "verify"))]
pub use grovedb_merk::tree::TreeFeatureType;
#[cfg(feature = "minimal")]
pub use grovedb_merk::tree_type::{MaybeTree, TreeType};
#[cfg(feature = "minimal")]
use grovedb_merk::{
    self,
    tree::{combine_hash, value_hash},
    BatchEntry, CryptoHash, KVIterator, Merk,
};
#[cfg(feature = "minimal")]
use grovedb_path::SubtreePath;
#[cfg(feature = "minimal")]
use grovedb_storage::rocksdb_storage::PrefixedRocksDbImmediateStorageContext;
#[cfg(feature = "minimal")]
use grovedb_storage::rocksdb_storage::RocksDbStorage;
#[cfg(feature = "minimal")]
use grovedb_storage::{rocksdb_storage::PrefixedRocksDbTransactionContext, StorageBatch};
#[cfg(feature = "minimal")]
use grovedb_storage::{Storage, StorageContext};
#[cfg(feature = "minimal")]
use grovedb_version::version::GroveVersion;
#[cfg(feature = "minimal")]
use grovedb_visualize::DebugByteVectors;
#[cfg(any(feature = "minimal", feature = "verify"))]
pub use query::{
    GroveBranchQueryResult, GroveTrunkQueryResult, LeafInfo, PathBranchChunkQuery, PathQuery,
    PathTrunkChunkQuery, SizedQuery,
};
#[cfg(feature = "minimal")]
use reference_path::path_from_reference_path_type;
#[cfg(feature = "grovedbg")]
use tokio::net::ToSocketAddrs;
#[cfg(feature = "minimal")]
use util::{compat, TxRef};

#[cfg(any(feature = "minimal", feature = "verify"))]
pub use crate::error::Error;
#[cfg(feature = "minimal")]
use crate::operations::proof::util::hex_to_ascii;
#[cfg(feature = "minimal")]
use crate::Error::MerkError;

#[cfg(feature = "minimal")]
type Hash = [u8; 32];

/// GroveDb
pub struct GroveDb {
    #[cfg(feature = "minimal")]
    db: RocksDbStorage,
}

#[cfg(feature = "minimal")]
pub(crate) type SubtreePrefix = [u8; blake3::OUT_LEN];

/// Transaction
#[cfg(feature = "minimal")]
pub type Transaction<'db> = <RocksDbStorage as Storage<'db>>::Transaction;
/// TransactionArg
#[cfg(feature = "minimal")]
pub type TransactionArg<'db, 'a> = Option<&'a Transaction<'db>>;

/// Type alias for the return type of the `verify_merk_and_submerks` and
/// `verify_grovedb` functions. It represents a mapping of paths (as vectors of
/// vectors of bytes) to a tuple of three cryptographic hashes: the root hash,
/// the combined value hash, and the expected value hash.
#[cfg(feature = "minimal")]
type VerificationIssues = HashMap<Vec<Vec<u8>>, (CryptoHash, CryptoHash, CryptoHash)>;

/// Type alias for the return type of the `open_merk_for_replication` function.
/// It represents a tuple containing:
/// - A `Merk` instance with a prefixed RocksDB immediate storage context.
/// - An optional `root_key`, represented as a vector of bytes.
/// - A boolean indicating whether the Merk is a sum tree.
#[cfg(feature = "minimal")]
type OpenedMerkForReplication<'tx> = (
    Merk<PrefixedRocksDbImmediateStorageContext<'tx>>,
    Option<Vec<u8>>,
    TreeType,
);

#[cfg(feature = "minimal")]
impl GroveDb {
    /// Opens a given path
    pub fn open<P: AsRef<Path>>(path: P) -> Result<Self, Error> {
        let db = RocksDbStorage::default_rocksdb_with_path(path)?;
        Ok(GroveDb { db })
    }

    #[cfg(feature = "grovedbg")]
    // Start visualizer server for the GroveDB instance
    pub fn start_visualizer<A>(self: &Arc<Self>, addr: A)
    where
        A: ToSocketAddrs + Send + 'static,
    {
        let weak = Arc::downgrade(self);
        start_visualizer(weak, addr);
    }

    /// Uses raw iter to delete GroveDB key values pairs from rocksdb
    pub fn wipe(&self) -> Result<(), Error> {
        self.db.wipe()?;
        Ok(())
    }

    /// Opens the transactional Merk at the given path. Returns CostResult.
    fn open_transactional_merk_at_path<'db, 'b, B>(
        &'db self,
        path: SubtreePath<'b, B>,
        tx: &'db Transaction,
        batch: Option<&'db StorageBatch>,
        grove_version: &GroveVersion,
    ) -> CostResult<Merk<PrefixedRocksDbTransactionContext<'db>>, Error>
    where
        B: AsRef<[u8]> + 'b,
    {
        struct Compat;

        impl compat::OpenMerkErrorsCompat for Compat {
            fn parent_key_not_found<B: AsRef<[u8]>>(
                e: Error,
                parent_path: SubtreePath<B>,
                parent_key: &[u8],
            ) -> Error {
                Error::InvalidParentLayerPath(format!(
                    "could not get key {} for parent {:?} of subtree: {}",
                    hex::encode(parent_key),
                    DebugByteVectors(parent_path.to_vec()),
                    e
                ))
            }

            fn open_base_error() -> Error {
                Error::CorruptedData("cannot open a the root subtree".to_owned())
            }

            fn parent_must_be_tree() -> Error {
                Error::CorruptedData("cannot open a subtree with given root key".to_owned())
            }
        }

        compat::open_merk::<_, Compat>(&self.db, path, tx, batch, grove_version)
    }

    fn open_transactional_merk_by_prefix<'db>(
        &'db self,
        prefix: SubtreePrefix,
        root_key: Option<Vec<u8>>,
        tree_type: TreeType,
        tx: &'db Transaction,
        batch: Option<&'db StorageBatch>,
        grove_version: &GroveVersion,
    ) -> CostResult<Merk<PrefixedRocksDbTransactionContext<'db>>, Error> {
        let mut cost = OperationCost::default();
        let storage = self
            .db
            .get_transactional_storage_context_by_subtree_prefix(prefix, batch, tx)
            .unwrap_add_cost(&mut cost);
        if root_key.is_some() {
            Merk::open_layered_with_root_key(
                storage,
                root_key,
                tree_type,
                Some(&Element::value_defined_cost_for_serialized_value),
                grove_version,
            )
            .map_err(|_| {
                Error::CorruptedData(
                    "cannot open a subtree by prefix with given root key".to_owned(),
                )
            })
            .add_cost(cost)
        } else {
            Merk::open_base(
                storage,
                TreeType::NormalTree,
                Some(&Element::value_defined_cost_for_serialized_value),
                grove_version,
            )
            .map_err(|_| Error::CorruptedData("cannot open a root subtree by prefix".to_owned()))
            .add_cost(cost)
        }
    }

    /// Opens a Merk at given path for with direct write access. Intended for
    /// replication purposes.
    fn open_merk_for_replication<'tx, 'db: 'tx, 'b, B>(
        &'db self,
        path: SubtreePath<'b, B>,
        tx: &'tx Transaction<'db>,
        grove_version: &GroveVersion,
    ) -> Result<OpenedMerkForReplication<'tx>, Error>
    where
        B: AsRef<[u8]> + 'b,
    {
        let mut cost = OperationCost::default();

        let storage = self
            .db
            .get_immediate_storage_context(path.clone(), tx)
            .unwrap_add_cost(&mut cost);
        if let Some((parent_path, parent_key)) = path.derive_parent() {
            let parent_storage = self
                .db
                .get_immediate_storage_context(parent_path.clone(), tx)
                .unwrap_add_cost(&mut cost);
            let element = Element::get_from_storage(&parent_storage, parent_key, grove_version)
                .map_err(|e| {
                    Error::InvalidParentLayerPath(format!(
                        "could not get key {} for parent {:?} of subtree: {}",
                        hex::encode(parent_key),
                        DebugByteVectors(parent_path.to_vec()),
                        e
                    ))
                })
                .unwrap()?;
            if let Some((root_key, tree_type)) = element.root_key_and_tree_type_owned() {
                Ok((
                    Merk::open_layered_with_root_key(
                        storage,
                        root_key.clone(),
                        tree_type,
                        Some(&Element::value_defined_cost_for_serialized_value),
                        grove_version,
                    )
                    .map_err(|_| {
                        Error::CorruptedData("cannot open a subtree with given root key".to_owned())
                    })
                    .unwrap()?,
                    root_key,
                    tree_type,
                ))
            } else {
                Err(Error::CorruptedPath(
                    "cannot open a subtree as parent exists but is not a tree".to_string(),
                ))
            }
        } else {
            Ok((
                Merk::open_base(
                    storage,
                    TreeType::NormalTree,
                    None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                    grove_version,
                )
                .map_err(|_| Error::CorruptedData("cannot open a the root subtree".to_owned()))
                .unwrap()?,
                None,
                TreeType::NormalTree,
            ))
        }
    }

    /// Returns root key of GroveDb.
    /// Will be `None` if GroveDb is empty.
    pub fn root_key(
        &self,
        transaction: TransactionArg,
        grove_version: &GroveVersion,
    ) -> CostResult<Option<Vec<u8>>, Error> {
        let mut cost = OperationCost {
            ..Default::default()
        };

        let tx = TxRef::new(&self.db, transaction);

        let root_merk =
            cost_return_on_error!(&mut cost, self.open_root_merk(tx.as_ref(), grove_version));

        let root_key = root_merk.root_key();
        Ok(root_key).wrap_with_cost(cost)
    }

    /// Returns root hash of GroveDb.
    pub fn root_hash(
        &self,
        transaction: TransactionArg,
        grove_version: &GroveVersion,
    ) -> CostResult<Hash, Error> {
        let mut cost = OperationCost {
            ..Default::default()
        };

        let tx = TxRef::new(&self.db, transaction);

        let root_merk =
            cost_return_on_error!(&mut cost, self.open_root_merk(tx.as_ref(), grove_version));

        root_merk.root_hash().map(Ok).add_cost(cost)
    }

    fn open_root_merk<'tx, 'db>(
        &'db self,
        tx: &'tx Transaction<'db>,
        grove_version: &GroveVersion,
    ) -> CostResult<Merk<PrefixedRocksDbTransactionContext<'tx>>, Error> {
        self.db
            .get_transactional_storage_context(SubtreePath::empty(), None, tx)
            .flat_map(|storage_ctx| {
                grovedb_merk::Merk::open_base(
                    storage_ctx,
                    TreeType::NormalTree,
                    Some(Element::value_defined_cost_for_serialized_value),
                    grove_version,
                )
                .map(|merk_res| {
                    merk_res.map_err(|_| {
                        crate::Error::CorruptedData("cannot open a subtree".to_owned())
                    })
                })
            })
    }

    /// Method to propagate updated subtree key changes one level up inside a
    /// transaction
    fn propagate_changes_with_batch_transaction<'b, B: AsRef<[u8]>>(
        &self,
        storage_batch: &StorageBatch,
        mut merk_cache: HashMap<SubtreePath<'b, B>, Merk<PrefixedRocksDbTransactionContext>>,
        path: &SubtreePath<'b, B>,
        transaction: &Transaction,
        grove_version: &GroveVersion,
    ) -> CostResult<(), Error> {
        let mut cost = OperationCost::default();

        let mut child_tree = cost_return_on_error_no_add!(
            cost,
            merk_cache.remove(path).ok_or(Error::CorruptedCodeExecution(
                "Merk Cache should always contain the last path",
            ))
        );

        let mut current_path = path.clone();

        while let Some((parent_path, parent_key)) = current_path.derive_parent() {
            let mut parent_tree = cost_return_on_error!(
                &mut cost,
                self.open_batch_transactional_merk_at_path(
                    storage_batch,
                    parent_path.clone(),
                    transaction,
                    false,
                    grove_version,
                )
            );
            let (root_hash, root_key, aggregate_data) = cost_return_on_error!(
                &mut cost,
                child_tree
                    .root_hash_key_and_aggregate_data()
                    .map_err(Error::MerkError)
            );
            cost_return_on_error!(
                &mut cost,
                Self::update_tree_item_preserve_flag(
                    &mut parent_tree,
                    parent_key,
                    root_key,
                    root_hash,
                    aggregate_data,
                    grove_version,
                )
            );
            child_tree = parent_tree;
            current_path = parent_path;
        }
        Ok(()).wrap_with_cost(cost)
    }

    /// Method to propagate updated subtree key changes one level up inside a
    /// transaction
    fn propagate_changes_with_transaction<'b, B: AsRef<[u8]>>(
        &self,
        mut merk_cache: HashMap<SubtreePath<'b, B>, Merk<PrefixedRocksDbTransactionContext>>,
        path: SubtreePath<'b, B>,
        transaction: &Transaction,
        batch: &StorageBatch,
        grove_version: &GroveVersion,
    ) -> CostResult<(), Error> {
        let mut cost = OperationCost::default();

        let mut child_tree = cost_return_on_error_no_add!(
            cost,
            merk_cache
                .remove(&path)
                .ok_or(Error::CorruptedCodeExecution(
                    "Merk Cache should always contain the last path",
                ))
        );

        let mut current_path = path.clone();

        while let Some((parent_path, parent_key)) = current_path.derive_parent() {
            let mut parent_tree: Merk<PrefixedRocksDbTransactionContext> = cost_return_on_error!(
                &mut cost,
                self.open_transactional_merk_at_path(
                    parent_path.clone(),
                    transaction,
                    Some(batch),
                    grove_version
                )
            );
            let (root_hash, root_key, aggregate_data) = cost_return_on_error!(
                &mut cost,
                child_tree
                    .root_hash_key_and_aggregate_data()
                    .map_err(Error::MerkError)
            );
            cost_return_on_error!(
                &mut cost,
                Self::update_tree_item_preserve_flag(
                    &mut parent_tree,
                    parent_key,
                    root_key,
                    root_hash,
                    aggregate_data,
                    grove_version,
                )
            );
            child_tree = parent_tree;
            current_path = parent_path;
        }
        Ok(()).wrap_with_cost(cost)
    }

    /// Updates a tree item and preserves flags. Returns CostResult.
    pub(crate) fn update_tree_item_preserve_flag<'db, K: AsRef<[u8]>, S: StorageContext<'db>>(
        parent_tree: &mut Merk<S>,
        key: K,
        maybe_root_key: Option<Vec<u8>>,
        root_tree_hash: Hash,
        aggregate_data: AggregateData,
        grove_version: &GroveVersion,
    ) -> CostResult<(), Error> {
        let key_ref = key.as_ref();

        Self::get_element_from_subtree(parent_tree, key_ref, grove_version).flat_map_ok(|element| {
            if let Element::Tree(_, flag) = element {
                let tree = Element::new_tree_with_flags(maybe_root_key, flag);
                tree.insert_subtree(parent_tree, key_ref, root_tree_hash, None, grove_version)
                    .map_err(|e| e.into())
            } else if let Element::SumTree(.., flag) = element {
                let tree = Element::new_sum_tree_with_flags_and_sum_value(
                    maybe_root_key,
                    aggregate_data.as_sum_i64(),
                    flag,
                );
                tree.insert_subtree(
                    parent_tree,
                    key.as_ref(),
                    root_tree_hash,
                    None,
                    grove_version,
                )
                .map_err(|e| e.into())
            } else if let Element::BigSumTree(.., flag) = element {
                let tree = Element::new_big_sum_tree_with_flags_and_sum_value(
                    maybe_root_key,
                    aggregate_data.as_summed_i128(),
                    flag,
                );
                tree.insert_subtree(
                    parent_tree,
                    key.as_ref(),
                    root_tree_hash,
                    None,
                    grove_version,
                )
                .map_err(|e| e.into())
            } else if let Element::CountTree(.., flag) = element {
                let tree = Element::new_count_tree_with_flags_and_count_value(
                    maybe_root_key,
                    aggregate_data.as_count_u64(),
                    flag,
                );
                tree.insert_subtree(
                    parent_tree,
                    key.as_ref(),
                    root_tree_hash,
                    None,
                    grove_version,
                )
                .map_err(|e| e.into())
            } else if let Element::CountSumTree(.., flag) = element {
                let tree = Element::new_count_sum_tree_with_flags_and_sum_and_count_value(
                    maybe_root_key,
                    aggregate_data.as_count_u64(),
                    aggregate_data.as_sum_i64(),
                    flag,
                );
                tree.insert_subtree(
                    parent_tree,
                    key.as_ref(),
                    root_tree_hash,
                    None,
                    grove_version,
                )
                .map_err(|e| e.into())
            } else if let Element::ProvableCountTree(.., flag) = element {
                let tree = Element::new_provable_count_tree_with_flags_and_count_value(
                    maybe_root_key,
                    aggregate_data.as_count_u64(),
                    flag,
                );
                tree.insert_subtree(
                    parent_tree,
                    key.as_ref(),
                    root_tree_hash,
                    None,
                    grove_version,
                )
                .map_err(|e| e.into())
            } else if let Element::ProvableCountSumTree(.., flag) = element {
                let tree = Element::new_provable_count_sum_tree_with_flags_and_sum_and_count_value(
                    maybe_root_key,
                    aggregate_data.as_count_u64(),
                    aggregate_data.as_sum_i64(),
                    flag,
                );
                tree.insert_subtree(
                    parent_tree,
                    key.as_ref(),
                    root_tree_hash,
                    None,
                    grove_version,
                )
                .map_err(|e| e.into())
            } else {
                Err(Error::InvalidPath(
                    "can only propagate on tree items".to_owned(),
                ))
                .wrap_with_cost(Default::default())
            }
        })
    }

    /// Pushes to batch an operation which updates a tree item and preserves
    /// flags. Returns CostResult.
    pub(crate) fn update_tree_item_preserve_flag_into_batch_operations<
        'db,
        K: AsRef<[u8]>,
        S: StorageContext<'db>,
    >(
        parent_tree: &Merk<S>,
        key: K,
        maybe_root_key: Option<Vec<u8>>,
        root_tree_hash: Hash,
        aggregate_data: AggregateData,
        batch_operations: &mut Vec<BatchEntry<K>>,
        grove_version: &GroveVersion,
    ) -> CostResult<(), Error> {
        let mut cost = OperationCost::default();
        Self::get_element_from_subtree(parent_tree, key.as_ref(), grove_version).flat_map_ok(
            |element| {
                if let Element::Tree(_, flag) = element {
                    let tree = Element::new_tree_with_flags(maybe_root_key, flag);
                    let merk_feature_type = cost_return_on_error_into!(
                        &mut cost,
                        tree.get_feature_type(parent_tree.tree_type)
                            .wrap_with_cost(OperationCost::default())
                    );
                    tree.insert_subtree_into_batch_operations(
                        key,
                        root_tree_hash,
                        true,
                        batch_operations,
                        merk_feature_type,
                        grove_version,
                    )
                    .map_err(|e| e.into())
                } else if let Element::SumTree(.., flag) = element {
                    let tree = Element::new_sum_tree_with_flags_and_sum_value(
                        maybe_root_key,
                        aggregate_data.as_sum_i64(),
                        flag,
                    );
                    let merk_feature_type = cost_return_on_error_into!(
                        &mut cost,
                        tree.get_feature_type(parent_tree.tree_type)
                            .wrap_with_cost(OperationCost::default())
                    );
                    tree.insert_subtree_into_batch_operations(
                        key,
                        root_tree_hash,
                        true,
                        batch_operations,
                        merk_feature_type,
                        grove_version,
                    )
                    .map_err(|e| e.into())
                } else if let Element::BigSumTree(.., flag) = element {
                    let tree = Element::new_big_sum_tree_with_flags_and_sum_value(
                        maybe_root_key,
                        aggregate_data.as_summed_i128(),
                        flag,
                    );
                    let merk_feature_type = cost_return_on_error_into!(
                        &mut cost,
                        tree.get_feature_type(parent_tree.tree_type)
                            .wrap_with_cost(OperationCost::default())
                    );
                    tree.insert_subtree_into_batch_operations(
                        key,
                        root_tree_hash,
                        true,
                        batch_operations,
                        merk_feature_type,
                        grove_version,
                    )
                    .map_err(|e| e.into())
                } else if let Element::CountTree(.., flag) = element {
                    let tree = Element::new_count_tree_with_flags_and_count_value(
                        maybe_root_key,
                        aggregate_data.as_count_u64(),
                        flag,
                    );
                    let merk_feature_type = cost_return_on_error_into!(
                        &mut cost,
                        tree.get_feature_type(parent_tree.tree_type)
                            .wrap_with_cost(OperationCost::default())
                    );
                    tree.insert_subtree_into_batch_operations(
                        key,
                        root_tree_hash,
                        true,
                        batch_operations,
                        merk_feature_type,
                        grove_version,
                    )
                    .map_err(|e| e.into())
                } else if let Element::CountSumTree(.., flag) = element {
                    let tree = Element::new_count_sum_tree_with_flags_and_sum_and_count_value(
                        maybe_root_key,
                        aggregate_data.as_count_u64(),
                        aggregate_data.as_sum_i64(),
                        flag,
                    );
                    let merk_feature_type = cost_return_on_error_into!(
                        &mut cost,
                        tree.get_feature_type(parent_tree.tree_type)
                            .wrap_with_cost(OperationCost::default())
                    );
                    tree.insert_subtree_into_batch_operations(
                        key,
                        root_tree_hash,
                        true,
                        batch_operations,
                        merk_feature_type,
                        grove_version,
                    )
                    .map_err(|e| e.into())
                } else if let Element::ProvableCountTree(.., flag) = element {
                    let tree = Element::new_provable_count_tree_with_flags_and_count_value(
                        maybe_root_key,
                        aggregate_data.as_count_u64(),
                        flag,
                    );
                    let merk_feature_type = cost_return_on_error_into!(
                        &mut cost,
                        tree.get_feature_type(parent_tree.tree_type)
                            .wrap_with_cost(OperationCost::default())
                    );
                    tree.insert_subtree_into_batch_operations(
                        key,
                        root_tree_hash,
                        true,
                        batch_operations,
                        merk_feature_type,
                        grove_version,
                    )
                    .map_err(|e| e.into())
                } else if let Element::ProvableCountSumTree(.., flag) = element {
                    let tree =
                        Element::new_provable_count_sum_tree_with_flags_and_sum_and_count_value(
                            maybe_root_key,
                            aggregate_data.as_count_u64(),
                            aggregate_data.as_sum_i64(),
                            flag,
                        );
                    let merk_feature_type = cost_return_on_error_into!(
                        &mut cost,
                        tree.get_feature_type(parent_tree.tree_type)
                            .wrap_with_cost(OperationCost::default())
                    );
                    tree.insert_subtree_into_batch_operations(
                        key,
                        root_tree_hash,
                        true,
                        batch_operations,
                        merk_feature_type,
                        grove_version,
                    )
                    .map_err(|e| e.into())
                } else {
                    Err(Error::InvalidPath(
                        "can only propagate on tree items".to_owned(),
                    ))
                    .wrap_with_cost(Default::default())
                }
            },
        )
    }

    /// Get element from subtree. Return CostResult.
    fn get_element_from_subtree<'db, K: AsRef<[u8]>, S: StorageContext<'db>>(
        subtree: &Merk<S>,
        key: K,
        grove_version: &GroveVersion,
    ) -> CostResult<Element, Error> {
        subtree
            .get(
                key.as_ref(),
                true,
                Some(&Element::value_defined_cost_for_serialized_value),
                grove_version,
            )
            .map_err(|_| {
                Error::InvalidPath("can't find subtree in parent during propagation".to_owned())
            })
            .map_ok(|subtree_opt| {
                subtree_opt.ok_or_else(|| {
                    let key = hex::encode(key.as_ref());
                    Error::PathKeyNotFound(format!(
                        "can't find subtree with key {} in parent during propagation (subtree is \
                         {})",
                        key,
                        if subtree.root_key().is_some() {
                            "not empty"
                        } else {
                            "empty"
                        }
                    ))
                })
            })
            .flatten()
            .map_ok(|element_bytes| {
                Element::deserialize(&element_bytes, grove_version).map_err(|_| {
                    Error::CorruptedData(
                        "failed to deserialized parent during propagation".to_owned(),
                    )
                })
            })
            .flatten()
    }

    /// Flush memory table to disk.
    pub fn flush(&self) -> Result<(), Error> {
        Ok(self.db.flush()?)
    }

    /// Starts database transaction. Please note that you have to start
    /// underlying storage transaction manually.
    ///
    /// ## Examples:
    /// ```
    /// # use grovedb::{Element, Error, GroveDb};
    /// # use std::convert::TryFrom;
    /// # use tempfile::TempDir;
    /// # use grovedb_path::SubtreePath;
    /// # use grovedb_version::version::GroveVersion;
    /// #
    /// # fn main() -> Result<(), Box<dyn std::error::Error>> {
    /// use std::option::Option::None;
    /// ///
    ///
    /// const TEST_LEAF: &[u8] = b"test_leaf";
    ///
    /// let grove_version = GroveVersion::latest();
    ///
    /// let tmp_dir = TempDir::new().unwrap();
    /// let mut db = GroveDb::open(tmp_dir.path())?;
    /// db.insert(
    ///     SubtreePath::empty(),
    ///     TEST_LEAF,
    ///     Element::empty_tree(),
    ///     None,
    ///     None,
    ///     grove_version,
    /// )
    /// .unwrap()?;
    ///
    /// let tx = db.start_transaction();
    ///
    /// let subtree_key = b"subtree_key";
    /// db.insert(
    ///     [TEST_LEAF].as_ref(),
    ///     subtree_key,
    ///     Element::empty_tree(),
    ///     None,
    ///     Some(&tx),
    ///     grove_version,
    /// )
    /// .unwrap()?;
    ///
    /// // This action exists only inside the transaction for now
    /// let result = db
    ///     .get([TEST_LEAF].as_ref(), subtree_key, None, grove_version)
    ///     .unwrap();
    /// assert!(matches!(result, Err(Error::PathKeyNotFound(_))));
    ///
    /// // To access values inside the transaction, transaction needs to be passed to the `db::get`
    /// let result_with_transaction = db
    ///     .get([TEST_LEAF].as_ref(), subtree_key, Some(&tx), grove_version)
    ///     .unwrap()?;
    /// assert_eq!(result_with_transaction, Element::empty_tree());
    ///
    /// // After transaction is committed, the value from it can be accessed normally.
    /// let _ = db.commit_transaction(tx);
    /// let result = db
    ///     .get([TEST_LEAF].as_ref(), subtree_key, None, grove_version)
    ///     .unwrap()?;
    /// assert_eq!(result, Element::empty_tree());
    ///
    /// # Ok(())
    /// # }
    /// ```
    pub fn start_transaction(&self) -> Transaction<'_> {
        self.db.start_transaction()
    }

    /// Commits previously started db transaction. For more details on the
    /// transaction usage, please check [`GroveDb::start_transaction`]
    pub fn commit_transaction(&self, transaction: Transaction) -> CostResult<(), Error> {
        self.db.commit_transaction(transaction).map_err(Into::into)
    }

    /// Rollbacks previously started db transaction to initial state.
    /// For more details on the transaction usage, please check
    /// [`GroveDb::start_transaction`]
    pub fn rollback_transaction(&self, transaction: &Transaction) -> Result<(), Error> {
        Ok(self.db.rollback_transaction(transaction)?)
    }

    /// Method to visualize hash mismatch after verification
    pub fn visualize_verify_grovedb(
        &self,
        transaction: TransactionArg,
        verify_references: bool,
        allow_cache: bool,
        grove_version: &GroveVersion,
    ) -> Result<HashMap<String, (String, String, String)>, Error> {
        Ok(self
            .verify_grovedb(transaction, verify_references, allow_cache, grove_version)?
            .iter()
            .map(|(path, (root_hash, expected, actual))| {
                (
                    path.iter()
                        .map(hex::encode)
                        .collect::<Vec<String>>()
                        .join("/"),
                    (
                        hex::encode(root_hash),
                        hex::encode(expected),
                        hex::encode(actual),
                    ),
                )
            })
            .collect())
    }

    /// Method to check that the value_hash of Element::Tree nodes are computed
    /// correctly.
    pub fn verify_grovedb(
        &self,
        transaction: TransactionArg,
        verify_references: bool,
        allow_cache: bool,
        grove_version: &GroveVersion,
    ) -> Result<VerificationIssues, Error> {
        let tx = TxRef::new(&self.db, transaction);

        let root_merk = self
            .open_transactional_merk_at_path(SubtreePath::empty(), tx.as_ref(), None, grove_version)
            .unwrap()?;
        self.verify_merk_and_submerks_in_transaction(
            root_merk,
            &SubtreePath::empty(),
            None,
            tx.as_ref(),
            verify_references,
            allow_cache,
            grove_version,
        )
    }

    fn verify_merk_and_submerks_in_transaction<'db, B: AsRef<[u8]>, S: StorageContext<'db>>(
        &'db self,
        merk: Merk<S>,
        path: &SubtreePath<B>,
        batch: Option<&'db StorageBatch>,
        transaction: &Transaction,
        verify_references: bool,
        allow_cache: bool,
        grove_version: &GroveVersion,
    ) -> Result<VerificationIssues, Error> {
        let mut all_query = Query::new();
        all_query.insert_all();

        let mut issues = HashMap::new();
        let mut element_iterator = KVIterator::new(merk.storage.raw_iter(), &all_query).unwrap();

        while let Some((key, element_value)) = element_iterator.next_kv().unwrap() {
            let element = Element::raw_decode(&element_value, grove_version)?;
            match element {
                Element::SumTree(..)
                | Element::Tree(..)
                | Element::BigSumTree(..)
                | Element::CountTree(..)
                | Element::CountSumTree(..)
                | Element::ProvableCountTree(..)
                | Element::ProvableCountSumTree(..) => {
                    let (kv_value, element_value_hash) = merk
                        .get_value_and_value_hash(
                            &key,
                            allow_cache,
                            None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                            grove_version,
                        )
                        .unwrap()
                        .map_err(MerkError)?
                        .ok_or(Error::CorruptedData(format!(
                            "expected merk to contain value at key {} for {}",
                            hex_to_ascii(&key),
                            element.type_str()
                        )))?;
                    let new_path = path.derive_owned_with_child(key);
                    let new_path_ref = SubtreePath::from(&new_path);

                    let inner_merk = self
                        .open_transactional_merk_at_path(
                            new_path_ref.clone(),
                            transaction,
                            batch,
                            grove_version,
                        )
                        .unwrap()?;
                    let root_hash = inner_merk.root_hash().unwrap();

                    let actual_value_hash = value_hash(&kv_value).unwrap();
                    let combined_value_hash = combine_hash(&actual_value_hash, &root_hash).unwrap();

                    if combined_value_hash != element_value_hash {
                        issues.insert(
                            new_path.to_vec(),
                            (root_hash, combined_value_hash, element_value_hash),
                        );
                    }
                    issues.extend(self.verify_merk_and_submerks_in_transaction(
                        inner_merk,
                        &new_path_ref,
                        batch,
                        transaction,
                        verify_references,
                        true,
                        grove_version,
                    )?);
                }
                Element::Item(..) | Element::SumItem(..) | Element::ItemWithSumItem(..) => {
                    let (kv_value, element_value_hash) = merk
                        .get_value_and_value_hash(
                            &key,
                            allow_cache,
                            None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                            grove_version,
                        )
                        .unwrap()
                        .map_err(MerkError)?
                        .ok_or(Error::CorruptedData(format!(
                            "expected merk to contain value at key {} for {}",
                            hex_to_ascii(&key),
                            element.type_str()
                        )))?;
                    let actual_value_hash = value_hash(&kv_value).unwrap();
                    if actual_value_hash != element_value_hash {
                        issues.insert(
                            path.derive_owned_with_child(key).to_vec(),
                            (actual_value_hash, element_value_hash, actual_value_hash),
                        );
                    }
                }
                Element::Reference(ref reference_path, ..) => {
                    // Skip this whole check if we don't `verify_references`
                    if !verify_references {
                        continue;
                    }

                    // Merk we're checking:
                    let (kv_value, element_value_hash) = merk
                        .get_value_and_value_hash(
                            &key,
                            allow_cache,
                            None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                            grove_version,
                        )
                        .unwrap()
                        .map_err(MerkError)?
                        .ok_or(Error::CorruptedData(format!(
                            "expected merk to contain value at key {} for reference",
                            hex_to_ascii(&key)
                        )))?;

                    let referenced_value_hash = {
                        let full_path = path_from_reference_path_type(
                            reference_path.clone(),
                            &path.to_vec(),
                            Some(&key),
                        )?;
                        let item = self
                            .follow_reference(
                                (full_path.as_slice()).into(),
                                allow_cache,
                                Some(transaction),
                                grove_version,
                            )
                            .unwrap()?;
                        item.value_hash(grove_version).unwrap()?
                    };

                    // Take the current item (reference) hash and combine it with referenced value's
                    // hash
                    let self_actual_value_hash = value_hash(&kv_value).unwrap();
                    let combined_value_hash =
                        combine_hash(&self_actual_value_hash, &referenced_value_hash).unwrap();

                    if combined_value_hash != element_value_hash {
                        issues.insert(
                            path.derive_owned_with_child(key).to_vec(),
                            (combined_value_hash, element_value_hash, combined_value_hash),
                        );
                    }
                }
            }
        }
        Ok(issues)
    }
}
