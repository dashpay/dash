mod state_sync_session;

use std::pin::Pin;

use grovedb_merk::{tree::hash::CryptoHash, tree_type::TreeType, ChunkProducer};
use grovedb_path::SubtreePath;
use grovedb_version::{check_grovedb_v0, version::GroveVersion};

pub use self::state_sync_session::MultiStateSyncSession;
use crate::{
    replication::utils::{pack_nested_bytes, unpack_nested_bytes},
    util::TxRef,
    Error, GroveDb, TransactionArg,
};

/// Type alias representing a chunk identifier in the state synchronization
/// process.
///
/// - `SubtreePrefix`: The prefix of the subtree (32 bytes).
/// - `Option<Vec<u8>>`: The root key, which may be `None` if not present.
/// - `bool`: Indicates whether the tree is a sum tree.
/// - `Vec<Vec<u8>>`: Vector containing the chunk ID representing traversal
///   instructions.
pub type ChunkIdentifier = (
    crate::SubtreePrefix,
    Option<Vec<u8>>,
    TreeType,
    Vec<Vec<u8>>,
);

pub const CURRENT_STATE_SYNC_VERSION: u16 = 1;

#[cfg(feature = "minimal")]
impl GroveDb {
    pub fn start_syncing_session(
        &self,
        app_hash: [u8; 32],
        subtrees_batch_size: usize,
    ) -> Pin<Box<MultiStateSyncSession<'_>>> {
        MultiStateSyncSession::new(self, app_hash, subtrees_batch_size)
    }

    pub fn commit_session(&self, session: Pin<Box<MultiStateSyncSession>>) -> Result<(), Error> {
        session
            .commit()
            .inspect_err(|e| eprintln!("Failed to commit session: {:?}", e))
    }

    /// Fetches a chunk of data from the database based on the given global
    /// chunk ID.
    ///
    /// This function retrieves the requested chunk using the provided packed
    /// global chunk ID and the specified transaction. It validates the
    /// protocol version before proceeding.
    ///
    /// # Parameters
    ///
    /// - `packed_global_chunk_id`: A reference to a byte slice representing the
    ///   packed global chunk ID.
    /// - `transaction`: The transaction context used for database operations.
    /// - `version`: The protocol version for state synchronization.
    /// - `grove_version`: A reference to the GroveDB versioning structure.
    ///
    /// # Returns
    ///
    /// - `Ok(Vec<u8>)`: A packed byte vector containing the requested chunk
    ///   data.
    /// - `Err(Error)`: An error if the fetch operation fails.
    ///
    /// # Errors
    ///
    /// - Returns `Error::CorruptedData` if the protocol version is unsupported.
    /// - Returns `Error::CorruptedData` if an issue occurs while opening the
    ///   database transaction.
    /// - Returns `Error::CorruptedData` if chunk encoding or retrieval fails.
    ///
    /// # Notes
    ///
    /// - Only `CURRENT_STATE_SYNC_VERSION` is supported.
    /// - If the `packed_global_chunk_id` matches the `root_app_hash` length, it
    ///   is treated as a single ID.
    /// - Otherwise, it is unpacked into multiple nested chunk IDs.
    /// - The function opens a `Merk` tree for each chunk and retrieves the
    ///   associated data.
    /// - Empty trees return an empty byte vector.
    pub fn fetch_chunk(
        &self,
        packed_global_chunk_id: &[u8],
        transaction: TransactionArg,
        version: u16,
        grove_version: &GroveVersion,
    ) -> Result<Vec<u8>, Error> {
        check_grovedb_v0!(
            "fetch_chunk",
            grove_version.grovedb_versions.replication.fetch_chunk
        );

        let tx = TxRef::new(&self.db, transaction);

        // For now, only CURRENT_STATE_SYNC_VERSION is supported
        if version != CURRENT_STATE_SYNC_VERSION {
            return Err(Error::CorruptedData(
                "Unsupported state sync protocol version".to_string(),
            ));
        }

        let mut global_chunk_ids: Vec<Vec<u8>> = vec![];
        let root_app_hash = self.root_hash(Some(tx.as_ref()), grove_version).value?;
        if packed_global_chunk_id.len() == root_app_hash.len() {
            global_chunk_ids.push(packed_global_chunk_id.to_vec());
        } else {
            global_chunk_ids.extend(unpack_nested_bytes(packed_global_chunk_id)?);
        }

        let mut global_chunk_bytes: Vec<Vec<u8>> = vec![];
        for global_chunk_id in global_chunk_ids {
            let (chunk_prefix, root_key, tree_type, nested_chunk_ids) =
                utils::decode_global_chunk_id(global_chunk_id.as_slice(), &root_app_hash)?;

            let mut local_chunk_bytes: Vec<Vec<u8>> = vec![];

            let merk = self
                .open_transactional_merk_by_prefix(
                    chunk_prefix,
                    root_key,
                    tree_type,
                    tx.as_ref(),
                    None,
                    grove_version,
                )
                .value
                .map_err(|e| {
                    Error::CorruptedData(format!(
                        "failed to open merk by prefix tx:{} with:{}",
                        hex::encode(chunk_prefix),
                        e
                    ))
                })?;
            if merk.is_empty_tree().unwrap() {
                local_chunk_bytes.push(vec![]);
            } else {
                let mut chunk_producer = ChunkProducer::new(&merk).map_err(|e| {
                    Error::CorruptedData(format!(
                        "failed to create chunk producer by prefix tx:{} with:{}",
                        hex::encode(chunk_prefix),
                        e
                    ))
                })?;
                for chunk_id in nested_chunk_ids
                    .is_empty()
                    .then(|| Vec::new())
                    .into_iter()
                    .chain(nested_chunk_ids.into_iter())
                {
                    let (chunk, _) =
                        chunk_producer
                            .chunk(&chunk_id, grove_version)
                            .map_err(|e| {
                                Error::CorruptedData(format!(
                                    "failed to apply chunk:{} with:{}",
                                    hex::encode(chunk_prefix),
                                    e
                                ))
                            })?;
                    let op_bytes = utils::encode_vec_ops(chunk).map_err(|e| {
                        Error::CorruptedData(format!(
                            "failed to encode chunk ops:{} with:{}",
                            hex::encode(chunk_prefix),
                            e
                        ))
                    })?;
                    local_chunk_bytes.push(op_bytes);
                }
            }
            global_chunk_bytes.push(pack_nested_bytes(local_chunk_bytes));
        }
        Ok(pack_nested_bytes(global_chunk_bytes))
    }

    /// Starts a state synchronization process for a snapshot with the given
    /// `app_hash` root hash. This method should be called by ABCI when the
    /// `OfferSnapshot` method is invoked.
    ///
    /// # Parameters
    /// - `app_hash`: The root hash of the application state to synchronize.
    /// - `subtrees_batch_size`: Maximum number of subtrees that can be
    ///   processed in a single batch.
    /// - `version`: The version of the state sync protocol to use.
    /// - `grove_version`: The version of GroveDB being used.
    ///
    /// # Returns
    /// - `Ok(Pin<Box<MultiStateSyncSession>>)`: A pinned, boxed
    ///   `MultiStateSyncSession` representing the new sync session. This
    ///   session allows for managing the synchronization process.
    /// - `Err(Error)`: An error indicating why the state sync process could not
    ///   be started.
    ///
    /// # Behavior
    /// - Initiates the state synchronization process by preparing the necessary
    ///   data and resources.
    /// - Returns the first set of global chunk IDs that can be fetched from
    ///   available sources.
    /// - A new sync session is created and managed internally, facilitating
    ///   further synchronization.
    ///
    /// # Usage
    /// This method is typically called as part of the ABCI `OfferSnapshot`
    /// workflow when a new snapshot synchronization process is required to
    /// bring the application state up to date.
    ///
    /// # Notes
    /// - The returned `MultiStateSyncSession` is pinned because its lifetime
    ///   may depend on asynchronous operations or other system resources that
    ///   require it to remain immovable in memory.
    /// - Ensure that `app_hash` corresponds to a valid snapshot to avoid
    ///   errors.
    pub fn start_snapshot_syncing(
        &self,
        app_hash: CryptoHash,
        subtrees_batch_size: usize,
        version: u16,
        grove_version: &GroveVersion,
    ) -> Result<Pin<Box<MultiStateSyncSession<'_>>>, Error> {
        check_grovedb_v0!(
            "start_snapshot_syncing",
            grove_version
                .grovedb_versions
                .replication
                .start_snapshot_syncing
        );
        // For now, only CURRENT_STATE_SYNC_VERSION is supported
        if version != CURRENT_STATE_SYNC_VERSION {
            return Err(Error::CorruptedData(
                "Unsupported state sync protocol version".to_string(),
            ));
        }

        if subtrees_batch_size == 0 {
            return Err(Error::InternalError(
                "subtrees_batch_size cannot be zero".to_string(),
            ));
        }

        let root_prefix = [0u8; 32];

        let mut session = self.start_syncing_session(app_hash, subtrees_batch_size);

        session.add_subtree_sync_info(
            SubtreePath::empty(),
            app_hash,
            None,
            root_prefix,
            grove_version,
        )?;

        Ok(session)
    }
}

pub(crate) mod utils {
    use grovedb_merk::{
        ed::Encode,
        proofs::{Decoder, Op},
        tree_type::TreeType,
    };

    use crate::{replication::ChunkIdentifier, Error};

    /// Converts a path, represented as a slice of byte vectors (`&[Vec<u8>]`),
    /// into a human-readable string representation for debugging purposes.
    ///
    /// # Parameters
    /// - `path`: A slice of byte vectors where each vector represents a segment
    ///   of the path.
    ///
    /// # Returns
    /// - `Vec<String>`: A vector of strings where each string is a
    ///   human-readable representation of a corresponding segment in the input
    ///   path. If a segment contains invalid UTF-8, it is replaced with the
    ///   placeholder string `"<NON_UTF8_PATH>"`.
    ///
    /// # Behavior
    /// - Each byte vector in the path is interpreted as a UTF-8 string. If the
    ///   conversion fails, the placeholder `"<NON_UTF8_PATH>"` is used instead.
    /// - This function is primarily intended for debugging and logging.
    ///
    /// # Notes
    /// - This function does not handle or normalize paths; it only provides a
    ///   human-readable representation.
    /// - Be cautious when using this for paths that might contain sensitive
    ///   data, as the output could be logged.
    pub fn path_to_string(path: &[Vec<u8>]) -> Vec<String> {
        let mut subtree_path_str: Vec<String> = vec![];
        for subtree in path {
            let string = std::str::from_utf8(subtree).unwrap_or("<NON_UTF8_PATH>");
            subtree_path_str.push(string.to_string());
        }
        subtree_path_str
    }

    /// Decodes a global chunk identifier into its constituent parts.
    ///
    /// This function takes a `global_chunk_id` and an `app_hash` and extracts
    /// the chunk prefix, root key (if any), tree type, and any nested chunk
    /// IDs.
    ///
    /// # Arguments
    ///
    /// * `global_chunk_id` - A byte slice representing the global chunk
    ///   identifier.
    /// * `app_hash` - A byte slice representing the application hash.
    ///
    /// # Returns
    ///
    /// Returns a `Result` containing a tuple of:
    /// - `SubtreePrefix`: The chunk prefix key.
    /// - `Option<Vec<u8>>`: The optional root key.
    /// - `TreeType`: The type of tree associated with the chunk.
    /// - `Vec<Vec<u8>>`: A list of nested chunk IDs.
    ///
    /// Returns an `Error` if decoding fails due to incorrect input format.
    ///
    /// # Errors
    ///
    /// This function returns an error if:
    /// - The `global_chunk_id` is shorter than 32 bytes.
    /// - The root key size cannot be decoded.
    /// - The root key cannot be fully extracted.
    /// - The tree type cannot be decoded.
    /// - The subtree prefix cannot be constructed.
    pub fn decode_global_chunk_id(
        global_chunk_id: &[u8],
        app_hash: &[u8],
    ) -> Result<ChunkIdentifier, Error> {
        let chunk_prefix_length: usize = 32;
        if global_chunk_id.len() < chunk_prefix_length {
            return Err(Error::CorruptedData(
                "expected global chunk id of at least 32 length".to_string(),
            ));
        }

        if global_chunk_id == app_hash {
            let root_chunk_prefix_key: crate::SubtreePrefix = [0u8; 32];
            return Ok((root_chunk_prefix_key, None, TreeType::NormalTree, vec![]));
        }

        let (chunk_prefix_key, remaining) = global_chunk_id.split_at(chunk_prefix_length);

        let root_key_size_length: usize = 1;
        if remaining.len() < root_key_size_length {
            return Err(Error::CorruptedData(
                "unable to decode root key size".to_string(),
            ));
        }
        let (root_key_size, remaining) = remaining.split_at(root_key_size_length);
        if remaining.len() < root_key_size[0] as usize {
            return Err(Error::CorruptedData(
                "unable to decode root key".to_string(),
            ));
        }
        let (root_key, remaining) = remaining.split_at(root_key_size[0] as usize);
        let tree_type_length: usize = 1;
        if remaining.len() < tree_type_length {
            return Err(Error::CorruptedData(
                "unable to decode root key".to_string(),
            ));
        }
        let (tree_type_byte, packed_chunk_ids) = remaining.split_at(tree_type_length);
        let tree_type = tree_type_byte[0].try_into()?;

        let nested_chunk_ids = unpack_nested_bytes(packed_chunk_ids)?;

        let subtree_prefix: crate::SubtreePrefix = chunk_prefix_key
            .try_into()
            .map_err(|_| Error::CorruptedData("unable to construct subtree".to_string()))?;

        if !root_key.is_empty() {
            Ok((
                subtree_prefix,
                Some(root_key.to_vec()),
                tree_type,
                nested_chunk_ids,
            ))
        } else {
            Ok((subtree_prefix, None, tree_type, nested_chunk_ids))
        }
    }

    /// Encodes a global chunk identifier from its components.
    ///
    /// This function constructs a global chunk identifier by concatenating the
    /// given subtree prefix, root key (if any), tree type, and nested chunk
    /// IDs into a single byte vector.
    ///
    /// # Arguments
    ///
    /// * `subtree_prefix` - A fixed-size byte array representing the subtree
    ///   prefix (Blake3 hash output length).
    /// * `root_key_opt` - An optional root key represented as a `Vec<u8>`.
    /// * `tree_type` - The type of tree associated with the chunk.
    /// * `chunk_ids` - A vector of nested chunk IDs to be packed.
    ///
    /// # Returns
    ///
    /// Returns a `Vec<u8>` representing the encoded global chunk identifier.
    pub fn encode_global_chunk_id(
        subtree_prefix: [u8; blake3::OUT_LEN],
        root_key_opt: Option<Vec<u8>>,
        tree_type: TreeType,
        chunk_ids: Vec<Vec<u8>>,
    ) -> Vec<u8> {
        let mut res = vec![];

        res.extend(subtree_prefix);

        if let Some(root_key) = root_key_opt {
            res.push(root_key.len() as u8);
            res.extend(root_key);
        } else {
            res.push(0u8);
        }

        res.push(tree_type as u8);

        res.extend(pack_nested_bytes(chunk_ids));

        res
    }

    /// Encodes a vector of operations (`Vec<Op>`) into a byte vector.
    ///
    /// # Parameters
    /// - `chunk`: A vector of `Op` operations to be encoded.
    ///
    /// # Returns
    /// - `Ok(Vec<u8>)`: A byte vector representing the encoded operations.
    /// - `Err(Error)`: An error if the encoding process fails.
    pub fn encode_vec_ops(chunk: Vec<Op>) -> Result<Vec<u8>, Error> {
        let mut res = vec![];
        for op in chunk {
            op.encode_into(&mut res)
                .map_err(|e| Error::CorruptedData(format!("unable to encode chunk: {}", e)))?;
        }
        Ok(res)
    }

    /// Decodes a byte vector into a vector of operations (`Vec<Op>`).
    ///
    /// # Parameters
    /// - `chunk`: A byte vector representing encoded operations.
    ///
    /// # Returns
    /// - `Ok(Vec<Op>)`: A vector of decoded `Op` operations.
    /// - `Err(Error)`: An error if the decoding process fails.
    pub fn decode_vec_ops(chunk: &[u8]) -> Result<Vec<Op>, Error> {
        let decoder = Decoder::new(chunk);
        let mut res = vec![];
        for op in decoder {
            match op {
                Ok(op) => res.push(op),
                Err(e) => {
                    return Err(Error::CorruptedData(format!(
                        "unable to decode chunk: {}",
                        e
                    )));
                }
            }
        }
        Ok(res)
    }

    /// Packs a vector of byte vectors (`Vec<Vec<u8>>`) into a single byte
    /// vector.
    ///
    /// The encoding format is as follows:
    /// 1. The first byte stores the number of elements.
    /// 2. Each element is prefixed with its length as a 2-byte (`u16`) value in
    ///    big-endian format.
    /// 3. The actual byte sequence of the element is then appended.
    ///
    /// # Arguments
    ///
    /// * `nested_bytes` - A vector of byte vectors (`Vec<Vec<u8>>`) to be
    ///   packed.
    ///
    /// # Returns
    ///
    /// A `Vec<u8>` containing the encoded representation of the nested byte
    /// vectors.
    pub fn pack_nested_bytes(nested_bytes: Vec<Vec<u8>>) -> Vec<u8> {
        let mut packed_data = Vec::new();

        // Store the number of elements (2 bytes)
        packed_data.extend_from_slice(&(nested_bytes.len() as u16).to_be_bytes());

        for bytes in nested_bytes {
            // Store length as 4 bytes (big-endian)
            packed_data.extend_from_slice(&(bytes.len() as u32).to_be_bytes());

            // Append the actual byte sequence
            packed_data.extend(bytes);
        }

        packed_data
    }

    /// Unpacks a byte vector into a vector of byte vectors (`Vec<Vec<u8>>`).
    ///
    /// This function reverses the encoding performed by `pack_nested_bytes`,
    /// extracting the original nested structure from the packed byte
    /// representation.
    ///
    /// # Encoding Format:
    /// - The first two bytes represents the number of nested byte arrays.
    /// - Each nested array is prefixed with a **2-byte (u16) length** in
    ///   big-endian format.
    /// - The byte sequence of each nested array follows.
    ///
    /// # Arguments
    ///
    /// * `packed_data` - A byte slice (`&[u8]`) that represents the packed
    ///   nested byte arrays.
    ///
    /// # Returns
    ///
    /// * `Ok(Vec<Vec<u8>>)` - The successfully unpacked byte arrays.
    /// * `Err(String)` - An error message if the input data is malformed.
    ///
    /// # Errors
    ///
    /// This function returns an error if:
    /// - The input is empty.
    /// - The number of expected chunks does not match the actual data length.
    /// - The data is truncated or malformed.
    pub fn unpack_nested_bytes(packed_data: &[u8]) -> Result<Vec<Vec<u8>>, Error> {
        if packed_data.is_empty() {
            return Err(Error::CorruptedData("Input data is empty".to_string()));
        }

        // Read num_elements as u16 (big-endian)
        let num_elements = u16::from_be_bytes([packed_data[0], packed_data[1]]) as usize;
        let mut nested_bytes = Vec::with_capacity(num_elements);
        let mut index = 2;

        for i in 0..num_elements {
            // Ensure there is enough data to read the 2-byte length
            if index + 1 >= packed_data.len() {
                return Err(Error::CorruptedData(format!(
                    "Unexpected end of data while reading length of nested array {}",
                    i
                )));
            }

            // Read length as u32 (big-endian)
            let byte_length = u32::from_be_bytes([
                packed_data[index],
                packed_data[index + 1],
                packed_data[index + 2],
                packed_data[index + 3],
            ]) as usize;
            index += 4; // Move past the length bytes

            // Ensure there's enough data for the byte sequence
            if index + byte_length > packed_data.len() {
                return Err(Error::CorruptedData(format!(
                    "Unexpected end of data while reading nested array {} (expected length: {})",
                    i, byte_length
                )));
            }

            // Extract the byte sequence
            let byte_sequence = packed_data[index..index + byte_length].to_vec();
            index += byte_length;

            // Push into the result
            nested_bytes.push(byte_sequence);
        }

        // Ensure no extra unexpected data remains
        if index != packed_data.len() {
            return Err(Error::CorruptedData(format!(
                "Extra unexpected bytes found at the end of input (expected length: {}, actual: \
                 {})",
                index,
                packed_data.len()
            )));
        }

        Ok(nested_bytes)
    }
}
