/// Storage flag errors
#[derive(Debug, thiserror::Error)]
pub enum StorageFlagsError {
    /// Error
    #[error("deserialize unknown storage flags type error: {0}")]
    DeserializeUnknownStorageFlagsType(String),
    /// Error
    #[error("storage flags wrong size error: {0}")]
    StorageFlagsWrongSize(String),
    /// Error
    #[error("removing at epoch with no associated storage error: {0}")]
    RemovingAtEpochWithNoAssociatedStorage(String),
    /// Error
    #[error("storage flags overflow error: {0}")]
    StorageFlagsOverflow(String),
    /// Error
    #[error("removing flags error: {0}")]
    RemovingFlagsError(String),
    /// Error
    #[error("merging storage flags from different owners error: {0}")]
    MergingStorageFlagsFromDifferentOwners(String),
    /// Error
    #[error("merging storage flags with different base epoch: {0}")]
    MergingStorageFlagsWithDifferentBaseEpoch(String),
}

impl StorageFlagsError {
    /// Gets a mutable reference to the inner string of the error variant
    pub(crate) fn get_mut_info(&mut self) -> &mut String {
        match self {
            StorageFlagsError::DeserializeUnknownStorageFlagsType(ref mut msg)
            | StorageFlagsError::StorageFlagsWrongSize(ref mut msg)
            | StorageFlagsError::RemovingAtEpochWithNoAssociatedStorage(ref mut msg)
            | StorageFlagsError::StorageFlagsOverflow(ref mut msg)
            | StorageFlagsError::RemovingFlagsError(ref mut msg)
            | StorageFlagsError::MergingStorageFlagsFromDifferentOwners(ref mut msg)
            | StorageFlagsError::MergingStorageFlagsWithDifferentBaseEpoch(ref mut msg) => msg,
        }
    }

    /// adds info to the storage flags error
    pub(crate) fn add_info(&mut self, info: &str) {
        self.get_mut_info().push_str(format!(": {}", info).as_str());
    }
}
