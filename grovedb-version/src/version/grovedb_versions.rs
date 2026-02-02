use versioned_feature_core::FeatureVersion;

#[derive(Clone, Debug, Default)]
pub struct GroveDBVersions {
    pub apply_batch: GroveDBApplyBatchVersions,
    pub element: GroveDBElementMethodVersions,
    pub operations: GroveDBOperationsVersions,
    pub path_query_methods: GroveDBPathQueryMethodVersions,
    pub replication: GroveDBReplicationVersions,
}

#[derive(Clone, Debug, Default)]
pub struct GroveDBPathQueryMethodVersions {
    pub terminal_keys: FeatureVersion,
    pub merge: FeatureVersion,
    pub query_items_at_path: FeatureVersion,
    pub should_add_parent_tree_at_path: FeatureVersion,
}

#[derive(Clone, Debug, Default)]
pub struct GroveDBApplyBatchVersions {
    pub apply_batch_structure: FeatureVersion,
    pub apply_body: FeatureVersion,
    pub continue_partial_apply_body: FeatureVersion,
    pub apply_operations_without_batching: FeatureVersion,
    pub apply_batch: FeatureVersion,
    pub apply_partial_batch: FeatureVersion,
    pub open_batch_transactional_merk_at_path: FeatureVersion,
    pub open_batch_merk_at_path: FeatureVersion,
    pub apply_batch_with_element_flags_update: FeatureVersion,
    pub apply_partial_batch_with_element_flags_update: FeatureVersion,
    pub estimated_case_operations_for_batch: FeatureVersion,
}

#[derive(Clone, Debug, Default)]
pub struct GroveDBOperationsVersions {
    pub get: GroveDBOperationsGetVersions,
    pub insert: GroveDBOperationsInsertVersions,
    pub delete: GroveDBOperationsDeleteVersions,
    pub delete_up_tree: GroveDBOperationsDeleteUpTreeVersions,
    pub query: GroveDBOperationsQueryVersions,
    pub proof: GroveDBOperationsProofVersions,
    pub average_case: GroveDBOperationsAverageCaseVersions,
    pub worst_case: GroveDBOperationsWorstCaseVersions,
}

#[derive(Clone, Debug, Default)]
pub struct GroveDBOperationsGetVersions {
    pub get: FeatureVersion,
    pub get_caching_optional: FeatureVersion,
    pub follow_reference: FeatureVersion,
    pub follow_reference_once: FeatureVersion,
    pub get_raw: FeatureVersion,
    pub get_raw_caching_optional: FeatureVersion,
    pub get_raw_optional: FeatureVersion,
    pub get_raw_optional_caching_optional: FeatureVersion,
    pub has_raw: FeatureVersion,
    pub check_subtree_exists_invalid_path: FeatureVersion,
    pub average_case_for_has_raw: FeatureVersion,
    pub average_case_for_has_raw_tree: FeatureVersion,
    pub average_case_for_get_raw: FeatureVersion,
    pub average_case_for_get: FeatureVersion,
    pub average_case_for_get_tree: FeatureVersion,
    pub worst_case_for_has_raw: FeatureVersion,
    pub worst_case_for_get_raw: FeatureVersion,
    pub worst_case_for_get: FeatureVersion,
    pub is_empty_tree: FeatureVersion,
}

#[derive(Clone, Debug, Default)]
pub struct GroveDBOperationsProofVersions {
    pub prove_query: FeatureVersion,
    pub prove_query_many: FeatureVersion,
    pub verify_query_with_options: FeatureVersion,
    pub verify_query_raw: FeatureVersion,
    pub verify_layer_proof: FeatureVersion,
    pub verify_query: FeatureVersion,
    pub verify_subset_query: FeatureVersion,
    pub verify_query_with_absence_proof: FeatureVersion,
    pub verify_subset_query_with_absence_proof: FeatureVersion,
    pub verify_query_with_chained_path_queries: FeatureVersion,
    pub verify_query_get_parent_tree_info_with_options: FeatureVersion,
}

#[derive(Clone, Debug, Default)]
pub struct GroveDBOperationsQueryVersions {
    pub query_encoded_many: FeatureVersion,
    pub query_many_raw: FeatureVersion,
    pub get_proved_path_query: FeatureVersion,
    pub query: FeatureVersion,
    pub query_item_value: FeatureVersion,
    pub query_item_value_or_sum: FeatureVersion,
    pub query_sums: FeatureVersion,
    pub query_raw: FeatureVersion,
    pub query_keys_optional: FeatureVersion,
    pub query_raw_keys_optional: FeatureVersion,
    pub follow_element: FeatureVersion,
}

#[derive(Clone, Debug, Default)]
pub struct GroveDBOperationsAverageCaseVersions {
    pub add_average_case_get_merk_at_path: FeatureVersion,
    pub average_case_merk_replace_tree: FeatureVersion,
    pub average_case_merk_insert_tree: FeatureVersion,
    pub average_case_merk_delete_tree: FeatureVersion,
    pub average_case_merk_insert_element: FeatureVersion,
    pub average_case_merk_replace_element: FeatureVersion,
    pub average_case_merk_patch_element: FeatureVersion,
    pub average_case_merk_delete_element: FeatureVersion,
    pub add_average_case_has_raw_cost: FeatureVersion,
    pub add_average_case_has_raw_tree_cost: FeatureVersion,
    pub add_average_case_get_raw_cost: FeatureVersion,
    pub add_average_case_get_raw_tree_cost: FeatureVersion,
    pub add_average_case_get_cost: FeatureVersion,
}

#[derive(Clone, Debug, Default)]
pub struct GroveDBOperationsWorstCaseVersions {
    pub add_worst_case_get_merk_at_path: FeatureVersion,
    pub worst_case_merk_replace_tree: FeatureVersion,
    pub worst_case_merk_insert_tree: FeatureVersion,
    pub worst_case_merk_delete_tree: FeatureVersion,
    pub worst_case_merk_insert_element: FeatureVersion,
    pub worst_case_merk_replace_element: FeatureVersion,
    pub worst_case_merk_patch_element: FeatureVersion,
    pub worst_case_merk_delete_element: FeatureVersion,
    pub add_worst_case_has_raw_cost: FeatureVersion,
    pub add_worst_case_get_raw_tree_cost: FeatureVersion,
    pub add_worst_case_get_raw_cost: FeatureVersion,
    pub add_worst_case_get_cost: FeatureVersion,
}

#[derive(Clone, Debug, Default)]
pub struct GroveDBOperationsInsertVersions {
    pub insert: FeatureVersion,
    pub insert_on_transaction: FeatureVersion,
    pub insert_without_transaction: FeatureVersion,
    pub add_element_on_transaction: FeatureVersion,
    pub add_element_without_transaction: FeatureVersion,
    pub insert_if_not_exists: FeatureVersion,
    pub insert_if_not_exists_return_existing_element: FeatureVersion,
    pub insert_if_changed_value: FeatureVersion,
}

#[derive(Clone, Debug, Default)]
pub struct GroveDBOperationsDeleteVersions {
    pub delete: FeatureVersion,
    pub clear_subtree: FeatureVersion,
    pub delete_with_sectional_storage_function: FeatureVersion,
    pub delete_if_empty_tree: FeatureVersion,
    pub delete_if_empty_tree_with_sectional_storage_function: FeatureVersion,
    pub delete_operation_for_delete_internal: FeatureVersion,
    pub delete_internal_on_transaction: FeatureVersion,
    pub delete_internal_without_transaction: FeatureVersion,
    pub average_case_delete_operation_for_delete: FeatureVersion,
    pub worst_case_delete_operation_for_delete: FeatureVersion,
}

#[derive(Clone, Debug, Default)]
pub struct GroveDBOperationsDeleteUpTreeVersions {
    pub delete_up_tree_while_empty: FeatureVersion,
    pub delete_up_tree_while_empty_with_sectional_storage: FeatureVersion,
    pub delete_operations_for_delete_up_tree_while_empty: FeatureVersion,
    pub add_delete_operations_for_delete_up_tree_while_empty: FeatureVersion,
    pub average_case_delete_operations_for_delete_up_tree_while_empty: FeatureVersion,
    pub worst_case_delete_operations_for_delete_up_tree_while_empty: FeatureVersion,
}

#[derive(Clone, Debug, Default)]
pub struct GroveDBOperationsApplyBatchVersions {
    pub apply_batch_structure: FeatureVersion,
    pub apply_body: FeatureVersion,
    pub continue_partial_apply_body: FeatureVersion,
    pub apply_operations_without_batching: FeatureVersion,
    pub apply_batch: FeatureVersion,
    pub apply_partial_batch: FeatureVersion,
    pub open_batch_transactional_merk_at_path: FeatureVersion,
    pub open_batch_merk_at_path: FeatureVersion,
    pub apply_batch_with_element_flags_update: FeatureVersion,
    pub apply_partial_batch_with_element_flags_update: FeatureVersion,
    pub estimated_case_operations_for_batch: FeatureVersion,
}

#[derive(Clone, Debug, Default)]
pub struct GroveDBElementMethodVersions {
    pub delete: FeatureVersion,
    pub delete_with_sectioned_removal_bytes: FeatureVersion,
    pub delete_into_batch_operations: FeatureVersion,
    pub element_at_key_already_exists: FeatureVersion,
    pub get: FeatureVersion,
    pub get_optional: FeatureVersion,
    pub get_from_storage: FeatureVersion,
    pub get_optional_from_storage: FeatureVersion,
    pub get_with_absolute_refs: FeatureVersion,
    pub get_value_hash: FeatureVersion,
    pub get_with_value_hash: FeatureVersion,
    pub get_specialized_cost: FeatureVersion,
    pub value_defined_cost: FeatureVersion,
    pub value_defined_cost_for_serialized_value: FeatureVersion,
    pub specialized_costs_for_key_value: FeatureVersion,
    pub required_item_space: FeatureVersion,
    pub insert: FeatureVersion,
    pub insert_into_batch_operations: FeatureVersion,
    pub insert_if_not_exists: FeatureVersion,
    pub insert_if_not_exists_into_batch_operations: FeatureVersion,
    pub insert_if_changed_value: FeatureVersion,
    pub insert_if_changed_value_into_batch_operations: FeatureVersion,
    pub insert_reference: FeatureVersion,
    pub insert_reference_into_batch_operations: FeatureVersion,
    pub insert_reference_if_changed_value: FeatureVersion,
    pub insert_subtree: FeatureVersion,
    pub insert_subtree_into_batch_operations: FeatureVersion,
    pub get_query: FeatureVersion,
    pub get_query_values: FeatureVersion,
    pub get_query_apply_function: FeatureVersion,
    pub get_path_query: FeatureVersion,
    pub get_sized_query: FeatureVersion,
    pub path_query_push: FeatureVersion,
    pub query_item: FeatureVersion,
    pub basic_push: FeatureVersion,
    pub serialize: FeatureVersion,
    pub serialized_size: FeatureVersion,
    pub deserialize: FeatureVersion,
}

#[derive(Clone, Debug, Default)]
pub struct GroveDBReplicationVersions {
    pub get_subtrees_metadata: FeatureVersion,
    pub fetch_chunk: FeatureVersion,
    pub start_snapshot_syncing: FeatureVersion,
    pub apply_chunk: FeatureVersion,
}
