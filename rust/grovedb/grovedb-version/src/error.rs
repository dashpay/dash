use thiserror::Error;
use versioned_feature_core::FeatureVersion;

#[derive(Error, Debug)]
pub enum GroveVersionError {
    /// Expected some specific versions
    #[error("grove unknown version on {method}, received: {received}")]
    UnknownVersionMismatch {
        /// method
        method: String,
        /// the allowed versions for this method
        known_versions: Vec<FeatureVersion>,
        /// requested core height
        received: FeatureVersion,
    },

    /// Expected some specific versions
    #[error("{method} not active for grove version")]
    VersionNotActive {
        /// method
        method: String,
        /// the allowed versions for this method
        known_versions: Vec<FeatureVersion>,
    },
}
