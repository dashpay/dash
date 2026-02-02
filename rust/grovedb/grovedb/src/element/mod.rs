//! Module for subtrees handling.
//! Subtrees handling is isolated so basically this module is about adapting
//! Merk API to GroveDB needs.

#[cfg(feature = "minimal")]
pub mod elements_iterator;
#[cfg(feature = "minimal")]
mod path_query_push_args;
#[cfg(feature = "minimal")]
pub mod query;
pub mod query_options;

pub use grovedb_element::*;
