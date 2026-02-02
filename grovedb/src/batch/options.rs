//! Options

#[cfg(feature = "minimal")]
use grovedb_merk::MerkOptions;

#[cfg(feature = "minimal")]
use crate::operations::{delete::DeleteOptions, insert::InsertOptions};

/// Batch apply options
#[cfg(feature = "minimal")]
#[derive(Debug, Clone)]
pub struct BatchApplyOptions {
    /// Validate insertion does not override
    pub validate_insertion_does_not_override: bool,
    /// Validate insertion does not override tree
    pub validate_insertion_does_not_override_tree: bool,
    /// Allow deleting non-empty trees
    pub allow_deleting_non_empty_trees: bool,
    /// Deleting non empty trees returns error
    pub deleting_non_empty_trees_returns_error: bool,
    /// Disable operation consistency check
    pub disable_operation_consistency_check: bool,
    /// Base root storage is free
    pub base_root_storage_is_free: bool,
    /// At what height do we want to pause applying batch operations
    /// Most of the time this should be not set
    pub batch_pause_height: Option<u8>,
}

#[cfg(feature = "minimal")]
impl Default for BatchApplyOptions {
    fn default() -> Self {
        BatchApplyOptions {
            validate_insertion_does_not_override: false,
            validate_insertion_does_not_override_tree: false,
            allow_deleting_non_empty_trees: false,
            deleting_non_empty_trees_returns_error: true,
            disable_operation_consistency_check: false,
            base_root_storage_is_free: true,
            batch_pause_height: None,
        }
    }
}

#[cfg(feature = "minimal")]
impl BatchApplyOptions {
    /// As insert options
    pub(crate) fn as_insert_options(&self) -> InsertOptions {
        InsertOptions {
            validate_insertion_does_not_override: self.validate_insertion_does_not_override,
            validate_insertion_does_not_override_tree: self
                .validate_insertion_does_not_override_tree,
            base_root_storage_is_free: self.base_root_storage_is_free,
        }
    }

    /// As delete options
    pub(crate) fn as_delete_options(&self) -> DeleteOptions where {
        DeleteOptions {
            allow_deleting_non_empty_trees: self.allow_deleting_non_empty_trees,
            deleting_non_empty_trees_returns_error: self.deleting_non_empty_trees_returns_error,
            base_root_storage_is_free: self.base_root_storage_is_free,
            validate_tree_at_path_exists: false,
        }
    }

    /// As Merk options
    pub(crate) fn as_merk_options(&self) -> MerkOptions {
        MerkOptions {
            base_root_storage_is_free: self.base_root_storage_is_free,
        }
    }
}
