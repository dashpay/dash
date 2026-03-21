use std::{
    collections::{BTreeMap, BTreeSet},
    fmt,
    marker::PhantomPinned,
    mem,
    pin::Pin,
};

use grovedb_merk::{
    tree::{kv::ValueDefinedCostType, value_hash},
    tree_type::TreeType,
    CryptoHash, Restorer,
};
use grovedb_path::SubtreePath;
use grovedb_storage::{
    rocksdb_storage::{PrefixedRocksDbImmediateStorageContext, RocksDbStorage},
    StorageContext,
};
use grovedb_version::version::GroveVersion;

use super::{
    utils::{decode_vec_ops, encode_global_chunk_id, path_to_string},
    CURRENT_STATE_SYNC_VERSION,
};
use crate::{
    element::elements_iterator::ElementIteratorExtensions,
    replication,
    replication::utils::{pack_nested_bytes, unpack_nested_bytes},
    Element, Error, GroveDb, Transaction,
};

/// Number of elements packed together
pub const CONST_GROUP_PACKING_SIZE: usize = 32;

pub(crate) type SubtreePrefix = [u8; 32];

/// Struct governing the state synchronization of one subtree.
struct SubtreeStateSyncInfo<'db> {
    /// Current Chunk restorer
    restorer: Restorer<PrefixedRocksDbImmediateStorageContext<'db>>,

    /// Set of global chunk ids requested to be fetched and pending for
    /// processing. For the description of global chunk id check
    /// fetch_chunk().
    pending_chunks: BTreeSet<Vec<u8>>,

    /// Tree root key
    root_key: Option<Vec<u8>>,

    /// The type of tree
    tree_type: TreeType,

    /// Path of current tree
    current_path: Vec<Vec<u8>>,

    /// Number of processed chunks in current prefix (Path digest)
    num_processed_chunks: usize,
}

impl SubtreeStateSyncInfo<'_> {
    /// Applies a chunk using the given `SubtreeStateSyncInfo`.
    ///
    /// # Parameters
    /// - `chunk_id`: A byte slice representing the local chunk ID to be
    ///   applied.
    /// - `chunk_data`: A byte slice containing the chunk proof operators,
    ///   encoded as bytes.
    /// - `grove_version`: A reference to the `GroveVersion` being used for
    ///   synchronization.
    ///
    /// # Returns
    /// - `Ok(Vec<Vec<u8>>)`: A vector of global chunk IDs (each represented as
    ///   a vector of bytes) that can be fetched from sources for further
    ///   synchronization. Ownership of the `SubtreeStateSyncInfo` is
    ///   transferred back to the caller.
    /// - `Err(Error)`: An error if the chunk cannot be applied.
    ///
    /// # Behavior
    /// - The function consumes the provided `SubtreeStateSyncInfo` to apply the
    ///   given chunk.
    /// - Once the chunk is applied, the function calculates and returns the
    ///   next set of global chunk IDs required for further state
    ///   synchronization.
    ///
    /// # Usage
    /// This function is called as part of the state sync process to apply
    /// received chunks and advance the synchronization state.
    ///
    /// # Notes
    /// - Ensure that the `chunk_data` is correctly encoded and matches the
    ///   expected format.
    /// - The function modifies the state of the synchronization process, so it
    ///   must be used carefully to maintain correctness.
    fn apply_inner_chunk(
        &mut self,
        chunk_id: &[u8],
        chunk_data: &[u8],
        grove_version: &GroveVersion,
    ) -> Result<Vec<Vec<u8>>, Error> {
        let mut res = vec![];

        if !self.pending_chunks.contains(chunk_id) {
            return Err(Error::InternalError(
                "Incoming global_chunk_id not expected".to_string(),
            ));
        }
        self.pending_chunks.remove(chunk_id);
        if !chunk_data.is_empty() {
            match decode_vec_ops(chunk_data) {
                Ok(ops) => {
                    match self.restorer.process_chunk(chunk_id, ops, grove_version) {
                        Ok(next_chunk_ids) => {
                            self.num_processed_chunks += 1;
                            for next_chunk_id in next_chunk_ids {
                                self.pending_chunks.insert(next_chunk_id.clone());
                                res.push(next_chunk_id);
                            }
                        }
                        _ => {
                            return Err(Error::InternalError(
                                "Unable to process incoming chunk".to_string(),
                            ));
                        }
                    };
                }
                Err(_) => {
                    return Err(Error::CorruptedData(
                        "Unable to decode incoming chunk".to_string(),
                    ));
                }
            }
        }

        Ok(res)
    }
}

impl<'tx> SubtreeStateSyncInfo<'tx> {
    pub fn new(restorer: Restorer<PrefixedRocksDbImmediateStorageContext<'tx>>) -> Self {
        SubtreeStateSyncInfo {
            restorer,
            root_key: None,
            tree_type: TreeType::NormalTree,
            pending_chunks: Default::default(),
            current_path: vec![],
            num_processed_chunks: 0,
        }
    }
}

/// Struct governing the state synchronization process.
pub struct MultiStateSyncSession<'db> {
    /// GroveDb instance to apply changes to
    db: &'db GroveDb,

    /// Map of currently processing subtrees.
    /// Keys are `SubtreePrefix` (path digests), and values are
    /// `SubtreeStateSyncInfo` for each subtree.
    current_prefixes: BTreeMap<SubtreePrefix, SubtreeStateSyncInfo<'db>>,

    /// Set of processed prefixes, represented as `SubtreePrefix` (path
    /// digests).
    processed_prefixes: BTreeSet<SubtreePrefix>,

    /// Root application hash (`app_hash`).
    app_hash: [u8; 32],

    /// Version of the state synchronization protocol.
    pub(crate) version: u16,

    /// Maximum number of subtrees that can be processed in a single batch.
    subtrees_batch_size: usize,

    /// Counter tracking the number of subtrees processed in the current batch.
    num_processed_subtrees_in_batch: usize,

    /// Metadata for newly discovered subtrees that are pending processing.
    pending_discovered_subtrees: Option<SubtreesMetadata>,

    /// Transaction used for the synchronization process.
    /// This is placed last to ensure it is dropped last.
    transaction: Transaction<'db>,

    /// Marker to ensure this struct is not moved in memory.
    _pin: PhantomPinned,
}

impl<'db> MultiStateSyncSession<'db> {
    /// Initializes a new state sync session.
    pub fn new(db: &'db GroveDb, app_hash: [u8; 32], subtrees_batch_size: usize) -> Pin<Box<Self>> {
        Box::pin(MultiStateSyncSession {
            db,
            transaction: db.start_transaction(),
            current_prefixes: Default::default(),
            processed_prefixes: Default::default(),
            app_hash,
            version: CURRENT_STATE_SYNC_VERSION,
            subtrees_batch_size,
            num_processed_subtrees_in_batch: 0,
            pending_discovered_subtrees: None,
            _pin: PhantomPinned,
        })
    }

    pub fn is_empty(&self) -> bool {
        self.current_prefixes.is_empty()
    }

    pub fn is_sync_completed(&self) -> bool {
        for (_, subtree_state_info) in self.current_prefixes.iter() {
            if !subtree_state_info.pending_chunks.is_empty() {
                return false;
            }
        }

        if self.pending_discovered_subtrees.is_some() {
            return false;
        }

        true
    }

    pub fn commit(self: Pin<Box<Self>>) -> Result<(), Error> {
        // SAFETY: the struct isn't used anymore and no storage contexts would acccess
        // transaction
        let session = unsafe { Pin::into_inner_unchecked(self) };
        session.db.commit_transaction(session.transaction).unwrap()
    }

    // SAFETY: This is unsafe as it requires `self.current_prefixes` to be empty
    unsafe fn set_new_transaction(
        self: &mut Pin<Box<MultiStateSyncSession<'db>>>,
    ) -> Result<(), Error> {
        let this = Pin::as_mut(self).get_unchecked_mut();
        let old_tx = mem::replace(&mut this.transaction, this.db.start_transaction());
        self.db.commit_transaction(old_tx).unwrap()
    }

    /// Adds synchronization information for a subtree into the current
    /// synchronization session.
    ///
    /// This function interacts with a `GroveDb` database to open a Merk tree at
    /// the specified path, calculate and verify its cryptographic hashes,
    /// and update the session state with the relevant synchronization
    /// information. The function generates and returns the global chunk ID for
    /// the subtree.
    ///
    /// # Parameters
    /// - `self`: A pinned, boxed instance of the `MultiStateSyncSession`.
    /// - `db`: A reference to the `GroveDb` instance.
    /// - `path`: The path to the subtree as a `SubtreePath`.
    /// - `hash`: The expected cryptographic hash of the subtree.
    /// - `actual_hash`: An optional actual cryptographic hash to compare
    ///   against the expected hash.
    /// - `chunk_prefix`: A 32-byte prefix used for identifying chunks in the
    ///   synchronization process.
    /// - `grove_version`: The GroveDB version to use for processing.
    ///
    /// # Returns
    /// - `Ok(Vec<u8>)`: On success, returns the encoded global chunk ID for the
    ///   subtree.
    /// - `Err(Error)`: If the Merk tree cannot be opened or synchronization
    ///   information cannot be added.
    ///
    /// # Errors
    /// This function returns an error if:
    /// - The Merk tree at the specified path cannot be opened.
    /// - Any synchronization-related operations fail.
    /// - Internal errors occur during processing.
    ///
    /// # Safety
    /// - This function uses unsafe code to create a reference to the
    ///   transaction. Ensure that the transaction is properly managed and the
    ///   lifetime guarantees are respected.
    pub fn add_subtree_sync_info<'b, B: AsRef<[u8]>>(
        self: &mut Pin<Box<MultiStateSyncSession<'db>>>,
        path: SubtreePath<'b, B>,
        hash: CryptoHash,
        actual_hash: Option<CryptoHash>,
        chunk_prefix: [u8; 32],
        grove_version: &GroveVersion,
    ) -> Result<Vec<u8>, Error> {
        let transaction_ref: &'db Transaction<'db> = unsafe {
            let tx: &Transaction<'db> = &self.as_ref().transaction;
            &*(tx as *const _)
        };

        if let Ok((merk, root_key, tree_type)) =
            self.db
                .open_merk_for_replication(path.clone(), transaction_ref, grove_version)
        {
            let restorer = Restorer::new(merk, hash, actual_hash);
            let mut sync_info = SubtreeStateSyncInfo::new(restorer);
            sync_info.pending_chunks.insert(vec![]);
            sync_info.root_key = root_key.clone();
            sync_info.tree_type = tree_type;
            sync_info.current_path = path.to_vec();
            self.as_mut()
                .current_prefixes()
                .insert(chunk_prefix, sync_info);
            Ok(encode_global_chunk_id(
                chunk_prefix,
                root_key,
                tree_type,
                vec![],
            ))
        } else {
            Err(Error::InternalError(
                "Unable to open merk for replication".to_string(),
            ))
        }
    }

    fn current_prefixes(
        self: Pin<&mut MultiStateSyncSession<'db>>,
    ) -> &mut BTreeMap<SubtreePrefix, SubtreeStateSyncInfo<'db>> {
        // SAFETY: no memory-sensitive assumptions are made about fields except the
        // `transaciton` so it will be safe to modify them
        &mut unsafe { self.get_unchecked_mut() }.current_prefixes
    }

    fn processed_prefixes(
        self: Pin<&mut MultiStateSyncSession<'db>>,
    ) -> &mut BTreeSet<SubtreePrefix> {
        // SAFETY: no memory-sensitive assumptions are made about fields except the
        // `transaciton` so it will be safe to modify them
        &mut unsafe { self.get_unchecked_mut() }.processed_prefixes
    }

    fn num_processed_subtrees_in_batch(self: Pin<&mut MultiStateSyncSession<'db>>) -> &mut usize {
        // SAFETY: no memory-sensitive assumptions are made about fields except the
        // `transaciton` so it will be safe to modify them
        &mut unsafe { self.get_unchecked_mut() }.num_processed_subtrees_in_batch
    }

    fn pending_discovered_subtrees(
        self: Pin<&mut MultiStateSyncSession<'db>>,
    ) -> &mut Option<SubtreesMetadata> {
        // SAFETY: no memory-sensitive assumptions are made about fields except the
        // `transaciton` so it will be safe to modify them
        &mut unsafe { self.get_unchecked_mut() }.pending_discovered_subtrees
    }

    /// Applies a chunk during the state synchronization process.
    /// This method should be called by ABCI when the `ApplySnapshotChunk`
    /// method is invoked.
    ///
    /// # Parameters
    /// - `self`: A pinned mutable reference to the `MultiStateSyncSession`.
    /// - `db`: A reference to the `GroveDb` instance used for synchronization.
    /// - `packed_global_chunk_ids`: A byte slice representing the packed global
    ///   chunk IDs being applied.
    /// - `packed_global_chunks`: A byte slice containing packed encoded proof
    ///   for the chunk.
    /// - `version`: The state synchronization protocol version being used.
    /// - `grove_version`: A reference to the `GroveVersion` specifying the
    ///   GroveDB version.
    ///
    /// # Returns
    /// - `Ok(Vec<Vec<u8>>)`: A tuple of: vector of global chunk IDs (each
    ///   represented as a vector of bytes) that can be fetched from sources for
    ///   further synchronization.
    /// - `Err(Error)`: An error if the chunk application fails or if the chunk
    ///   proof is invalid.
    ///
    /// # Behavior
    /// - This method applies the given chunk using the provided
    ///   `global_chunk_id` and its corresponding proof data (`chunk`).
    /// - Once the chunk is applied successfully, it calculates and returns the
    ///   next set of global chunk IDs required for further synchronization.
    ///
    /// # Notes
    /// - Ensure the `chunk` is correctly encoded and matches the expected proof
    ///   format.
    /// - This function modifies the state of the synchronization session, so it
    ///   must be used carefully to maintain correctness and avoid errors.
    /// - The pinned `self` ensures that the session cannot be moved in memory,
    ///   preserving consistency during the synchronization process.
    pub fn apply_chunk(
        self: &mut Pin<Box<MultiStateSyncSession<'db>>>,
        packed_global_chunk_ids: &[u8],
        packed_global_chunks: &[u8],
        version: u16,
        grove_version: &GroveVersion,
    ) -> Result<Vec<Vec<u8>>, Error> {
        // For now, only CURRENT_STATE_SYNC_VERSION is supported
        if version != CURRENT_STATE_SYNC_VERSION {
            return Err(Error::CorruptedData(
                "Unsupported state sync protocol version".to_string(),
            ));
        }
        if version != self.version {
            return Err(Error::CorruptedData(
                "Unsupported state sync protocol version".to_string(),
            ));
        }

        let mut nested_global_chunk_ids: Vec<Vec<u8>> = vec![];
        let mut nested_global_chunks: Vec<Vec<u8>> = vec![];
        if self.app_hash == packed_global_chunk_ids {
            nested_global_chunk_ids = vec![packed_global_chunk_ids.to_vec()];
            nested_global_chunks = unpack_nested_bytes(packed_global_chunks)?;
        } else {
            nested_global_chunk_ids.extend(unpack_nested_bytes(packed_global_chunk_ids)?);
            nested_global_chunks.extend(unpack_nested_bytes(packed_global_chunks)?);
        }

        if nested_global_chunk_ids.len() != nested_global_chunks.len() {
            return Err(Error::InternalError(
                "Packed num of global chunkIDs and chunks are not matching".to_string(),
            ));
        }
        if self.is_empty() {
            return Err(Error::InternalError(
                "GroveDB is not in syncing mode".to_string(),
            ));
        }

        let mut next_global_chunk_ids: Vec<Vec<u8>> = vec![];

        for (iter_global_chunk_id, iter_packed_chunks) in nested_global_chunk_ids
            .iter()
            .zip(nested_global_chunks.iter())
        {
            let mut next_chunk_ids = vec![];

            let (chunk_prefix, _, _, nested_local_chunk_ids) =
                replication::utils::decode_global_chunk_id(
                    iter_global_chunk_id.as_slice(),
                    &self.app_hash,
                )?;

            let it_chunk_ids = if nested_local_chunk_ids.is_empty() {
                vec![vec![]]
            } else {
                nested_local_chunk_ids
            };

            let current_nested_chunk_data = unpack_nested_bytes(iter_packed_chunks.as_slice())?;

            if it_chunk_ids.len() != current_nested_chunk_data.len() {
                return Err(Error::InternalError(
                    "Packed num of chunkIDs and chunks are not matching #2".to_string(),
                ));
            }

            let current_prefixes = self.as_mut().current_prefixes();
            let Some(subtree_state_sync) = current_prefixes.get_mut(&chunk_prefix) else {
                return Err(Error::InternalError(
                    "Unable to process incoming chunk".to_string(),
                ));
            };

            let mut next_local_chunk_ids = vec![];
            for (current_local_chunk_id, current_local_chunks) in
                it_chunk_ids.iter().zip(current_nested_chunk_data.iter())
            {
                next_local_chunk_ids.extend(subtree_state_sync.apply_inner_chunk(
                    current_local_chunk_id.as_slice(),
                    current_local_chunks.as_slice(),
                    grove_version,
                )?);
            }

            if !next_local_chunk_ids.is_empty() {
                for grouped_ids in next_local_chunk_ids.chunks(CONST_GROUP_PACKING_SIZE) {
                    next_chunk_ids.push(encode_global_chunk_id(
                        chunk_prefix,
                        subtree_state_sync.root_key.clone(),
                        subtree_state_sync.tree_type,
                        grouped_ids.to_vec(),
                    ));
                }
                next_global_chunk_ids.extend(next_chunk_ids);
            } else if subtree_state_sync.pending_chunks.is_empty() {
                let completed_path = subtree_state_sync.current_path.clone();

                // Subtree is finished. We can save it.
                let is_subtree_empty = subtree_state_sync.num_processed_chunks == 0;
                if let Some(prefix_data) = current_prefixes.remove(&chunk_prefix) {
                    if !is_subtree_empty {
                        if let Err(err) = prefix_data.restorer.finalize(grove_version) {
                            return Err(Error::InternalError(format!(
                                "Unable to finalize Merk: {:?}",
                                err
                            )));
                        }
                    }
                } else {
                    return Err(Error::InternalError(format!(
                        "Prefix {:?} does not exist in current_prefixes",
                        chunk_prefix
                    )));
                }

                self.as_mut().processed_prefixes().insert(chunk_prefix);

                *self.as_mut().num_processed_subtrees_in_batch() += 1;

                let new_subtrees_metadata =
                    self.discover_new_subtrees_metadata(&completed_path, grove_version)?;

                if self.num_processed_subtrees_in_batch >= self.subtrees_batch_size {
                    match self.as_mut().pending_discovered_subtrees() {
                        None => {
                            *self.as_mut().pending_discovered_subtrees() =
                                Some(new_subtrees_metadata);
                        }
                        Some(existing_subtrees_metadata) => {
                            existing_subtrees_metadata
                                .data
                                .extend(new_subtrees_metadata.data);
                        }
                    }
                } else if let Ok(res) =
                    self.prepare_sync_state_sessions(new_subtrees_metadata, grove_version)
                {
                    next_chunk_ids.extend(res);
                    next_global_chunk_ids.extend(next_chunk_ids);
                } else {
                    return Err(Error::InternalError(
                        "Unable to discover Subtrees".to_string(),
                    ));
                }
            }
        }

        if self.num_processed_subtrees_in_batch >= self.subtrees_batch_size
            && self.current_prefixes.is_empty()
        {
            // SAFETY: we made sure `self.current_prefixes` is empty so there are no
            // references to the transaction we're about to replace
            unsafe {
                self.set_new_transaction()?;
            }

            let new_subtrees_metadata =
                self.as_mut()
                    .pending_discovered_subtrees()
                    .take()
                    .ok_or(Error::CorruptedData(
                        "No pending subtrees available for resume_sync".to_string(),
                    ))?;
            *self.as_mut().num_processed_subtrees_in_batch() = 0;

            let mut next_chunk_ids = vec![];

            if let Ok(discovered_chunk_ids) =
                self.prepare_sync_state_sessions(new_subtrees_metadata, grove_version)
            {
                next_chunk_ids.extend(discovered_chunk_ids);
                next_global_chunk_ids.extend(next_chunk_ids);
            } else {
                return Err(Error::InternalError(
                    "Unable to discover Subtrees".to_string(),
                ));
            }
        }

        let mut res: Vec<Vec<u8>> = vec![];
        for grouped_next_global_chunk_ids in next_global_chunk_ids.chunks(CONST_GROUP_PACKING_SIZE)
        {
            res.push(pack_nested_bytes(grouped_next_global_chunk_ids.to_vec()));
        }

        Ok(res)
    }

    /// Discovers new subtrees at the given path that need to be synchronized.
    ///
    /// # Parameters
    /// - `self`: A pinned mutable reference to the `MultiStateSyncSession`.
    /// - `db`: A reference to the `GroveDb` instance being used for
    ///   synchronization.
    /// - `path_vec`: A vector of byte vectors representing the path where
    ///   subtrees should be discovered.
    /// - `grove_version`: A reference to the `GroveVersion` specifying the
    ///   GroveDB version.
    ///
    /// # Returns
    /// - `Ok(SubtreesMetadata)`: Metadata about the discovered subtrees,
    ///   including information necessary for their synchronization.
    /// - `Err(Error)`: An error if the discovery process fails.
    ///
    /// # Behavior
    /// - This function traverses the specified `path_vec` in the database and
    ///   identifies subtrees that are not yet synchronized.
    /// - Returns metadata about these subtrees, which can be used to initiate
    ///   or manage the synchronization process.
    ///
    /// # Notes
    /// - The `path_vec` should represent a valid path in the GroveDB where
    ///   subtrees are expected to exist.
    /// - Ensure that the GroveDB instance (`db`) and Grove version
    ///   (`grove_version`) are compatible and up-to-date to avoid errors during
    ///   discovery.
    /// - The function modifies the state of the synchronization session, so it
    ///   should be used carefully to maintain session integrity.
    fn discover_new_subtrees_metadata(
        self: &mut Pin<Box<MultiStateSyncSession<'db>>>,
        path_vec: &[Vec<u8>],
        grove_version: &GroveVersion,
    ) -> Result<SubtreesMetadata, Error> {
        let transaction_ref: &'db Transaction<'db> = unsafe {
            let tx: &Transaction<'db> = &self.as_ref().transaction;
            &*(tx as *const _)
        };
        let subtree_path: Vec<&[u8]> = path_vec.iter().map(|vec| vec.as_slice()).collect();
        let path: &[&[u8]] = &subtree_path;
        let merk = self
            .db
            .open_transactional_merk_at_path(path.into(), transaction_ref, None, grove_version)
            .value
            .map_err(|e| Error::CorruptedData(format!("failed to open merk by path-tx:{}", e)))?;
        if merk.is_empty_tree().unwrap() {
            return Ok(SubtreesMetadata::default());
        }
        let mut subtree_keys = BTreeSet::new();

        let mut raw_iter = Element::iterator(merk.storage.raw_iter()).unwrap();
        while let Some((key, value)) = raw_iter.next_element(grove_version).unwrap()? {
            if value.is_any_tree() {
                subtree_keys.insert(key.to_vec());
            }
        }

        let mut subtrees_metadata = SubtreesMetadata::new();
        for subtree_key in &subtree_keys {
            if let Ok(Some((elem_value, elem_value_hash))) = merk
                .get_value_and_value_hash(
                    subtree_key.as_slice(),
                    true,
                    None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                    grove_version,
                )
                .value
            {
                let actual_value_hash = value_hash(&elem_value).unwrap();
                let mut new_path = path_vec.to_vec();
                new_path.push(subtree_key.to_vec());

                let subtree_path: Vec<&[u8]> = new_path.iter().map(|vec| vec.as_slice()).collect();
                let path: &[&[u8]] = &subtree_path;
                let prefix = RocksDbStorage::build_prefix(path.as_ref().into()).unwrap();

                subtrees_metadata.data.insert(
                    prefix,
                    (new_path.to_vec(), actual_value_hash, elem_value_hash),
                );
            }
        }

        Ok(subtrees_metadata)
    }

    /// Prepares a synchronization session for the newly discovered subtrees and
    /// returns the global chunk IDs of those subtrees.
    ///
    /// # Parameters
    /// - `self`: A pinned mutable reference to the `MultiStateSyncSession`.
    /// - `db`: A reference to the `GroveDb` instance used for managing the
    ///   synchronization process.
    /// - `subtrees_metadata`: Metadata about the discovered subtrees that
    ///   require synchronization.
    /// - `grove_version`: A reference to the `GroveVersion` specifying the
    ///   GroveDB version.
    ///
    /// # Returns
    /// - `Ok(Vec<Vec<u8>>)`: A vector of global chunk IDs (each represented as
    ///   a vector of bytes) corresponding to the newly discovered subtrees.
    ///   These IDs can be fetched from sources to continue the synchronization
    ///   process.
    /// - `Err(Error)`: An error if the synchronization session could not be
    ///   prepared or if processing the metadata fails.
    ///
    /// # Behavior
    /// - Initializes the synchronization state for the newly discovered
    ///   subtrees based on the provided metadata.
    /// - Calculates and returns the global chunk IDs of these subtrees,
    ///   enabling further state synchronization.
    ///
    /// # Notes
    /// - Ensure that the `subtrees_metadata` accurately reflects the subtrees
    ///   requiring synchronization.
    /// - This function modifies the state of the synchronization session to
    ///   include the new subtrees.
    /// - Proper handling of the returned global chunk IDs is essential to
    ///   ensure seamless state synchronization.
    fn prepare_sync_state_sessions(
        self: &mut Pin<Box<MultiStateSyncSession<'db>>>,
        subtrees_metadata: SubtreesMetadata,
        grove_version: &GroveVersion,
    ) -> Result<Vec<Vec<u8>>, Error> {
        let mut res = vec![];

        for (prefix, prefix_metadata) in &subtrees_metadata.data {
            if !self.processed_prefixes.contains(prefix)
                && !self.current_prefixes.contains_key(prefix)
            {
                let (current_path, actual_value_hash, elem_value_hash) = &prefix_metadata;

                let subtree_path: Vec<&[u8]> =
                    current_path.iter().map(|vec| vec.as_slice()).collect();
                let path: &[&[u8]] = &subtree_path;

                let next_chunks_ids = self.add_subtree_sync_info(
                    path.into(),
                    *elem_value_hash,
                    Some(*actual_value_hash),
                    *prefix,
                    grove_version,
                )?;

                res.push(next_chunks_ids);
            }
        }

        Ok(res)
    }
}

/// Struct containing metadata about the current subtrees found in GroveDB.
/// This metadata is used during the state synchronization process to track
/// discovered subtrees and verify their integrity after they are constructed.
pub struct SubtreesMetadata {
    /// A map where:
    /// - **Key**: `SubtreePrefix` (the path digest of the subtree).
    /// - **Value**: A tuple containing:
    ///   - `Vec<Vec<u8>>`: The actual path of the subtree in GroveDB.
    ///   - `CryptoHash`: The parent subtree's actual value hash.
    ///   - `CryptoHash`: The parent subtree's element value hash.
    ///
    /// The `parent subtree actual_value_hash` and `parent subtree
    /// elem_value_hash` are required to verify the integrity of the newly
    /// constructed subtree after synchronization.
    pub data: BTreeMap<SubtreePrefix, (Vec<Vec<u8>>, CryptoHash, CryptoHash)>,
}

impl SubtreesMetadata {
    pub fn new() -> SubtreesMetadata {
        SubtreesMetadata {
            data: BTreeMap::new(),
        }
    }
}

impl Default for SubtreesMetadata {
    fn default() -> Self {
        Self::new()
    }
}

impl fmt::Debug for SubtreesMetadata {
    fn fmt(&self, f: &mut fmt::Formatter) -> fmt::Result {
        for (prefix, metadata) in self.data.iter() {
            let metadata_path = &metadata.0;
            let metadata_path_str = path_to_string(metadata_path);
            writeln!(
                f,
                " prefix:{:?} -> path:{:?}",
                hex::encode(prefix),
                metadata_path_str,
            )?;
        }
        Ok(())
    }
}
