#[derive(Debug, thiserror::Error)]
pub enum ElementError {
    #[error("wrong element type: {0}")]
    /// Invalid element type
    WrongElementType(&'static str),

    #[error("data corruption error: {0}")]
    /// Corrupted data
    CorruptedData(String),

    #[error("invalid input: {0}")]
    /// Invalid input
    InvalidInput(&'static str),

    /// The corrupted path represents a consistency error in internal groveDB
    /// logic
    #[error("corrupted path: {0}")]
    CorruptedPath(String),

    // Version errors
    #[error(transparent)]
    /// Version error
    VersionError(grovedb_version::error::GroveVersionError),
}
impl From<grovedb_version::error::GroveVersionError> for ElementError {
    fn from(value: grovedb_version::error::GroveVersionError) -> Self {
        ElementError::VersionError(value)
    }
}
