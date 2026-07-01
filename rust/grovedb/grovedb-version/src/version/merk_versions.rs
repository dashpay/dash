use versioned_feature_core::FeatureVersion;

#[derive(Clone, Debug, Default)]
pub struct MerkVersions {
    pub average_case_costs: MerkAverageCaseCostsVersions,
}

#[derive(Clone, Debug, Default)]
pub struct MerkAverageCaseCostsVersions {
    pub add_average_case_merk_propagate: FeatureVersion,
    pub sum_tree_estimated_size: FeatureVersion,
}
