//! GroveDB Errors

use std::convert::Infallible;

use grovedb_costs::CostResult;

use crate::PathQuery;

/// GroveDB Errors
#[cfg(any(feature = "minimal", feature = "verify"))]
#[derive(Debug, thiserror::Error)]
pub enum Error {
    #[error("infallible")]
    /// This error can not happen, used for generics
    Infallible,
    // Input data errors
    #[error("cyclic reference path")]
    /// Cyclic reference
    CyclicReference,
    #[error("reference hops limit exceeded")]
    /// Reference limit
    ReferenceLimit,
    #[error("missing reference {0}")]
    /// Missing reference
    MissingReference(String),
    #[error("internal error: {0}")]
    /// Internal error
    InternalError(String),
    #[error("invalid proof: {1}")]
    /// Invalid proof
    InvalidProof(PathQuery, String),
    #[error("invalid input: {0}")]
    /// Invalid input
    InvalidInput(&'static str),

    // Path errors
    /// The path key not found could represent a valid query, just where the
    /// path key isn't there
    #[error("path key not found: {0}")]
    PathKeyNotFound(String),
    /// The path not found could represent a valid query, just where the path
    /// isn't there
    #[error("path not found: {0}")]
    PathNotFound(String),
    /// The path not found could represent a valid query, just where the parent
    /// path merk isn't there
    #[error("path parent layer not found: {0}")]
    PathParentLayerNotFound(String),

    /// The path's item by key referenced was not found
    #[error("corrupted referenced path key not found: {0}")]
    CorruptedReferencePathKeyNotFound(String),
    /// The path referenced was not found
    #[error("corrupted referenced path not found: {0}")]
    CorruptedReferencePathNotFound(String),
    /// The path's parent merk wasn't found
    #[error("corrupted referenced path key not found: {0}")]
    CorruptedReferencePathParentLayerNotFound(String),

    /// The invalid parent layer path represents a logical error from the client
    /// library
    #[error("invalid parent layer path: {0}")]
    InvalidParentLayerPath(String),
    /// The invalid path represents a logical error from the client library
    #[error("invalid path: {0}")]
    InvalidPath(String),
    /// The corrupted path represents a consistency error in internal groveDB
    /// logic
    #[error("corrupted path: {0}")]
    CorruptedPath(String),

    // Query errors
    #[error("invalid query: {0}")]
    /// Invalid query
    InvalidQuery(&'static str),
    #[error("missing parameter: {0}")]
    /// Missing parameter
    MissingParameter(&'static str),
    #[error("invalid parameter: {0}")]
    /// Invalid parameter
    InvalidParameter(&'static str),

    #[cfg(feature = "minimal")]
    // Irrecoverable errors
    #[error("storage_cost error: {0}")]
    /// Storage error
    StorageError(#[from] grovedb_storage::error::Error),

    #[error("data corruption error: {0}")]
    /// Corrupted data
    CorruptedData(String),

    #[error("data storage error: {0}")]
    /// Corrupted storage
    CorruptedStorage(String),

    #[error("invalid code execution error: {0}")]
    /// Invalid code execution
    InvalidCodeExecution(&'static str),
    #[error("corrupted code execution error: {0}")]
    /// Corrupted code execution
    CorruptedCodeExecution(&'static str),

    #[error("invalid batch operation error: {0}")]
    /// Invalid batch operation
    InvalidBatchOperation(&'static str),

    #[error("delete up tree stop height more than initial path size error: {0}")]
    /// Delete up tree stop height more than initial path size
    DeleteUpTreeStopHeightMoreThanInitialPathSize(String),

    #[error("deleting non empty tree error: {0}")]
    /// Deleting non empty tree
    DeletingNonEmptyTree(&'static str),

    #[error("clearing tree with subtrees not allowed error: {0}")]
    /// Clearing tree with subtrees not allowed
    ClearingTreeWithSubtreesNotAllowed(&'static str),

    // Client allowed errors
    #[error("just in time element flags client error: {0}")]
    /// Just in time element flags client error
    JustInTimeElementFlagsClientError(String),

    #[error("split removal bytes client error: {0}")]
    /// Split removal bytes client error
    SplitRemovalBytesClientError(String),

    #[error("client returned non client error: {0}")]
    /// Client returned non client error
    ClientReturnedNonClientError(String),

    #[error("override not allowed error: {0}")]
    /// Override not allowed
    OverrideNotAllowed(&'static str),

    #[error("path not found in cache for estimated costs: {0}")]
    /// Path not found in cache for estimated costs
    PathNotFoundInCacheForEstimatedCosts(String),

    // Support errors
    #[error("not supported: {0}")]
    /// Not supported
    NotSupported(String),

    // Merk errors
    #[error("merk error: {0}")]
    /// Merk error
    MerkError(grovedb_merk::error::Error),

    // Version errors
    #[error(transparent)]
    /// Version error
    VersionError(grovedb_version::error::GroveVersionError),

    // Element errors
    #[error(transparent)]
    /// Element error
    ElementError(grovedb_element::error::ElementError),

    #[error("cyclic error")]
    /// Cyclic reference
    CyclicError(&'static str),
}

impl Error {
    pub fn add_context(&mut self, append: impl AsRef<str>) {
        match self {
            Self::MissingReference(s)
            | Self::InternalError(s)
            | Self::InvalidProof(_, s)
            | Self::PathKeyNotFound(s)
            | Self::PathNotFound(s)
            | Self::PathParentLayerNotFound(s)
            | Self::CorruptedReferencePathKeyNotFound(s)
            | Self::CorruptedReferencePathNotFound(s)
            | Self::CorruptedReferencePathParentLayerNotFound(s)
            | Self::InvalidParentLayerPath(s)
            | Self::InvalidPath(s)
            | Self::CorruptedPath(s)
            | Self::CorruptedData(s)
            | Self::CorruptedStorage(s)
            | Self::DeleteUpTreeStopHeightMoreThanInitialPathSize(s)
            | Self::JustInTimeElementFlagsClientError(s)
            | Self::SplitRemovalBytesClientError(s)
            | Self::ClientReturnedNonClientError(s)
            | Self::PathNotFoundInCacheForEstimatedCosts(s)
            | Self::NotSupported(s) => {
                s.push_str(", ");
                s.push_str(append.as_ref());
            }
            _ => {}
        }
    }
}

pub trait GroveDbErrorExt {
    fn add_context(self, append: impl AsRef<str>) -> Self;
}

impl<T> GroveDbErrorExt for CostResult<T, Error> {
    fn add_context(self, append: impl AsRef<str>) -> Self {
        self.map_err(|mut e| {
            e.add_context(append.as_ref());
            e
        })
    }
}

impl From<Infallible> for Error {
    fn from(_value: Infallible) -> Self {
        Self::Infallible
    }
}

impl From<grovedb_merk::error::Error> for Error {
    fn from(value: grovedb_merk::Error) -> Self {
        match value {
            grovedb_merk::Error::PathKeyNotFound(e) => Self::PathKeyNotFound(e),
            grovedb_merk::Error::PathNotFound(e) => Self::PathNotFound(e),
            grovedb_merk::Error::PathParentLayerNotFound(e) => Self::PathParentLayerNotFound(e),
            grovedb_merk::Error::ElementError(e) => Self::ElementError(e),
            grovedb_merk::Error::InvalidInputError(e) => Self::InvalidInput(e),
            _ => Self::MerkError(value),
        }
    }
}

impl From<grovedb_version::error::GroveVersionError> for Error {
    fn from(value: grovedb_version::error::GroveVersionError) -> Self {
        Error::VersionError(value)
    }
}

impl From<grovedb_element::error::ElementError> for Error {
    fn from(value: grovedb_element::error::ElementError) -> Self {
        Error::ElementError(value)
    }
}
