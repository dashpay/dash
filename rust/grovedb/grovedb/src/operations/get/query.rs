//! Query operations

use grovedb_costs::cost_return_on_error_default;
#[cfg(feature = "minimal")]
use grovedb_costs::{
    cost_return_on_error, cost_return_on_error_no_add, CostResult, CostsExt, OperationCost,
};
use grovedb_version::{check_grovedb_v0, check_grovedb_v0_with_cost, version::GroveVersion};
#[cfg(feature = "minimal")]
use integer_encoding::VarInt;

#[cfg(feature = "minimal")]
use crate::element::SumValue;
use crate::{
    element::{
        query::ElementQueryExtensions, query_options::QueryOptions, BigSumValue, CountValue,
    },
    operations::proof::ProveOptions,
    query_result_type::PathKeyOptionalElementTrio,
};
#[cfg(feature = "minimal")]
use crate::{
    query_result_type::{QueryResultElement, QueryResultElements, QueryResultType},
    reference_path::ReferencePathType,
    Element, Error, GroveDb, PathQuery, TransactionArg,
};

#[cfg(feature = "minimal")]
#[derive(Debug, Eq, PartialEq, Clone)]
/// A return type for query_item_value_or_sum
pub enum QueryItemOrSumReturnType {
    /// an Item in serialized form
    ItemData(Vec<u8>),
    /// A sum item or a sum tree value
    SumValue(SumValue),
    /// A big sum tree value
    BigSumValue(BigSumValue),
    /// A count value
    CountValue(CountValue),
    /// A count and sum value
    CountSumValue(CountValue, SumValue),
    /// an Item in serialized form with a Sum Value
    ItemDataWithSumValue(Vec<u8>, SumValue),
}

#[cfg(feature = "minimal")]
impl GroveDb {
    /// Encoded query for multiple path queries
    #[deprecated]
    pub fn query_encoded_many(
        &self,
        path_queries: &[&PathQuery],
        allow_cache: bool,
        decrease_limit_on_range_with_no_sub_elements: bool,
        error_if_intermediate_path_tree_not_present: bool,
        transaction: TransactionArg,
        grove_version: &GroveVersion,
    ) -> CostResult<Vec<Vec<u8>>, Error> {
        check_grovedb_v0_with_cost!(
            "query_encoded_many",
            grove_version
                .grovedb_versions
                .operations
                .query
                .query_encoded_many
        );

        let mut cost = OperationCost::default();

        let elements = cost_return_on_error!(
            &mut cost,
            self.query_many_raw(
                path_queries,
                allow_cache,
                decrease_limit_on_range_with_no_sub_elements,
                error_if_intermediate_path_tree_not_present,
                QueryResultType::QueryElementResultType,
                transaction,
                grove_version
            )
        );
        let results_wrapped = elements
            .into_iterator()
            .map(|result_item| match result_item {
                QueryResultElement::ElementResultItem(Element::Reference(reference_path, ..)) => {
                    match reference_path {
                        ReferencePathType::AbsolutePathReference(absolute_path) => {
                            // While `map` on iterator is lazy, we should accumulate costs even if
                            // `collect` will end in `Err`, so we'll use
                            // external costs accumulator instead of
                            // returning costs from `map` call.
                            let maybe_item = self
                                .follow_reference(
                                    absolute_path.as_slice().into(),
                                    allow_cache,
                                    transaction,
                                    grove_version,
                                )
                                .unwrap_add_cost(&mut cost)?;

                            match maybe_item {
                                Element::Item(item, _) => Ok(item),
                                Element::ItemWithSumItem(item, ..) => Ok(item),
                                Element::SumItem(value, _) => Ok(value.encode_var_vec()),
                                _ => {
                                    Err(Error::InvalidQuery("the reference must result in an item"))
                                }
                            }
                        }
                        _ => Err(Error::CorruptedCodeExecution(
                            "reference after query must have absolute paths",
                        )),
                    }
                }
                _ => Err(Error::InvalidQuery(
                    "path_queries can only refer to references",
                )),
            })
            .collect::<Result<Vec<Vec<u8>>, Error>>();

        results_wrapped.wrap_with_cost(cost)
    }

    /// Raw query for multiple path queries
    pub fn query_many_raw(
        &self,
        path_queries: &[&PathQuery],
        allow_cache: bool,
        decrease_limit_on_range_with_no_sub_elements: bool,
        error_if_intermediate_path_tree_not_present: bool,
        result_type: QueryResultType,
        transaction: TransactionArg,
        grove_version: &GroveVersion,
    ) -> CostResult<QueryResultElements, Error>
where {
        check_grovedb_v0_with_cost!(
            "query_many_raw",
            grove_version
                .grovedb_versions
                .operations
                .query
                .query_many_raw
        );
        let mut cost = OperationCost::default();

        let query = cost_return_on_error_no_add!(
            cost,
            PathQuery::merge(path_queries.to_vec(), grove_version)
        );
        let (result, _) = cost_return_on_error!(
            &mut cost,
            self.query_raw(
                &query,
                allow_cache,
                decrease_limit_on_range_with_no_sub_elements,
                error_if_intermediate_path_tree_not_present,
                result_type,
                transaction,
                grove_version
            )
        );
        Ok(result).wrap_with_cost(cost)
    }

    /// Prove a path query as either verbose or non-verbose
    pub fn get_proved_path_query(
        &self,
        path_query: &PathQuery,
        prove_options: Option<ProveOptions>,
        transaction: TransactionArg,
        grove_version: &GroveVersion,
    ) -> CostResult<Vec<u8>, Error> {
        check_grovedb_v0_with_cost!(
            "get_proved_path_query",
            grove_version
                .grovedb_versions
                .operations
                .query
                .get_proved_path_query
        );
        if transaction.is_some() {
            Err(Error::NotSupported(
                "transactions are not currently supported".to_string(),
            ))
            .wrap_with_cost(Default::default())
        } else {
            self.prove_query(path_query, prove_options, grove_version)
        }
    }

    fn follow_element(
        &self,
        element: Element,
        allow_cache: bool,
        cost: &mut OperationCost,
        transaction: TransactionArg,
        grove_version: &GroveVersion,
    ) -> Result<Element, Error> {
        check_grovedb_v0!(
            "follow_element",
            grove_version
                .grovedb_versions
                .operations
                .query
                .follow_element
        );
        match element {
            Element::Reference(reference_path, ..) => {
                match reference_path {
                    ReferencePathType::AbsolutePathReference(absolute_path) => {
                        // While `map` on iterator is lazy, we should accumulate costs
                        // even if `collect` will
                        // end in `Err`, so we'll use
                        // external costs accumulator instead of
                        // returning costs from `map` call.
                        let maybe_item = self
                            .follow_reference(
                                absolute_path.as_slice().into(),
                                allow_cache,
                                transaction,
                                grove_version,
                            )
                            .unwrap_add_cost(cost)?;

                        if maybe_item.is_any_item() {
                            Ok(maybe_item)
                        } else {
                            Err(Error::InvalidQuery("the reference must result in an item"))
                        }
                    }
                    _ => Err(Error::CorruptedCodeExecution(
                        "reference after query must have absolute paths",
                    )),
                }
            }
            Element::Item(..)
            | Element::SumItem(..)
            | Element::ItemWithSumItem(..)
            | Element::SumTree(..)
            | Element::BigSumTree(..)
            | Element::CountTree(..)
            | Element::CountSumTree(..)
            | Element::ProvableCountTree(..)
            | Element::ProvableCountSumTree(..) => Ok(element),
            Element::Tree(..) => Err(Error::InvalidQuery("path_queries can not refer to trees")),
        }
    }

    /// Returns the result set after applying a path query
    pub fn query(
        &self,
        path_query: &PathQuery,
        allow_cache: bool,
        decrease_limit_on_range_with_no_sub_elements: bool,
        error_if_intermediate_path_tree_not_present: bool,
        result_type: QueryResultType,
        transaction: TransactionArg,
        grove_version: &GroveVersion,
    ) -> CostResult<(QueryResultElements, u16), Error> {
        check_grovedb_v0_with_cost!(
            "query",
            grove_version.grovedb_versions.operations.query.query
        );
        let mut cost = OperationCost::default();

        let (elements, skipped) = cost_return_on_error!(
            &mut cost,
            self.query_raw(
                path_query,
                allow_cache,
                decrease_limit_on_range_with_no_sub_elements,
                error_if_intermediate_path_tree_not_present,
                result_type,
                transaction,
                grove_version
            )
        );

        let results_wrapped = elements
            .into_iterator()
            .map(|result_item| {
                result_item.map_element(|element| {
                    self.follow_element(element, allow_cache, &mut cost, transaction, grove_version)
                })
            })
            .collect::<Result<Vec<QueryResultElement>, Error>>();

        let results = cost_return_on_error_no_add!(cost, results_wrapped);
        Ok((QueryResultElements { elements: results }, skipped)).wrap_with_cost(cost)
    }

    /// Queries the backing store and returns element items by their value,
    /// Sum Items are encoded as var vec
    pub fn query_item_value(
        &self,
        path_query: &PathQuery,
        allow_cache: bool,
        decrease_limit_on_range_with_no_sub_elements: bool,
        error_if_intermediate_path_tree_not_present: bool,
        transaction: TransactionArg,
        grove_version: &GroveVersion,
    ) -> CostResult<(Vec<Vec<u8>>, u16), Error> {
        check_grovedb_v0_with_cost!(
            "query_item_value",
            grove_version
                .grovedb_versions
                .operations
                .query
                .query_item_value
        );
        let mut cost = OperationCost::default();

        let (elements, skipped) = cost_return_on_error!(
            &mut cost,
            self.query_raw(
                path_query,
                allow_cache,
                decrease_limit_on_range_with_no_sub_elements,
                error_if_intermediate_path_tree_not_present,
                QueryResultType::QueryElementResultType,
                transaction,
                grove_version
            )
        );

        let results_wrapped = elements
            .into_iterator()
            .map(|result_item| match result_item {
                QueryResultElement::ElementResultItem(element) => {
                    match element {
                        Element::Reference(reference_path, ..) => {
                            match reference_path {
                                ReferencePathType::AbsolutePathReference(absolute_path) => {
                                    // While `map` on iterator is lazy, we should accumulate costs
                                    // even if `collect` will
                                    // end in `Err`, so we'll use
                                    // external costs accumulator instead of
                                    // returning costs from `map` call.
                                    let maybe_item = self
                                        .follow_reference(
                                            absolute_path.as_slice().into(),
                                            allow_cache,
                                            transaction,
                                            grove_version,
                                        )
                                        .unwrap_add_cost(&mut cost)?;

                                    match maybe_item {
                                        Element::Item(item, _)
                                        | Element::ItemWithSumItem(item, ..) => Ok(item),
                                        Element::SumItem(item, _) => Ok(item.encode_var_vec()),
                                        _ => Err(Error::InvalidQuery(
                                            "the reference must result in an item",
                                        )),
                                    }
                                }
                                _ => Err(Error::CorruptedCodeExecution(
                                    "reference after query must have absolute paths",
                                )),
                            }
                        }
                        Element::Item(item, _) | Element::ItemWithSumItem(item, ..) => Ok(item),
                        Element::SumItem(item, _) => Ok(item.encode_var_vec()),
                        Element::Tree(..)
                        | Element::SumTree(..)
                        | Element::BigSumTree(..)
                        | Element::CountTree(..)
                        | Element::CountSumTree(..)
                        | Element::ProvableCountTree(..)
                        | Element::ProvableCountSumTree(..) => Err(Error::InvalidQuery(
                            "path_queries can only refer to items and references",
                        )),
                    }
                }
                _ => Err(Error::CorruptedCodeExecution(
                    "query returned incorrect result type",
                )),
            })
            .collect::<Result<Vec<Vec<u8>>, Error>>();

        let results = cost_return_on_error_no_add!(cost, results_wrapped);
        Ok((results, skipped)).wrap_with_cost(cost)
    }

    /// Queries the backing store and returns element items by their value,
    /// Sum Items are returned
    pub fn query_item_value_or_sum(
        &self,
        path_query: &PathQuery,
        allow_cache: bool,
        decrease_limit_on_range_with_no_sub_elements: bool,
        error_if_intermediate_path_tree_not_present: bool,
        transaction: TransactionArg,
        grove_version: &GroveVersion,
    ) -> CostResult<(Vec<QueryItemOrSumReturnType>, u16), Error> {
        check_grovedb_v0_with_cost!(
            "query_item_value_or_sum",
            grove_version
                .grovedb_versions
                .operations
                .query
                .query_item_value_or_sum
        );
        let mut cost = OperationCost::default();

        let (elements, skipped) = cost_return_on_error!(
            &mut cost,
            self.query_raw(
                path_query,
                allow_cache,
                decrease_limit_on_range_with_no_sub_elements,
                error_if_intermediate_path_tree_not_present,
                QueryResultType::QueryElementResultType,
                transaction,
                grove_version
            )
        );

        let results_wrapped = elements
            .into_iterator()
            .map(|result_item| match result_item {
                QueryResultElement::ElementResultItem(element) => {
                    match element {
                        Element::Reference(reference_path, ..) => {
                            match reference_path {
                                ReferencePathType::AbsolutePathReference(absolute_path) => {
                                    // While `map` on iterator is lazy, we should accumulate costs
                                    // even if `collect` will
                                    // end in `Err`, so we'll use
                                    // external costs accumulator instead of
                                    // returning costs from `map` call.
                                    let maybe_item = self
                                        .follow_reference(
                                            absolute_path.as_slice().into(),
                                            allow_cache,
                                            transaction,
                                            grove_version,
                                        )
                                        .unwrap_add_cost(&mut cost)?;

                                    match maybe_item {
                                        Element::Item(item, _) => {
                                            Ok(QueryItemOrSumReturnType::ItemData(item))
                                        }
                                        Element::SumItem(sum_value, _) => {
                                            Ok(QueryItemOrSumReturnType::SumValue(sum_value))
                                        }
                                        Element::ItemWithSumItem(item, sum_value, _) => {
                                            Ok(QueryItemOrSumReturnType::ItemDataWithSumValue(
                                                item, sum_value,
                                            ))
                                        }
                                        Element::SumTree(_, sum_value, _) => {
                                            Ok(QueryItemOrSumReturnType::SumValue(sum_value))
                                        }
                                        Element::BigSumTree(_, big_sum_value, _) => {
                                            Ok(QueryItemOrSumReturnType::BigSumValue(big_sum_value))
                                        }
                                        Element::CountTree(_, count_value, _) => {
                                            Ok(QueryItemOrSumReturnType::CountValue(count_value))
                                        }
                                        Element::CountSumTree(_, count_value, sum_value, _) => {
                                            Ok(QueryItemOrSumReturnType::CountSumValue(
                                                count_value,
                                                sum_value,
                                            ))
                                        }
                                        Element::ProvableCountTree(_, count_value, _) => {
                                            Ok(QueryItemOrSumReturnType::CountValue(count_value))
                                        }
                                        Element::ProvableCountSumTree(
                                            _,
                                            count_value,
                                            sum_value,
                                            _,
                                        ) => Ok(QueryItemOrSumReturnType::CountSumValue(
                                            count_value,
                                            sum_value,
                                        )),
                                        _ => Err(Error::InvalidQuery(
                                            "the reference must result in an item",
                                        )),
                                    }
                                }
                                _ => Err(Error::CorruptedCodeExecution(
                                    "reference after query must have absolute paths",
                                )),
                            }
                        }
                        Element::Item(item, _) => Ok(QueryItemOrSumReturnType::ItemData(item)),
                        Element::SumItem(sum_value, _) => {
                            Ok(QueryItemOrSumReturnType::SumValue(sum_value))
                        }
                        Element::ItemWithSumItem(item, sum_value, _) => Ok(
                            QueryItemOrSumReturnType::ItemDataWithSumValue(item, sum_value),
                        ),
                        Element::SumTree(_, sum_value, _) => {
                            Ok(QueryItemOrSumReturnType::SumValue(sum_value))
                        }
                        Element::BigSumTree(_, big_sum_value, _) => {
                            Ok(QueryItemOrSumReturnType::BigSumValue(big_sum_value))
                        }
                        Element::CountTree(_, count_value, _) => {
                            Ok(QueryItemOrSumReturnType::CountValue(count_value))
                        }
                        Element::CountSumTree(_, count_value, sum_value, _) => Ok(
                            QueryItemOrSumReturnType::CountSumValue(count_value, sum_value),
                        ),
                        Element::ProvableCountTree(_, count_value, _) => {
                            Ok(QueryItemOrSumReturnType::CountValue(count_value))
                        }
                        Element::ProvableCountSumTree(_, count_value, sum_value, _) => Ok(
                            QueryItemOrSumReturnType::CountSumValue(count_value, sum_value),
                        ),
                        Element::Tree(..) => Err(Error::InvalidQuery(
                            "path_queries can only refer to items, sum items, references and sum \
                             trees",
                        )),
                    }
                }
                _ => Err(Error::CorruptedCodeExecution(
                    "query returned incorrect result type",
                )),
            })
            .collect::<Result<Vec<QueryItemOrSumReturnType>, Error>>();

        let results = cost_return_on_error_no_add!(cost, results_wrapped);
        Ok((results, skipped)).wrap_with_cost(cost)
    }

    /// Retrieves only SumItem elements that match a path query
    pub fn query_sums(
        &self,
        path_query: &PathQuery,
        allow_cache: bool,
        decrease_limit_on_range_with_no_sub_elements: bool,
        error_if_intermediate_path_tree_not_present: bool,
        transaction: TransactionArg,
        grove_version: &GroveVersion,
    ) -> CostResult<(Vec<i64>, u16), Error> {
        check_grovedb_v0_with_cost!(
            "query_sums",
            grove_version.grovedb_versions.operations.query.query_sums
        );
        let mut cost = OperationCost::default();

        let (elements, skipped) = cost_return_on_error!(
            &mut cost,
            self.query_raw(
                path_query,
                allow_cache,
                decrease_limit_on_range_with_no_sub_elements,
                error_if_intermediate_path_tree_not_present,
                QueryResultType::QueryElementResultType,
                transaction,
                grove_version
            )
        );

        let results_wrapped = elements
            .into_iterator()
            .map(|result_item| match result_item {
                QueryResultElement::ElementResultItem(element) => {
                    match element {
                        Element::Reference(reference_path, ..) => {
                            match reference_path {
                                ReferencePathType::AbsolutePathReference(absolute_path) => {
                                    // While `map` on iterator is lazy, we should accumulate costs
                                    // even if `collect` will
                                    // end in `Err`, so we'll use
                                    // external costs accumulator instead of
                                    // returning costs from `map` call.
                                    let maybe_item = self
                                        .follow_reference(
                                            absolute_path.as_slice().into(),
                                            allow_cache,
                                            transaction,
                                            grove_version,
                                        )
                                        .unwrap_add_cost(&mut cost)?;

                                    if let Element::SumItem(item, _) = maybe_item {
                                        Ok(item)
                                    } else {
                                        Err(Error::InvalidQuery(
                                            "the reference must result in a sum item",
                                        ))
                                    }
                                }
                                _ => Err(Error::CorruptedCodeExecution(
                                    "reference after query must have absolute paths",
                                )),
                            }
                        }
                        Element::SumItem(item, _) | Element::ItemWithSumItem(_, item, _) => {
                            Ok(item)
                        }
                        Element::Tree(..)
                        | Element::SumTree(..)
                        | Element::BigSumTree(..)
                        | Element::CountTree(..)
                        | Element::CountSumTree(..)
                        | Element::ProvableCountTree(..)
                        | Element::ProvableCountSumTree(..)
                        | Element::Item(..) => Err(Error::InvalidQuery(
                            "path_queries over sum items can only refer to sum items and \
                             references",
                        )),
                    }
                }
                _ => Err(Error::CorruptedCodeExecution(
                    "query returned incorrect result type",
                )),
            })
            .collect::<Result<Vec<i64>, Error>>();

        let results = cost_return_on_error_no_add!(cost, results_wrapped);
        Ok((results, skipped)).wrap_with_cost(cost)
    }

    /// Returns result elements and number of elements skipped given path query
    #[allow(clippy::too_many_arguments)]
    pub fn query_raw(
        &self,
        path_query: &PathQuery,
        allow_cache: bool,
        decrease_limit_on_range_with_no_sub_elements: bool,
        error_if_intermediate_path_tree_not_present: bool,
        result_type: QueryResultType,
        transaction: TransactionArg,
        grove_version: &GroveVersion,
    ) -> CostResult<(QueryResultElements, u16), Error> {
        check_grovedb_v0_with_cost!(
            "query_raw",
            grove_version.grovedb_versions.operations.query.query_raw
        );
        Element::get_path_query(
            &self.db,
            path_query,
            QueryOptions {
                allow_get_raw: true,
                allow_cache,
                decrease_limit_on_range_with_no_sub_elements,
                error_if_intermediate_path_tree_not_present,
            },
            result_type,
            transaction,
            grove_version,
        )
    }

    /// Splits the result set of a path query by query path.
    /// If max_results is exceeded we return an error.
    pub fn query_keys_optional(
        &self,
        path_query: &PathQuery,
        allow_cache: bool,
        decrease_limit_on_range_with_no_sub_elements: bool,
        error_if_intermediate_path_tree_not_present: bool,
        transaction: TransactionArg,
        grove_version: &GroveVersion,
    ) -> CostResult<Vec<PathKeyOptionalElementTrio>, Error> {
        check_grovedb_v0_with_cost!(
            "query_keys_optional",
            grove_version
                .grovedb_versions
                .operations
                .query
                .query_keys_optional
        );
        let max_results = cost_return_on_error_default!(path_query.query.limit.ok_or(
            Error::NotSupported("limits must be set in query_keys_optional".to_string())
        )) as usize;
        if path_query.query.offset.is_some() {
            return Err(Error::NotSupported(
                "offsets are not supported in query_raw_keys_optional".to_string(),
            ))
            .wrap_with_cost(OperationCost::default());
        }
        let mut cost = OperationCost::default();

        let terminal_keys = cost_return_on_error_no_add!(
            cost,
            path_query.terminal_keys(max_results, grove_version)
        );

        let (elements, _) = cost_return_on_error!(
            &mut cost,
            self.query(
                path_query,
                allow_cache,
                decrease_limit_on_range_with_no_sub_elements,
                error_if_intermediate_path_tree_not_present,
                QueryResultType::QueryPathKeyElementTrioResultType,
                transaction,
                grove_version
            )
        );

        let mut elements_map = elements.to_path_key_elements_btree_map();

        Ok(terminal_keys
            .into_iter()
            .map(|path_key| {
                let element = elements_map.remove(&path_key);
                (path_key.0, path_key.1, element)
            })
            .collect())
        .wrap_with_cost(cost)
    }

    /// If max_results is exceeded we return an error
    pub fn query_raw_keys_optional(
        &self,
        path_query: &PathQuery,
        allow_cache: bool,
        decrease_limit_on_range_with_no_sub_elements: bool,
        error_if_intermediate_path_tree_not_present: bool,
        transaction: TransactionArg,
        grove_version: &GroveVersion,
    ) -> CostResult<Vec<PathKeyOptionalElementTrio>, Error> {
        check_grovedb_v0_with_cost!(
            "query_raw_keys_optional",
            grove_version
                .grovedb_versions
                .operations
                .query
                .query_raw_keys_optional
        );
        let max_results = cost_return_on_error_default!(path_query.query.limit.ok_or(
            Error::NotSupported("limits must be set in query_raw_keys_optional".to_string())
        )) as usize;
        if path_query.query.offset.is_some() {
            return Err(Error::NotSupported(
                "offsets are not supported in query_raw_keys_optional".to_string(),
            ))
            .wrap_with_cost(OperationCost::default());
        }
        let mut cost = OperationCost::default();

        let terminal_keys = cost_return_on_error_no_add!(
            cost,
            path_query.terminal_keys(max_results, grove_version)
        );

        let (elements, _) = cost_return_on_error!(
            &mut cost,
            self.query_raw(
                path_query,
                allow_cache,
                decrease_limit_on_range_with_no_sub_elements,
                error_if_intermediate_path_tree_not_present,
                QueryResultType::QueryPathKeyElementTrioResultType,
                transaction,
                grove_version
            )
        );

        let mut elements_map = elements.to_path_key_elements_btree_map();

        Ok(terminal_keys
            .into_iter()
            .map(|path_key| {
                let element = elements_map.remove(&path_key);
                (path_key.0, path_key.1, element)
            })
            .collect())
        .wrap_with_cost(cost)
    }
}

#[cfg(feature = "minimal")]
#[cfg(test)]
mod tests {
    use std::collections::HashMap;

    use grovedb_merk::proofs::{query::query_item::QueryItem, Query};
    use grovedb_version::version::GroveVersion;
    use pretty_assertions::assert_eq;

    use crate::{
        reference_path::ReferencePathType::AbsolutePathReference,
        tests::{make_test_grovedb, ANOTHER_TEST_LEAF, TEST_LEAF},
        Element, PathQuery, SizedQuery,
    };

    #[test]
    fn test_query_raw_keys_options() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        db.insert(
            [TEST_LEAF].as_ref(),
            b"1",
            Element::new_item(b"hello".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF].as_ref(),
            b"3",
            Element::new_item(b"hello too".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF].as_ref(),
            b"5",
            Element::new_item(b"bye".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");

        let mut query = Query::new();
        query.insert_key(b"1".to_vec());
        query.insert_key(b"2".to_vec());
        query.insert_key(b"5".to_vec());
        let path = vec![TEST_LEAF.to_vec()];
        let path_query = PathQuery::new(path.clone(), SizedQuery::new(query, Some(5), None));
        let raw_result = db
            .query_raw_keys_optional(&path_query, true, true, true, None, GroveVersion::latest())
            .unwrap()
            .expect("should get successfully");

        let raw_result: HashMap<_, _> = raw_result
            .into_iter()
            .map(|(path, key, element)| ((path, key), element))
            .collect();

        assert_eq!(raw_result.len(), 3);
        assert_eq!(raw_result.get(&(path.clone(), b"4".to_vec())), None);
        assert_eq!(raw_result.get(&(path.clone(), b"2".to_vec())), Some(&None));
        assert_eq!(
            raw_result.get(&(path, b"5".to_vec())),
            Some(&Some(Element::new_item(b"bye".to_vec())))
        );
    }

    #[test]
    fn test_query_raw_keys_options_with_range() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        db.insert(
            [TEST_LEAF].as_ref(),
            b"1",
            Element::new_item(b"hello".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF].as_ref(),
            b"3",
            Element::new_item(b"hello too".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF].as_ref(),
            b"5",
            Element::new_item(b"bye".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");

        let mut query = Query::new();
        query.insert_range(b"1".to_vec()..b"3".to_vec());
        query.insert_key(b"5".to_vec());
        let path = vec![TEST_LEAF.to_vec()];
        let path_query = PathQuery::new(path.clone(), SizedQuery::new(query, Some(5), None));
        let raw_result = db
            .query_raw_keys_optional(&path_query, true, true, true, None, GroveVersion::latest())
            .unwrap()
            .expect("should get successfully");

        let raw_result: HashMap<_, _> = raw_result
            .into_iter()
            .map(|(path, key, element)| ((path, key), element))
            .collect();

        assert_eq!(raw_result.len(), 3);
        assert_eq!(raw_result.get(&(path.clone(), b"4".to_vec())), None);
        assert_eq!(raw_result.get(&(path.clone(), b"2".to_vec())), Some(&None));
        assert_eq!(
            raw_result.get(&(path.clone(), b"5".to_vec())),
            Some(&Some(Element::new_item(b"bye".to_vec())))
        );
        assert_eq!(raw_result.get(&(path, b"3".to_vec())), None);
    }

    #[test]
    fn test_query_raw_keys_options_with_range_inclusive() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        db.insert(
            [TEST_LEAF].as_ref(),
            b"1",
            Element::new_item(b"hello".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF].as_ref(),
            b"3",
            Element::new_item(b"hello too".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF].as_ref(),
            b"5",
            Element::new_item(b"bye".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");

        let mut query = Query::new();
        query.insert_range_inclusive(b"1".to_vec()..=b"3".to_vec());
        query.insert_key(b"5".to_vec());
        let path = vec![TEST_LEAF.to_vec()];
        let path_query = PathQuery::new(path.clone(), SizedQuery::new(query, Some(5), None));
        let raw_result = db
            .query_raw_keys_optional(&path_query, true, true, true, None, GroveVersion::latest())
            .unwrap()
            .expect("should get successfully");

        let raw_result: HashMap<_, _> = raw_result
            .into_iter()
            .map(|(path, key, element)| ((path, key), element))
            .collect();

        assert_eq!(raw_result.len(), 4);
        assert_eq!(raw_result.get(&(path.clone(), b"4".to_vec())), None);
        assert_eq!(raw_result.get(&(path.clone(), b"2".to_vec())), Some(&None));
        assert_eq!(
            raw_result.get(&(path.clone(), b"5".to_vec())),
            Some(&Some(Element::new_item(b"bye".to_vec())))
        );
        assert_eq!(
            raw_result.get(&(path, b"3".to_vec())),
            Some(&Some(Element::new_item(b"hello too".to_vec())))
        );
    }

    #[test]
    fn test_query_raw_keys_options_with_range_bounds() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        db.insert(
            [TEST_LEAF].as_ref(),
            b"",
            Element::new_item(b"empty".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF].as_ref(),
            b"1",
            Element::new_item(b"hello".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF].as_ref(),
            b"3",
            Element::new_item(b"hello too".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF].as_ref(),
            b"5",
            Element::new_item(b"bye".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");

        let mut query = Query::new();
        query.insert_range(b"a".to_vec()..b"g".to_vec());

        let path = vec![TEST_LEAF.to_vec()];
        let path_query = PathQuery::new(path, SizedQuery::new(query, Some(4), None));
        db.query_raw_keys_optional(&path_query, true, true, true, None, GroveVersion::latest())
            .unwrap()
            .expect_err("range a should error");

        let mut query = Query::new();
        query.insert_range(b"a".to_vec()..b"c".to_vec()); // 2
        query.insert_key(b"5".to_vec()); // 3
        let path = vec![TEST_LEAF.to_vec()];
        let path_query = PathQuery::new(path, SizedQuery::new(query, Some(3), None));
        db.query_raw_keys_optional(&path_query, true, true, true, None, GroveVersion::latest())
            .unwrap()
            .expect("range b should not error");

        let mut query = Query::new();
        query.insert_range_inclusive(b"a".to_vec()..=b"c".to_vec()); // 3
        query.insert_key(b"5".to_vec()); // 4
        let path = vec![TEST_LEAF.to_vec()];
        let path_query = PathQuery::new(path, SizedQuery::new(query, Some(3), None));
        db.query_raw_keys_optional(&path_query, true, true, true, None, GroveVersion::latest())
            .unwrap()
            .expect_err("range c should error");

        let mut query = Query::new();
        query.insert_range(b"a".to_vec()..b"c".to_vec()); // 2
        query.insert_key(b"5".to_vec()); // 3
        let path = vec![TEST_LEAF.to_vec()];
        let path_query = PathQuery::new(path, SizedQuery::new(query, Some(2), None));
        db.query_raw_keys_optional(&path_query, true, true, true, None, GroveVersion::latest())
            .unwrap()
            .expect_err("range d should error");

        let mut query = Query::new();
        query.insert_range(b"z".to_vec()..b"10".to_vec());
        let path = vec![TEST_LEAF.to_vec()];
        let path_query = PathQuery::new(path, SizedQuery::new(query, Some(1000), None));
        db.query_raw_keys_optional(&path_query, true, true, true, None, GroveVersion::latest())
            .unwrap()
            .expect_err("range using 2 bytes should error");
    }

    #[test]
    fn test_query_raw_keys_options_with_empty_start_range() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        db.insert(
            [TEST_LEAF].as_ref(),
            b"",
            Element::new_item(b"empty".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF].as_ref(),
            b"1",
            Element::new_item(b"hello".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF].as_ref(),
            b"3",
            Element::new_item(b"hello too".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF].as_ref(),
            b"5",
            Element::new_item(b"bye".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");

        let mut query = Query::new();
        query.insert_range(b"".to_vec()..b"c".to_vec());
        let path = vec![TEST_LEAF.to_vec()];
        let path_query = PathQuery::new(path.clone(), SizedQuery::new(query, Some(1000), None));
        let raw_result = db
            .query_raw_keys_optional(&path_query, true, true, true, None, GroveVersion::latest())
            .unwrap()
            .expect("range starting with null should not error");

        let raw_result: HashMap<_, _> = raw_result
            .into_iter()
            .map(|(path, key, element)| ((path, key), element))
            .collect();

        assert_eq!(raw_result.len(), 100); // because is 99 ascii, and we have empty too
        assert_eq!(raw_result.get(&(path.clone(), b"4".to_vec())), Some(&None));
        assert_eq!(raw_result.get(&(path.clone(), b"2".to_vec())), Some(&None));
        assert_eq!(
            raw_result.get(&(path.clone(), b"5".to_vec())),
            Some(&Some(Element::new_item(b"bye".to_vec())))
        );
        assert_eq!(
            raw_result.get(&(path.clone(), b"3".to_vec())),
            Some(&Some(Element::new_item(b"hello too".to_vec())))
        );
        assert_eq!(
            raw_result.get(&(path, b"".to_vec())),
            Some(&Some(Element::new_item(b"empty".to_vec())))
        );
    }

    #[test]
    fn test_query_raw_keys_options_with_subquery_path() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        db.insert(
            [TEST_LEAF].as_ref(),
            b"",
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF, b""].as_ref(),
            b"",
            Element::new_item(b"null in null".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF, b""].as_ref(),
            b"1",
            Element::new_item(b"1 in null".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF].as_ref(),
            b"2",
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF, b"2"].as_ref(),
            b"1",
            Element::new_item(b"1 in 2".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF, b"2"].as_ref(),
            b"5",
            Element::new_item(b"5 in 2".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");

        let mut query = Query::new();
        query.insert_range(b"".to_vec()..b"c".to_vec());
        let path = vec![TEST_LEAF.to_vec()];
        let path_query = PathQuery::new(path, SizedQuery::new(query, Some(1000), None));
        db.query_keys_optional(&path_query, true, true, true, None, GroveVersion::latest())
            .unwrap()
            .expect_err("range should error because we didn't subquery");

        let mut query = Query::new();
        query.insert_range(b"".to_vec()..b"c".to_vec());
        query.set_subquery_key(b"1".to_vec());
        let path = vec![TEST_LEAF.to_vec()];
        let path_query = PathQuery::new(path, SizedQuery::new(query, Some(1000), None));
        let raw_result = db
            .query_raw_keys_optional(&path_query, true, true, true, None, GroveVersion::latest())
            .unwrap()
            .expect("query with subquery should not error");

        let raw_result: HashMap<_, _> = raw_result
            .into_iter()
            .map(|(path, key, element)| ((path, key), element))
            .collect();

        assert_eq!(raw_result.len(), 100); // because is 99 ascii, and we have empty too
        assert_eq!(
            raw_result.get(&(vec![TEST_LEAF.to_vec()], b"4".to_vec())),
            None
        );
        assert_eq!(
            raw_result.get(&(vec![TEST_LEAF.to_vec(), b"".to_vec()], b"4".to_vec())),
            None
        );
        assert_eq!(
            raw_result.get(&(vec![TEST_LEAF.to_vec(), b"4".to_vec()], b"1".to_vec())),
            Some(&None)
        ); // because we are sub-querying 1
        assert_eq!(
            raw_result.get(&(vec![TEST_LEAF.to_vec(), b"4".to_vec()], b"4".to_vec())),
            None
        );

        assert_eq!(
            raw_result.get(&(vec![TEST_LEAF.to_vec(), b"".to_vec()], b"1".to_vec())),
            Some(&Some(Element::new_item(b"1 in null".to_vec())))
        );
        assert_eq!(
            raw_result.get(&(vec![TEST_LEAF.to_vec(), b"2".to_vec()], b"1".to_vec())),
            Some(&Some(Element::new_item(b"1 in 2".to_vec())))
        );
    }

    #[test]
    fn test_query_raw_keys_options_with_subquery() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        db.insert(
            [TEST_LEAF].as_ref(),
            b"",
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF, b""].as_ref(),
            b"",
            Element::new_item(b"null in null".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF, b""].as_ref(),
            b"1",
            Element::new_item(b"1 in null".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF].as_ref(),
            b"2",
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF, b"2"].as_ref(),
            b"1",
            Element::new_item(b"1 in 2".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF, b"2"].as_ref(),
            b"5",
            Element::new_item(b"5 in 2".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF, b"2"].as_ref(),
            b"2",
            Element::new_item(b"2 in 2".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");

        let mut sub_query = Query::new();
        sub_query.insert_key(b"1".to_vec());
        sub_query.insert_key(b"2".to_vec());
        let mut query = Query::new();
        query.insert_range(b"".to_vec()..b"c".to_vec());
        query.set_subquery(sub_query);
        let path = vec![TEST_LEAF.to_vec()];
        let path_query = PathQuery::new(path, SizedQuery::new(query, Some(1000), None));
        let raw_result = db
            .query_raw_keys_optional(&path_query, true, true, true, None, GroveVersion::latest())
            .unwrap()
            .expect("query with subquery should not error");

        let raw_result: HashMap<_, _> = raw_result
            .into_iter()
            .map(|(path, key, element)| ((path, key), element))
            .collect();

        // because is 99 ascii, and we have empty too = 100 then x 2
        assert_eq!(raw_result.len(), 200);
        assert_eq!(
            raw_result.get(&(vec![TEST_LEAF.to_vec()], b"4".to_vec())),
            None
        );
        assert_eq!(
            raw_result.get(&(vec![TEST_LEAF.to_vec(), b"".to_vec()], b"4".to_vec())),
            None
        );
        assert_eq!(
            raw_result.get(&(vec![TEST_LEAF.to_vec(), b"4".to_vec()], b"1".to_vec())),
            Some(&None)
        ); // because we are sub-querying 1
        assert_eq!(
            raw_result.get(&(vec![TEST_LEAF.to_vec(), b"4".to_vec()], b"2".to_vec())),
            Some(&None)
        ); // because we are sub-querying 1
        assert_eq!(
            raw_result.get(&(vec![TEST_LEAF.to_vec(), b"4".to_vec()], b"4".to_vec())),
            None
        );

        assert_eq!(
            raw_result.get(&(vec![TEST_LEAF.to_vec(), b"".to_vec()], b"1".to_vec())),
            Some(&Some(Element::new_item(b"1 in null".to_vec())))
        );
        assert_eq!(
            raw_result.get(&(vec![TEST_LEAF.to_vec(), b"2".to_vec()], b"1".to_vec())),
            Some(&Some(Element::new_item(b"1 in 2".to_vec())))
        );
        assert_eq!(
            raw_result.get(&(vec![TEST_LEAF.to_vec(), b"2".to_vec()], b"2".to_vec())),
            Some(&Some(Element::new_item(b"2 in 2".to_vec())))
        );
        assert_eq!(
            raw_result.get(&(vec![TEST_LEAF.to_vec(), b"2".to_vec()], b"5".to_vec())),
            None
        ); // because we didn't query for it
    }

    #[test]
    fn test_query_raw_keys_options_with_subquery_having_intermediate_paths_missing() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        db.insert(
            [TEST_LEAF].as_ref(),
            b"",
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF].as_ref(),
            b"1",
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF].as_ref(),
            b"2",
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF].as_ref(),
            b"3",
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF, b"1"].as_ref(),
            b"deep_1",
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF, b"1", b"deep_1"].as_ref(),
            b"deeper_1",
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF, b"1", b"deep_1", b"deeper_1"].as_ref(),
            b"2",
            Element::new_item(b"found_me".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF, b"2"].as_ref(),
            b"1",
            Element::new_item(b"1 in 2".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF, b"2"].as_ref(),
            b"5",
            Element::new_item(b"5 in 2".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF, b"2"].as_ref(),
            b"2",
            Element::new_item(b"2 in 2".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");

        let mut sub_query = Query::new();
        sub_query.insert_key(b"1".to_vec());
        sub_query.insert_key(b"2".to_vec());
        let mut query = Query::new();
        query.insert_keys(vec![b"1".to_vec(), b"2".to_vec(), b"3".to_vec()]);
        query.set_subquery_path(vec![b"deep_1".to_vec(), b"deeper_1".to_vec()]);
        query.set_subquery(sub_query);
        let path = vec![TEST_LEAF.to_vec()];
        let path_query = PathQuery::new(path, SizedQuery::new(query, Some(1000), None));

        db.query_raw_keys_optional(&path_query, true, true, true, None, GroveVersion::latest())
            .unwrap()
            .expect_err(
                "query with subquery should error if error_if_intermediate_path_tree_not_present \
                 is set to true",
            );

        let raw_result = db
            .query_raw_keys_optional(&path_query, true, true, false, None, GroveVersion::latest())
            .unwrap()
            .expect("query with subquery should not error");

        // because is 99 ascii, and we have empty too = 100 then x 2
        assert_eq!(raw_result.len(), 6);

        let expected_result = vec![
            (
                vec![
                    b"test_leaf".to_vec(),
                    b"1".to_vec(),
                    b"deep_1".to_vec(),
                    b"deeper_1".to_vec(),
                ],
                b"1".to_vec(),
                None,
            ),
            (
                vec![
                    b"test_leaf".to_vec(),
                    b"1".to_vec(),
                    b"deep_1".to_vec(),
                    b"deeper_1".to_vec(),
                ],
                b"2".to_vec(),
                Some(Element::new_item(b"found_me".to_vec())),
            ),
            (
                vec![
                    b"test_leaf".to_vec(),
                    b"2".to_vec(),
                    b"deep_1".to_vec(),
                    b"deeper_1".to_vec(),
                ],
                b"1".to_vec(),
                None,
            ),
            (
                vec![
                    b"test_leaf".to_vec(),
                    b"2".to_vec(),
                    b"deep_1".to_vec(),
                    b"deeper_1".to_vec(),
                ],
                b"2".to_vec(),
                None,
            ),
            (
                vec![
                    b"test_leaf".to_vec(),
                    b"3".to_vec(),
                    b"deep_1".to_vec(),
                    b"deeper_1".to_vec(),
                ],
                b"1".to_vec(),
                None,
            ),
            (
                vec![
                    b"test_leaf".to_vec(),
                    b"3".to_vec(),
                    b"deep_1".to_vec(),
                    b"deeper_1".to_vec(),
                ],
                b"2".to_vec(),
                None,
            ),
        ];

        assert_eq!(raw_result, expected_result);
    }

    #[test]
    fn test_query_raw_keys_options_with_subquery_and_subquery_path() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        db.insert(
            [TEST_LEAF].as_ref(),
            b"",
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF, b""].as_ref(),
            b"",
            Element::new_item(b"null in null".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF, b""].as_ref(),
            b"1",
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF, b"", b"1"].as_ref(),
            b"2",
            Element::new_item(b"2 in null/1".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF].as_ref(),
            b"2",
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF, b"2"].as_ref(),
            b"1",
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF, b"2"].as_ref(),
            b"2",
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF, b"2", b"1"].as_ref(),
            b"2",
            Element::new_item(b"2 in 2/1".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF, b"2", b"1"].as_ref(),
            b"5",
            Element::new_item(b"5 in 2/1".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");

        // Our tree should be
        //      Test_Leaf
        //   ""        "2"
        //    |       /   \
        //   "1"     "1"   "2"
        //    |     /   \
        //   "2"   "2"  "5"

        let mut sub_query = Query::new();
        sub_query.insert_key(b"1".to_vec());
        sub_query.insert_key(b"2".to_vec());
        let mut query = Query::new();
        query.insert_range(b"".to_vec()..b"c".to_vec());
        query.set_subquery_key(b"1".to_vec());
        query.set_subquery(sub_query);
        let path = vec![TEST_LEAF.to_vec()];
        let path_query = PathQuery::new(path, SizedQuery::new(query, Some(1000), None));
        let raw_result = db
            .query_raw_keys_optional(&path_query, true, true, true, None, GroveVersion::latest())
            .unwrap()
            .expect("query with subquery should not error");

        let raw_result: HashMap<_, _> = raw_result
            .into_iter()
            .map(|(path, key, element)| ((path, key), element))
            .collect();

        // because is 99 ascii, and we have empty too = 100 then x 2
        assert_eq!(raw_result.len(), 200);
        assert_eq!(
            raw_result.get(&(vec![TEST_LEAF.to_vec()], b"4".to_vec())),
            None
        );
        assert_eq!(
            raw_result.get(&(vec![TEST_LEAF.to_vec(), b"".to_vec()], b"4".to_vec())),
            None
        );
        assert_eq!(
            raw_result.get(&(vec![TEST_LEAF.to_vec(), b"4".to_vec()], b"1".to_vec())),
            None
        );
        assert_eq!(
            raw_result.get(&(vec![TEST_LEAF.to_vec(), b"4".to_vec()], b"2".to_vec())),
            None
        );
        assert_eq!(
            raw_result.get(&(vec![TEST_LEAF.to_vec(), b"4".to_vec()], b"4".to_vec())),
            None
        );

        assert_eq!(
            raw_result.get(&(
                vec![TEST_LEAF.to_vec(), b"".to_vec(), b"1".to_vec()],
                b"2".to_vec()
            )),
            Some(&Some(Element::new_item(b"2 in null/1".to_vec())))
        );
        assert_eq!(
            raw_result.get(&(
                vec![TEST_LEAF.to_vec(), b"2".to_vec(), b"1".to_vec()],
                b"1".to_vec()
            )),
            Some(&None)
        );
        assert_eq!(
            raw_result.get(&(
                vec![TEST_LEAF.to_vec(), b"2".to_vec(), b"1".to_vec()],
                b"5".to_vec()
            )),
            None
        ); // because we didn't query for it
        assert_eq!(
            raw_result.get(&(
                vec![TEST_LEAF.to_vec(), b"2".to_vec(), b"1".to_vec()],
                b"2".to_vec()
            )),
            Some(&Some(Element::new_item(b"2 in 2/1".to_vec())))
        );
    }

    #[test]
    fn test_query_raw_keys_options_with_subquery_and_conditional_subquery() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        db.insert(
            [TEST_LEAF].as_ref(),
            b"",
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF, b""].as_ref(),
            b"",
            Element::new_item(b"null in null".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF, b""].as_ref(),
            b"1",
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF, b"", b"1"].as_ref(),
            b"2",
            Element::new_item(b"2 in null/1".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF].as_ref(),
            b"2",
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF, b"2"].as_ref(),
            b"1",
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF, b"2"].as_ref(),
            b"2",
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF, b"2", b"1"].as_ref(),
            b"2",
            Element::new_item(b"2 in 2/1".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF, b"2", b"1"].as_ref(),
            b"5",
            Element::new_item(b"5 in 2/1".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");

        // Our tree should be
        //      Test_Leaf
        //   ""        "2"
        //    |       /   \
        //   "1"     "1"   "2"
        //    |     /   \
        //   "2"   "2"  "5"

        let mut sub_query = Query::new();
        sub_query.insert_key(b"1".to_vec());
        sub_query.insert_key(b"2".to_vec());
        let mut conditional_sub_query = Query::new();
        conditional_sub_query.insert_key(b"5".to_vec());
        let mut query = Query::new();
        query.insert_range(b"".to_vec()..b"c".to_vec());
        query.set_subquery_key(b"1".to_vec());
        query.set_subquery(sub_query);
        query.add_conditional_subquery(
            QueryItem::Key(b"2".to_vec()),
            Some(vec![b"1".to_vec()]),
            Some(conditional_sub_query),
        );
        let path = vec![TEST_LEAF.to_vec()];
        let path_query = PathQuery::new(path, SizedQuery::new(query, Some(1000), None));
        let raw_result = db
            .query_raw_keys_optional(&path_query, true, true, true, None, GroveVersion::latest())
            .unwrap()
            .expect("query with subquery should not error");

        let raw_result: HashMap<_, _> = raw_result
            .into_iter()
            .map(|(path, key, element)| ((path, key), element))
            .collect();

        // 1 less than 200, because of the conditional subquery of 1 element that takes
        // 1 instead of 2
        assert_eq!(raw_result.len(), 199);
        assert_eq!(
            raw_result.get(&(vec![TEST_LEAF.to_vec()], b"4".to_vec())),
            None
        );
        assert_eq!(
            raw_result.get(&(vec![TEST_LEAF.to_vec(), b"".to_vec()], b"4".to_vec())),
            None
        );
        assert_eq!(
            raw_result.get(&(vec![TEST_LEAF.to_vec(), b"4".to_vec()], b"1".to_vec())),
            None
        );
        assert_eq!(
            raw_result.get(&(vec![TEST_LEAF.to_vec(), b"4".to_vec()], b"2".to_vec())),
            None
        );
        assert_eq!(
            raw_result.get(&(vec![TEST_LEAF.to_vec(), b"4".to_vec()], b"4".to_vec())),
            None
        );

        assert_eq!(
            raw_result.get(&(
                vec![TEST_LEAF.to_vec(), b"".to_vec(), b"1".to_vec()],
                b"2".to_vec()
            )),
            Some(&Some(Element::new_item(b"2 in null/1".to_vec())))
        );
        assert_eq!(
            raw_result.get(&(
                vec![TEST_LEAF.to_vec(), b"2".to_vec(), b"1".to_vec()],
                b"1".to_vec()
            )),
            None
        ); // conditional subquery overrides this
        assert_eq!(
            raw_result.get(&(
                vec![TEST_LEAF.to_vec(), b"2".to_vec(), b"1".to_vec()],
                b"5".to_vec()
            )),
            Some(&Some(Element::new_item(b"5 in 2/1".to_vec())))
        );
        assert_eq!(
            raw_result.get(&(
                vec![TEST_LEAF.to_vec(), b"2".to_vec(), b"1".to_vec()],
                b"2".to_vec()
            )),
            None
        ); // because we didn't query for it
    }

    #[test]
    fn test_query_keys_options_with_subquery_and_conditional_subquery_and_reference() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);
        db.insert(
            [ANOTHER_TEST_LEAF].as_ref(),
            b"5",
            Element::new_item(b"ref result".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");

        db.insert(
            [TEST_LEAF].as_ref(),
            b"",
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF, b""].as_ref(),
            b"",
            Element::new_item(b"null in null".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF, b""].as_ref(),
            b"1",
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF, b"", b"1"].as_ref(),
            b"2",
            Element::new_item(b"2 in null/1".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF].as_ref(),
            b"2",
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF, b"2"].as_ref(),
            b"1",
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF, b"2"].as_ref(),
            b"2",
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF, b"2", b"1"].as_ref(),
            b"2",
            Element::new_item(b"2 in 2/1".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");
        db.insert(
            [TEST_LEAF, b"2", b"1"].as_ref(),
            b"5",
            Element::new_reference_with_hops(
                AbsolutePathReference(vec![ANOTHER_TEST_LEAF.to_vec(), b"5".to_vec()]),
                Some(1),
            ),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert subtree successfully");

        // Our tree should be
        //      Test_Leaf         ANOTHER_TEST_LEAF
        //   ""        "2"              "5": "ref result"
        //    |       /   \
        //   "1"     "1"   "2"
        //    |     /   \
        //   "2"   "2"  "5"

        let mut sub_query = Query::new();
        sub_query.insert_key(b"1".to_vec());
        sub_query.insert_key(b"2".to_vec());
        let mut conditional_sub_query = Query::new();
        conditional_sub_query.insert_key(b"5".to_vec());
        let mut query = Query::new();
        query.insert_range(b"".to_vec()..b"c".to_vec());
        query.set_subquery_key(b"1".to_vec());
        query.set_subquery(sub_query);
        query.add_conditional_subquery(
            QueryItem::Key(b"2".to_vec()),
            Some(vec![b"1".to_vec()]),
            Some(conditional_sub_query),
        );
        let path = vec![TEST_LEAF.to_vec()];
        let path_query = PathQuery::new(path, SizedQuery::new(query, Some(1000), None));
        let result = db
            .query_keys_optional(&path_query, true, true, true, None, GroveVersion::latest())
            .unwrap()
            .expect("query with subquery should not error");

        let result: HashMap<_, _> = result
            .into_iter()
            .map(|(path, key, element)| ((path, key), element))
            .collect();

        // 1 less than 200, because of the conditional subquery of 1 element that takes
        // 1 instead of 2
        assert_eq!(result.len(), 199);
        assert_eq!(result.get(&(vec![TEST_LEAF.to_vec()], b"4".to_vec())), None);
        assert_eq!(
            result.get(&(vec![TEST_LEAF.to_vec(), b"".to_vec()], b"4".to_vec())),
            None
        );
        assert_eq!(
            result.get(&(vec![TEST_LEAF.to_vec(), b"4".to_vec()], b"1".to_vec())),
            None
        );
        assert_eq!(
            result.get(&(vec![TEST_LEAF.to_vec(), b"4".to_vec()], b"2".to_vec())),
            None
        );
        assert_eq!(
            result.get(&(vec![TEST_LEAF.to_vec(), b"4".to_vec()], b"4".to_vec())),
            None
        );

        assert_eq!(
            result.get(&(
                vec![TEST_LEAF.to_vec(), b"".to_vec(), b"1".to_vec()],
                b"2".to_vec()
            )),
            Some(&Some(Element::new_item(b"2 in null/1".to_vec())))
        );
        assert_eq!(
            result.get(&(
                vec![TEST_LEAF.to_vec(), b"2".to_vec(), b"1".to_vec()],
                b"1".to_vec()
            )),
            None
        ); // conditional subquery overrides this
        assert_eq!(
            result.get(&(
                vec![TEST_LEAF.to_vec(), b"2".to_vec(), b"1".to_vec()],
                b"5".to_vec()
            )),
            Some(&Some(Element::new_item(b"ref result".to_vec())))
        );
        assert_eq!(
            result.get(&(
                vec![TEST_LEAF.to_vec(), b"2".to_vec(), b"1".to_vec()],
                b"2".to_vec()
            )),
            None
        ); // because we didn't query for it
    }
}
