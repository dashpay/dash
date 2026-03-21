//! Batch running mode

#[cfg(feature = "estimated_costs")]
use std::collections::HashMap;

#[cfg(feature = "estimated_costs")]
use grovedb_merk::estimated_costs::{
    average_case_costs::EstimatedLayerInformation, worst_case_costs::WorstCaseLayerInformation,
};

#[cfg(feature = "estimated_costs")]
use crate::batch::KeyInfoPath;

#[cfg(feature = "minimal")]
/// Batch Running Mode
#[derive(Clone, PartialEq, Eq)]
pub enum BatchRunMode {
    Execute,
    #[cfg(feature = "estimated_costs")]
    AverageCase(HashMap<KeyInfoPath, EstimatedLayerInformation>),
    #[cfg(feature = "estimated_costs")]
    WorstCase(HashMap<KeyInfoPath, WorstCaseLayerInformation>),
}
