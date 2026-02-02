//! Path branch chunk query for retrieving chunked proofs of tree branches.
//!
//! A branch chunk query navigates to a specific key in a tree at a given path,
//! then returns the subtree rooted at that key up to a specified depth.
//! This is useful for retrieving subsequent chunks after a trunk query.

use bincode::{Decode, Encode};

/// Path branch chunk query
///
/// Represents a path to a specific GroveDB tree and parameters for retrieving
/// a branch chunk proof from that tree.
///
/// # Usage
/// After performing a trunk query, use the terminal node keys from the
/// `TrunkQueryResult` as the `key` parameter to retrieve deeper branches.
#[cfg(any(feature = "minimal", feature = "verify"))]
#[derive(Debug, Clone, PartialEq, Encode, Decode)]
pub struct PathBranchChunkQuery {
    /// Path to the tree to query
    pub path: Vec<Vec<u8>>,
    /// Key to navigate to before extracting the branch
    pub key: Vec<u8>,
    /// Depth of the branch to return from the key
    /// This is the max depth we want to get.
    /// This is not the depth where the key is.
    pub depth: u8,
}

#[cfg(any(feature = "minimal", feature = "verify"))]
impl PathBranchChunkQuery {
    /// Create a new path branch chunk query
    ///
    /// # Arguments
    /// * `path` - Path to the tree to query
    /// * `key` - Key to navigate to in the tree
    /// * `depth` - Depth of branch to return from the key (minimum 1)
    pub fn new(path: Vec<Vec<u8>>, key: Vec<u8>, depth: u8) -> Self {
        Self {
            path,
            key,
            depth: depth.max(1),
        }
    }

    /// Create a new path branch chunk query from slice path
    pub fn new_from_slice_path(path: &[&[u8]], key: &[u8], depth: u8) -> Self {
        Self::new(
            path.iter().map(|p| p.to_vec()).collect(),
            key.to_vec(),
            depth,
        )
    }

    /// Get the path as a slice of slices
    pub fn path_slices(&self) -> Vec<&[u8]> {
        self.path.iter().map(|p| p.as_slice()).collect()
    }
}
