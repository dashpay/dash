//! Query
//! Implements functions in Element for querying
use grovedb_costs::{
    cost_return_on_error, cost_return_on_error_into, cost_return_on_error_into_no_add,
    cost_return_on_error_no_add, CostResult, CostsExt, OperationCost,
};
use grovedb_element::Element;
use grovedb_merk::{
    element::{decode::ElementDecodeExtensions, get::ElementFetchFromStorageExtensions},
    error::MerkErrorExt,
    proofs::{query::query_item::QueryItem, Query},
};
use grovedb_path::SubtreePath;
use grovedb_storage::{rocksdb_storage::RocksDbStorage, RawIterator, StorageContext};
use grovedb_version::{check_grovedb_v0, check_grovedb_v0_with_cost, version::GroveVersion};

use crate::{
    element::{path_query_push_args::PathQueryPushArgs, query_options::QueryOptions},
    operations::proof::util::path_as_slices_hex_to_ascii,
    query_result_type::{
        Path, QueryResultElement, QueryResultElements, QueryResultType,
        QueryResultType::{
            QueryElementResultType, QueryKeyElementPairResultType,
            QueryPathKeyElementTrioResultType,
        },
    },
    Error, PathQuery, SizedQuery, TransactionArg,
};

pub trait ElementQueryExtensions {
    fn get_query(
        storage: &RocksDbStorage,
        merk_path: &[&[u8]],
        query: &Query,
        query_options: QueryOptions,
        result_type: QueryResultType,
        transaction: TransactionArg,
        grove_version: &GroveVersion,
    ) -> CostResult<QueryResultElements, Error>;
    fn get_query_values(
        storage: &RocksDbStorage,
        merk_path: &[&[u8]],
        query: &Query,
        query_options: QueryOptions,
        transaction: TransactionArg,
        grove_version: &GroveVersion,
    ) -> CostResult<Vec<Element>, Error>;
    fn get_query_apply_function(
        storage: &RocksDbStorage,
        path: &[&[u8]],
        sized_query: &SizedQuery,
        query_options: QueryOptions,
        result_type: QueryResultType,
        transaction: TransactionArg,
        add_element_function: fn(PathQueryPushArgs, &GroveVersion) -> CostResult<(), Error>,
        grove_version: &GroveVersion,
    ) -> CostResult<(QueryResultElements, u16), Error>;
    fn get_path_query(
        storage: &RocksDbStorage,
        path_query: &PathQuery,
        query_options: QueryOptions,
        result_type: QueryResultType,
        transaction: TransactionArg,
        grove_version: &GroveVersion,
    ) -> CostResult<(QueryResultElements, u16), Error>;
    /// Returns a vector of elements, and the number of skipped elements
    fn get_sized_query(
        storage: &RocksDbStorage,
        path: &[&[u8]],
        sized_query: &SizedQuery,
        query_options: QueryOptions,
        result_type: QueryResultType,
        transaction: TransactionArg,
        grove_version: &GroveVersion,
    ) -> CostResult<(QueryResultElements, u16), Error>;
    /// Push arguments to path query
    fn path_query_push(
        args: PathQueryPushArgs,
        grove_version: &GroveVersion,
    ) -> CostResult<(), Error>;

    /// Takes a sized query and a key and returns subquery key and subquery as
    /// tuple
    fn subquery_paths_and_value_for_sized_query(
        sized_query: &SizedQuery,
        key: &[u8],
    ) -> (Option<Path>, Option<Query>);
    /// `decrease_limit_on_range_with_no_sub_elements` should generally be set
    /// to true, as having it false could mean very expensive queries.
    /// The queries would be expensive because we could go through many
    /// trees where the sub elements have no matches, hence the limit would
    /// not decrease and hence we would continue on the increasingly
    /// expensive query.
    // TODO: refactor
    fn query_item(
        storage: &RocksDbStorage,
        item: &QueryItem,
        results: &mut Vec<QueryResultElement>,
        path: &[&[u8]],
        sized_query: &SizedQuery,
        transaction: TransactionArg,
        limit: &mut Option<u16>,
        offset: &mut Option<u16>,
        query_options: QueryOptions,
        result_type: QueryResultType,
        add_element_function: fn(PathQueryPushArgs, &GroveVersion) -> CostResult<(), Error>,
        grove_version: &GroveVersion,
    ) -> CostResult<(), Error>;
    fn basic_push(args: PathQueryPushArgs, grove_version: &GroveVersion) -> Result<(), Error>;
}

impl ElementQueryExtensions for Element {
    /// Returns a vector of result elements based on given query
    fn get_query(
        storage: &RocksDbStorage,
        merk_path: &[&[u8]],
        query: &Query,
        query_options: QueryOptions,
        result_type: QueryResultType,
        transaction: TransactionArg,
        grove_version: &GroveVersion,
    ) -> CostResult<QueryResultElements, Error> {
        check_grovedb_v0_with_cost!(
            "insert_subtree_into_batch_operations",
            grove_version.grovedb_versions.element.get_query
        );

        let sized_query = SizedQuery::new(query.clone(), None, None);
        Element::get_sized_query(
            storage,
            merk_path,
            &sized_query,
            query_options,
            result_type,
            transaction,
            grove_version,
        )
        .map_ok(|(elements, _)| elements)
    }

    /// Get values of result elements coming from given query
    fn get_query_values(
        storage: &RocksDbStorage,
        merk_path: &[&[u8]],
        query: &Query,
        query_options: QueryOptions,
        transaction: TransactionArg,
        grove_version: &GroveVersion,
    ) -> CostResult<Vec<Element>, Error> {
        check_grovedb_v0_with_cost!(
            "get_query_values",
            grove_version.grovedb_versions.element.get_query_values
        );

        Element::get_query(
            storage,
            merk_path,
            query,
            query_options,
            QueryElementResultType,
            transaction,
            grove_version,
        )
        .flat_map_ok(|result_items| {
            let elements: Vec<Element> = result_items
                .elements
                .into_iter()
                .filter_map(|result_item| match result_item {
                    QueryResultElement::ElementResultItem(element) => Some(element),
                    QueryResultElement::KeyElementPairResultItem(_) => None,
                    QueryResultElement::PathKeyElementTrioResultItem(_) => None,
                })
                .collect();
            Ok(elements).wrap_with_cost(OperationCost::default())
        })
    }

    /// Returns a vector of result elements and the number of skipped items
    /// based on given query
    fn get_query_apply_function(
        storage: &RocksDbStorage,
        path: &[&[u8]],
        sized_query: &SizedQuery,
        query_options: QueryOptions,
        result_type: QueryResultType,
        transaction: TransactionArg,
        add_element_function: fn(PathQueryPushArgs, &GroveVersion) -> CostResult<(), Error>,
        grove_version: &GroveVersion,
    ) -> CostResult<(QueryResultElements, u16), Error> {
        check_grovedb_v0_with_cost!(
            "get_query_apply_function",
            grove_version
                .grovedb_versions
                .element
                .get_query_apply_function
        );

        let mut cost = OperationCost::default();

        let mut results = Vec::new();

        let mut limit = sized_query.limit;
        let original_offset = sized_query.offset;
        let mut offset = original_offset;

        if sized_query.query.left_to_right {
            for item in sized_query.query.iter() {
                cost_return_on_error!(
                    &mut cost,
                    Self::query_item(
                        storage,
                        item,
                        &mut results,
                        path,
                        sized_query,
                        transaction,
                        &mut limit,
                        &mut offset,
                        query_options,
                        result_type,
                        add_element_function,
                        grove_version,
                    )
                );
                if limit == Some(0) {
                    break;
                }
            }
        } else {
            for item in sized_query.query.rev_iter() {
                cost_return_on_error!(
                    &mut cost,
                    Self::query_item(
                        storage,
                        item,
                        &mut results,
                        path,
                        sized_query,
                        transaction,
                        &mut limit,
                        &mut offset,
                        query_options,
                        result_type,
                        add_element_function,
                        grove_version,
                    )
                );
                if limit == Some(0) {
                    break;
                }
            }
        }

        let skipped = if let Some(original_offset_unwrapped) = original_offset {
            original_offset_unwrapped - offset.unwrap()
        } else {
            0
        };
        Ok((QueryResultElements::from_elements(results), skipped)).wrap_with_cost(cost)
    }

    /// Returns a vector of elements excluding trees, and the number of skipped
    /// elements
    fn get_path_query(
        storage: &RocksDbStorage,
        path_query: &PathQuery,
        query_options: QueryOptions,
        result_type: QueryResultType,
        transaction: TransactionArg,
        grove_version: &GroveVersion,
    ) -> CostResult<(QueryResultElements, u16), Error> {
        check_grovedb_v0_with_cost!(
            "get_path_query",
            grove_version.grovedb_versions.element.get_path_query
        );

        let path_slices = path_query
            .path
            .iter()
            .map(|x| x.as_slice())
            .collect::<Vec<_>>();
        Element::get_query_apply_function(
            storage,
            path_slices.as_slice(),
            &path_query.query,
            query_options,
            result_type,
            transaction,
            Element::path_query_push,
            grove_version,
        )
    }

    /// Returns a vector of elements, and the number of skipped elements
    fn get_sized_query(
        storage: &RocksDbStorage,
        path: &[&[u8]],
        sized_query: &SizedQuery,
        query_options: QueryOptions,
        result_type: QueryResultType,
        transaction: TransactionArg,
        grove_version: &GroveVersion,
    ) -> CostResult<(QueryResultElements, u16), Error> {
        check_grovedb_v0_with_cost!(
            "get_sized_query",
            grove_version.grovedb_versions.element.get_sized_query
        );

        Element::get_query_apply_function(
            storage,
            path,
            sized_query,
            query_options,
            result_type,
            transaction,
            Element::path_query_push,
            grove_version,
        )
    }

    /// Push arguments to path query
    fn path_query_push(
        args: PathQueryPushArgs,
        grove_version: &GroveVersion,
    ) -> CostResult<(), Error> {
        use crate::util::{compat, TxRef};

        check_grovedb_v0_with_cost!(
            "path_query_push",
            grove_version.grovedb_versions.element.path_query_push
        );

        // println!("path_query_push {} \n", args);

        let mut cost = OperationCost::default();

        let PathQueryPushArgs {
            storage,
            transaction,
            key,
            element,
            path,
            subquery_path,
            subquery,
            left_to_right,
            query_options,
            result_type,
            results,
            limit,
            offset,
        } = args;

        let tx = TxRef::new(storage, transaction);

        let QueryOptions {
            allow_get_raw,
            allow_cache,
            decrease_limit_on_range_with_no_sub_elements,
            ..
        } = query_options;
        if element.is_any_tree() {
            let mut path_vec = path.to_vec();
            let key = cost_return_on_error_no_add!(
                cost,
                key.ok_or(Error::MissingParameter(
                    "the key must be provided when using a subquery path",
                ))
            );
            path_vec.push(key);

            if let Some(subquery) = subquery {
                if let Some(subquery_path) = &subquery_path {
                    path_vec.extend(subquery_path.iter().map(|k| k.as_slice()));
                }

                let inner_query = SizedQuery::new(subquery, *limit, *offset);
                let path_vec_owned = path_vec.iter().map(|x| x.to_vec()).collect();
                let inner_path_query = PathQuery::new(path_vec_owned, inner_query);

                let (mut sub_elements, skipped) = cost_return_on_error!(
                    &mut cost,
                    Element::get_path_query(
                        storage,
                        &inner_path_query,
                        query_options,
                        result_type,
                        transaction,
                        grove_version,
                    )
                );

                if let Some(limit) = limit {
                    if sub_elements.is_empty() && decrease_limit_on_range_with_no_sub_elements {
                        // we should decrease by 1 in this case
                        *limit = limit.saturating_sub(1);
                    } else {
                        *limit = limit.saturating_sub(sub_elements.len() as u16);
                    }
                }
                if let Some(offset) = offset {
                    *offset = offset.saturating_sub(skipped);
                }
                results.append(&mut sub_elements.elements);
            } else if let Some(subquery_path) = subquery_path {
                if offset.unwrap_or(0) == 0 {
                    if let Some((subquery_path_last_key, subquery_path_front_keys)) =
                        &subquery_path.split_last()
                    {
                        path_vec.extend(subquery_path_front_keys.iter().map(|k| k.as_slice()));

                        let subtree_path: SubtreePath<_> = path_vec.as_slice().into();
                        let subtree = cost_return_on_error!(
                            &mut cost,
                            compat::merk_optional_tx(
                                storage,
                                subtree_path,
                                tx.as_ref(),
                                None,
                                grove_version
                            )
                        );

                        match result_type {
                            QueryElementResultType => {
                                if let Some(element) = cost_return_on_error_into!(
                                    &mut cost,
                                    Element::get_optional_with_absolute_refs(
                                        &subtree,
                                        path_vec.as_slice(),
                                        subquery_path_last_key.as_slice(),
                                        allow_cache,
                                        grove_version,
                                    )
                                ) {
                                    results.push(QueryResultElement::ElementResultItem(element));
                                }
                            }
                            QueryKeyElementPairResultType => {
                                if let Some(element) = cost_return_on_error_into!(
                                    &mut cost,
                                    Element::get_optional_with_absolute_refs(
                                        &subtree,
                                        path_vec.as_slice(),
                                        subquery_path_last_key.as_slice(),
                                        allow_cache,
                                        grove_version,
                                    )
                                ) {
                                    results.push(QueryResultElement::KeyElementPairResultItem((
                                        subquery_path_last_key.to_vec(),
                                        element,
                                    )));
                                }
                            }
                            QueryPathKeyElementTrioResultType => {
                                if let Some(element) = cost_return_on_error_into!(
                                    &mut cost,
                                    Element::get_optional_with_absolute_refs(
                                        &subtree,
                                        path_vec.as_slice(),
                                        subquery_path_last_key.as_slice(),
                                        allow_cache,
                                        grove_version,
                                    )
                                ) {
                                    results.push(QueryResultElement::PathKeyElementTrioResultItem(
                                        (
                                            path_vec.iter().map(|p| p.to_vec()).collect(),
                                            subquery_path_last_key.to_vec(),
                                            element,
                                        ),
                                    ));
                                }
                            }
                        }
                    } else {
                        return Err(Error::CorruptedCodeExecution(
                            "subquery_paths can not be empty",
                        ))
                        .wrap_with_cost(cost);
                    };

                    if let Some(limit) = limit {
                        *limit -= 1;
                    }
                } else if let Some(offset) = offset {
                    *offset -= 1;
                }
            } else if allow_get_raw {
                cost_return_on_error_no_add!(
                    cost,
                    Element::basic_push(
                        PathQueryPushArgs {
                            storage,
                            transaction,
                            key: Some(key),
                            element,
                            path,
                            subquery_path,
                            subquery,
                            left_to_right,
                            query_options,
                            result_type,
                            results,
                            limit,
                            offset,
                        },
                        grove_version
                    )
                );
            } else {
                return Err(Error::InvalidPath(
                    "you must provide a subquery or a subquery_path when interacting with a Tree \
                     of trees"
                        .to_owned(),
                ))
                .wrap_with_cost(cost);
            }
        } else {
            cost_return_on_error_no_add!(
                cost,
                Element::basic_push(
                    PathQueryPushArgs {
                        storage,
                        transaction,
                        key,
                        element,
                        path,
                        subquery_path,
                        subquery,
                        left_to_right,
                        query_options,
                        result_type,
                        results,
                        limit,
                        offset,
                    },
                    grove_version
                )
            );
        }
        Ok(()).wrap_with_cost(cost)
    }

    /// Takes a sized query and a key and returns subquery key and subquery as
    /// tuple
    fn subquery_paths_and_value_for_sized_query(
        sized_query: &SizedQuery,
        key: &[u8],
    ) -> (Option<Path>, Option<Query>) {
        if let Some(conditional_subquery_branches) =
            &sized_query.query.conditional_subquery_branches
        {
            for (query_item, subquery_branch) in conditional_subquery_branches {
                if query_item.contains(key) {
                    let subquery_path = subquery_branch.subquery_path.clone();
                    let subquery = subquery_branch
                        .subquery
                        .as_ref()
                        .map(|query| *query.clone());
                    return (subquery_path, subquery);
                }
            }
        }
        let subquery_path = sized_query
            .query
            .default_subquery_branch
            .subquery_path
            .clone();
        let subquery = sized_query
            .query
            .default_subquery_branch
            .subquery
            .as_ref()
            .map(|query| *query.clone());
        (subquery_path, subquery)
    }

    /// `decrease_limit_on_range_with_no_sub_elements` should generally be set
    /// to true, as having it false could mean very expensive queries.
    /// The queries would be expensive because we could go through many
    /// trees where the sub elements have no matches, hence the limit would
    /// not decrease and hence we would continue on the increasingly
    /// expensive query.
    fn query_item(
        storage: &RocksDbStorage,
        item: &QueryItem,
        results: &mut Vec<QueryResultElement>,
        path: &[&[u8]],
        sized_query: &SizedQuery,
        transaction: TransactionArg,
        limit: &mut Option<u16>,
        offset: &mut Option<u16>,
        query_options: QueryOptions,
        result_type: QueryResultType,
        add_element_function: fn(PathQueryPushArgs, &GroveVersion) -> CostResult<(), Error>,
        grove_version: &GroveVersion,
    ) -> CostResult<(), Error> {
        use grovedb_storage::Storage;

        use crate::util::{compat, TxRef};

        check_grovedb_v0_with_cost!(
            "query_item",
            grove_version.grovedb_versions.element.query_item
        );

        let mut cost = OperationCost::default();
        let tx = TxRef::new(storage, transaction);

        let subtree_path: SubtreePath<_> = path.into();

        if !item.is_range() {
            // this is a query on a key
            if let QueryItem::Key(key) = item {
                let subtree_res = compat::merk_optional_tx(
                    storage,
                    subtree_path,
                    tx.as_ref(),
                    None,
                    grove_version,
                );

                if subtree_res.value().is_err()
                    && !matches!(subtree_res.value(), Err(Error::PathParentLayerNotFound(..)))
                {
                    // simulating old macro's behavior by letting this particular kind of error to
                    // pass and to short circuit with the rest
                    return subtree_res.map_ok(|_| ());
                }

                let element_res = subtree_res
                    .flat_map_ok(|subtree| {
                        Element::get(&subtree, key, query_options.allow_cache, grove_version)
                            .add_context(format!("path is {}", path_as_slices_hex_to_ascii(path)))
                            .map_err(|e| e.into())
                    })
                    .unwrap_add_cost(&mut cost);

                match element_res {
                    Ok(element) => {
                        let (subquery_path, subquery) =
                            Self::subquery_paths_and_value_for_sized_query(sized_query, key);
                        match add_element_function(
                            PathQueryPushArgs {
                                storage,
                                transaction,
                                key: Some(key.as_slice()),
                                element,
                                path,
                                subquery_path,
                                subquery,
                                left_to_right: sized_query.query.left_to_right,
                                query_options,
                                result_type,
                                results,
                                limit,
                                offset,
                            },
                            grove_version,
                        )
                        .unwrap_add_cost(&mut cost)
                        {
                            Ok(_) => Ok(()),
                            Err(e) => {
                                if !query_options.error_if_intermediate_path_tree_not_present {
                                    match e {
                                        Error::PathParentLayerNotFound(_) => Ok(()),
                                        _ => Err(e),
                                    }
                                } else {
                                    Err(e)
                                }
                            }
                        }
                    }
                    Err(Error::PathKeyNotFound(_)) => Ok(()),
                    Err(e) => {
                        if !query_options.error_if_intermediate_path_tree_not_present {
                            match e {
                                Error::PathParentLayerNotFound(_) => Ok(()),
                                _ => Err(e),
                            }
                        } else {
                            Err(e)
                        }
                    }
                }
            } else {
                Err(Error::InternalError(
                    "QueryItem must be a Key if not a range".to_string(),
                ))
            }
        } else {
            // this is a query on a range
            let ctx = storage
                .get_transactional_storage_context(subtree_path, None, tx.as_ref())
                .unwrap_add_cost(&mut cost);

            let mut iter = ctx.raw_iter();

            item.seek_for_iter(&mut iter, sized_query.query.left_to_right)
                .unwrap_add_cost(&mut cost);

            while item
                .iter_is_valid_for_type(&iter, *limit, sized_query.query.left_to_right)
                .unwrap_add_cost(&mut cost)
            {
                let element = cost_return_on_error_into_no_add!(
                    cost,
                    Element::raw_decode(
                        iter.value()
                            .unwrap_add_cost(&mut cost)
                            .expect("if key exists then value should too"),
                        grove_version
                    )
                );
                let key = iter
                    .key()
                    .unwrap_add_cost(&mut cost)
                    .expect("key should exist");
                let (subquery_path, subquery) =
                    Self::subquery_paths_and_value_for_sized_query(sized_query, key);
                let result_with_cost = add_element_function(
                    PathQueryPushArgs {
                        storage,
                        transaction,
                        key: Some(key),
                        element,
                        path,
                        subquery_path,
                        subquery,
                        left_to_right: sized_query.query.left_to_right,
                        query_options,
                        result_type,
                        results,
                        limit,
                        offset,
                    },
                    grove_version,
                );
                let result = result_with_cost.unwrap_add_cost(&mut cost);
                match result {
                    Ok(x) => x,
                    Err(e) => {
                        if !query_options.error_if_intermediate_path_tree_not_present {
                            match e {
                                Error::PathKeyNotFound(_) | Error::PathParentLayerNotFound(_) => (),
                                _ => return Err(e).wrap_with_cost(cost),
                            }
                        } else {
                            return Err(e).wrap_with_cost(cost);
                        }
                    }
                }
                if sized_query.query.left_to_right {
                    iter.next().unwrap_add_cost(&mut cost);
                } else {
                    iter.prev().unwrap_add_cost(&mut cost);
                }
                cost.seek_count += 1;
            }
            Ok(())
        }
        .wrap_with_cost(cost)
    }

    fn basic_push(args: PathQueryPushArgs, grove_version: &GroveVersion) -> Result<(), Error> {
        check_grovedb_v0!(
            "basic_push",
            grove_version.grovedb_versions.element.basic_push
        );

        // println!("basic_push {}", args);
        let PathQueryPushArgs {
            path,
            key,
            element,
            result_type,
            results,
            limit,
            offset,
            ..
        } = args;

        let element = element.convert_if_reference_to_absolute_reference(path, key)?;

        if offset.unwrap_or(0) == 0 {
            match result_type {
                QueryElementResultType => {
                    results.push(QueryResultElement::ElementResultItem(element));
                }
                QueryKeyElementPairResultType => {
                    let key = key.ok_or(Error::CorruptedPath(
                        "basic push must have a key".to_string(),
                    ))?;
                    results.push(QueryResultElement::KeyElementPairResultItem((
                        Vec::from(key),
                        element,
                    )));
                }
                QueryPathKeyElementTrioResultType => {
                    let key = key.ok_or(Error::CorruptedPath(
                        "basic push must have a key".to_string(),
                    ))?;
                    let path = path.iter().map(|a| a.to_vec()).collect();
                    results.push(QueryResultElement::PathKeyElementTrioResultItem((
                        path,
                        Vec::from(key),
                        element,
                    )));
                }
            }
            if let Some(limit) = limit {
                *limit -= 1;
            }
        } else if let Some(offset) = offset {
            *offset -= 1;
        }
        Ok(())
    }
}

#[cfg(test)]
mod tests {
    use grovedb_element::Element;
    use grovedb_merk::{element::insert::ElementInsertToStorageExtensions, proofs::Query};
    use grovedb_storage::{Storage, StorageBatch};
    use grovedb_version::version::GroveVersion;

    use crate::{
        element::query::{ElementQueryExtensions, QueryOptions},
        query_result_type::{
            KeyElementPair, QueryResultElement, QueryResultElements,
            QueryResultType::{QueryKeyElementPairResultType, QueryPathKeyElementTrioResultType},
        },
        tests::{make_test_grovedb, TEST_LEAF},
        SizedQuery,
    };

    #[test]
    fn test_get_query() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        db.insert(
            [TEST_LEAF].as_ref(),
            b"d",
            Element::new_item(b"ayyd".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("cannot insert element");
        db.insert(
            [TEST_LEAF].as_ref(),
            b"c",
            Element::new_item(b"ayyc".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("cannot insert element");
        db.insert(
            [TEST_LEAF].as_ref(),
            b"a",
            Element::new_item(b"ayya".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("cannot insert element");
        db.insert(
            [TEST_LEAF].as_ref(),
            b"b",
            Element::new_item(b"ayyb".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("cannot insert element");

        // Test queries by key
        let mut query = Query::new();
        query.insert_key(b"c".to_vec());
        query.insert_key(b"a".to_vec());

        assert_eq!(
            Element::get_query_values(
                &db.db,
                &[TEST_LEAF],
                &query,
                QueryOptions::default(),
                None,
                grove_version
            )
            .unwrap()
            .expect("expected successful get_query"),
            vec![
                Element::new_item(b"ayya".to_vec()),
                Element::new_item(b"ayyc".to_vec())
            ]
        );

        // Test range query
        let mut query = Query::new();
        query.insert_range(b"b".to_vec()..b"d".to_vec());
        query.insert_range(b"a".to_vec()..b"c".to_vec());
        assert_eq!(
            Element::get_query_values(
                &db.db,
                &[TEST_LEAF],
                &query,
                QueryOptions::default(),
                None,
                grove_version
            )
            .unwrap()
            .expect("expected successful get_query"),
            vec![
                Element::new_item(b"ayya".to_vec()),
                Element::new_item(b"ayyb".to_vec()),
                Element::new_item(b"ayyc".to_vec())
            ]
        );

        // Test range inclusive query
        let mut query = Query::new();
        query.insert_range_inclusive(b"b".to_vec()..=b"d".to_vec());
        query.insert_range(b"b".to_vec()..b"c".to_vec());
        assert_eq!(
            Element::get_query_values(
                &db.db,
                &[TEST_LEAF],
                &query,
                QueryOptions::default(),
                None,
                grove_version
            )
            .unwrap()
            .expect("expected successful get_query"),
            vec![
                Element::new_item(b"ayyb".to_vec()),
                Element::new_item(b"ayyc".to_vec()),
                Element::new_item(b"ayyd".to_vec())
            ]
        );

        // Test overlaps
        let mut query = Query::new();
        query.insert_key(b"a".to_vec());
        query.insert_range(b"b".to_vec()..b"d".to_vec());
        query.insert_range(b"a".to_vec()..b"c".to_vec());
        assert_eq!(
            Element::get_query_values(
                &db.db,
                &[TEST_LEAF],
                &query,
                QueryOptions::default(),
                None,
                grove_version
            )
            .unwrap()
            .expect("expected successful get_query"),
            vec![
                Element::new_item(b"ayya".to_vec()),
                Element::new_item(b"ayyb".to_vec()),
                Element::new_item(b"ayyc".to_vec())
            ]
        );
    }

    #[test]
    fn test_get_query_with_path() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        db.insert(
            [TEST_LEAF].as_ref(),
            b"d",
            Element::new_item(b"ayyd".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("cannot insert element");
        db.insert(
            [TEST_LEAF].as_ref(),
            b"c",
            Element::new_item(b"ayyc".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("cannot insert element");
        db.insert(
            [TEST_LEAF].as_ref(),
            b"a",
            Element::new_item(b"ayya".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("cannot insert element");
        db.insert(
            [TEST_LEAF].as_ref(),
            b"b",
            Element::new_item(b"ayyb".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("cannot insert element");

        // Test queries by key
        let mut query = Query::new();
        query.insert_key(b"c".to_vec());
        query.insert_key(b"a".to_vec());
        assert_eq!(
            Element::get_query(
                &db.db,
                &[TEST_LEAF],
                &query,
                QueryOptions::default(),
                QueryPathKeyElementTrioResultType,
                None,
                grove_version
            )
            .unwrap()
            .expect("expected successful get_query")
            .to_path_key_elements(),
            vec![
                (
                    vec![TEST_LEAF.to_vec()],
                    b"a".to_vec(),
                    Element::new_item(b"ayya".to_vec())
                ),
                (
                    vec![TEST_LEAF.to_vec()],
                    b"c".to_vec(),
                    Element::new_item(b"ayyc".to_vec())
                )
            ]
        );
    }

    #[test]
    fn test_get_range_query() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        let batch = StorageBatch::new();
        let storage = &db.db;
        let transaction = db.start_transaction();

        let mut merk = db
            .open_transactional_merk_at_path(
                [TEST_LEAF].as_ref().into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("cannot open Merk"); // TODO implement costs

        Element::new_item(b"ayyd".to_vec())
            .insert(&mut merk, b"d", None, grove_version)
            .unwrap()
            .expect("expected successful insertion");
        Element::new_item(b"ayyc".to_vec())
            .insert(&mut merk, b"c", None, grove_version)
            .unwrap()
            .expect("expected successful insertion");
        Element::new_item(b"ayya".to_vec())
            .insert(&mut merk, b"a", None, grove_version)
            .unwrap()
            .expect("expected successful insertion");
        Element::new_item(b"ayyb".to_vec())
            .insert(&mut merk, b"b", None, grove_version)
            .unwrap()
            .expect("expected successful insertion");

        storage
            .commit_multi_context_batch(batch, None)
            .unwrap()
            .expect("expected successful batch commit");

        transaction.commit().unwrap();

        // Test range inclusive query
        let mut query = Query::new();
        query.insert_range(b"a".to_vec()..b"d".to_vec());

        let ascending_query = SizedQuery::new(query.clone(), None, None);
        let (elements, skipped) = Element::get_sized_query(
            storage,
            &[TEST_LEAF],
            &ascending_query,
            QueryOptions::default(),
            QueryKeyElementPairResultType,
            None,
            grove_version,
        )
        .unwrap()
        .expect("expected successful get_query");

        let elements: Vec<KeyElementPair> = elements
            .into_iterator()
            .filter_map(|result_item| match result_item {
                QueryResultElement::ElementResultItem(_element) => None,
                QueryResultElement::KeyElementPairResultItem(key_element_pair) => {
                    Some(key_element_pair)
                }
                QueryResultElement::PathKeyElementTrioResultItem(_) => None,
            })
            .collect();
        assert_eq!(
            elements,
            vec![
                (b"a".to_vec(), Element::new_item(b"ayya".to_vec())),
                (b"b".to_vec(), Element::new_item(b"ayyb".to_vec())),
                (b"c".to_vec(), Element::new_item(b"ayyc".to_vec())),
            ]
        );
        assert_eq!(skipped, 0);

        query.left_to_right = false;

        let backwards_query = SizedQuery::new(query.clone(), None, None);
        let (elements, skipped) = Element::get_sized_query(
            storage,
            &[TEST_LEAF],
            &backwards_query,
            QueryOptions::default(),
            QueryKeyElementPairResultType,
            None,
            grove_version,
        )
        .unwrap()
        .expect("expected successful get_query");

        let elements: Vec<KeyElementPair> = elements
            .into_iterator()
            .filter_map(|result_item| match result_item {
                QueryResultElement::ElementResultItem(_element) => None,
                QueryResultElement::KeyElementPairResultItem(key_element_pair) => {
                    Some(key_element_pair)
                }
                QueryResultElement::PathKeyElementTrioResultItem(_) => None,
            })
            .collect();
        assert_eq!(
            elements,
            vec![
                (b"c".to_vec(), Element::new_item(b"ayyc".to_vec())),
                (b"b".to_vec(), Element::new_item(b"ayyb".to_vec())),
                (b"a".to_vec(), Element::new_item(b"ayya".to_vec())),
            ]
        );
        assert_eq!(skipped, 0);
    }

    #[test]
    fn test_get_range_inclusive_query() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        let batch = StorageBatch::new();

        let storage = &db.db;
        let transaction = db.start_transaction();

        let mut merk = db
            .open_transactional_merk_at_path(
                [TEST_LEAF].as_ref().into(),
                &transaction,
                Some(&batch),
                grove_version,
            )
            .unwrap()
            .expect("cannot open Merk");

        Element::new_item(b"ayyd".to_vec())
            .insert(&mut merk, b"d", None, grove_version)
            .unwrap()
            .expect("expected successful insertion");
        Element::new_item(b"ayyc".to_vec())
            .insert(&mut merk, b"c", None, grove_version)
            .unwrap()
            .expect("expected successful insertion");
        Element::new_item(b"ayya".to_vec())
            .insert(&mut merk, b"a", None, grove_version)
            .unwrap()
            .expect("expected successful insertion");
        Element::new_item(b"ayyb".to_vec())
            .insert(&mut merk, b"b", None, grove_version)
            .unwrap()
            .expect("expected successful insertion");

        storage
            .commit_multi_context_batch(batch, None)
            .unwrap()
            .expect("expected successful batch commit");

        transaction.commit().unwrap();

        // Test range inclusive query
        let mut query = Query::new_with_direction(true);
        query.insert_range_inclusive(b"a".to_vec()..=b"d".to_vec());

        let ascending_query = SizedQuery::new(query.clone(), None, None);
        fn check_elements_no_skipped(
            (elements, skipped): (QueryResultElements, u16),
            reverse: bool,
        ) {
            let mut expected = vec![
                (b"a".to_vec(), Element::new_item(b"ayya".to_vec())),
                (b"b".to_vec(), Element::new_item(b"ayyb".to_vec())),
                (b"c".to_vec(), Element::new_item(b"ayyc".to_vec())),
                (b"d".to_vec(), Element::new_item(b"ayyd".to_vec())),
            ];
            if reverse {
                expected.reverse();
            }
            assert_eq!(elements.to_key_elements(), expected);
            assert_eq!(skipped, 0);
        }

        check_elements_no_skipped(
            Element::get_sized_query(
                storage,
                &[TEST_LEAF],
                &ascending_query,
                QueryOptions::default(),
                QueryKeyElementPairResultType,
                None,
                grove_version,
            )
            .unwrap()
            .expect("expected successful get_query"),
            false,
        );

        query.left_to_right = false;

        let backwards_query = SizedQuery::new(query.clone(), None, None);
        check_elements_no_skipped(
            Element::get_sized_query(
                storage,
                &[TEST_LEAF],
                &backwards_query,
                QueryOptions::default(),
                QueryKeyElementPairResultType,
                None,
                grove_version,
            )
            .unwrap()
            .expect("expected successful get_query"),
            true,
        );

        // Test range inclusive query
        let mut query = Query::new_with_direction(false);
        query.insert_range_inclusive(b"b".to_vec()..=b"d".to_vec());
        query.insert_range(b"a".to_vec()..b"c".to_vec());

        let backwards_query = SizedQuery::new(query.clone(), None, None);
        check_elements_no_skipped(
            Element::get_sized_query(
                storage,
                &[TEST_LEAF],
                &backwards_query,
                QueryOptions::default(),
                QueryKeyElementPairResultType,
                None,
                grove_version,
            )
            .unwrap()
            .expect("expected successful get_query"),
            true,
        );
    }

    #[test]
    fn test_get_limit_query() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        db.insert(
            [TEST_LEAF].as_ref(),
            b"d",
            Element::new_item(b"ayyd".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("cannot insert element");
        db.insert(
            [TEST_LEAF].as_ref(),
            b"c",
            Element::new_item(b"ayyc".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("cannot insert element");
        db.insert(
            [TEST_LEAF].as_ref(),
            b"a",
            Element::new_item(b"ayya".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("cannot insert element");
        db.insert(
            [TEST_LEAF].as_ref(),
            b"b",
            Element::new_item(b"ayyb".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("cannot insert element");

        // Test queries by key
        let mut query = Query::new_with_direction(true);
        query.insert_key(b"c".to_vec());
        query.insert_key(b"a".to_vec());

        // since these are just keys a backwards query will keep same order
        let backwards_query = SizedQuery::new(query.clone(), None, None);
        let (elements, skipped) = Element::get_sized_query(
            &db.db,
            &[TEST_LEAF],
            &backwards_query,
            QueryOptions::default(),
            QueryKeyElementPairResultType,
            None,
            grove_version,
        )
        .unwrap()
        .expect("expected successful get_query");
        assert_eq!(
            elements.to_key_elements(),
            vec![
                (b"a".to_vec(), Element::new_item(b"ayya".to_vec())),
                (b"c".to_vec(), Element::new_item(b"ayyc".to_vec())),
            ]
        );
        assert_eq!(skipped, 0);

        // Test queries by key
        let mut query = Query::new_with_direction(false);
        query.insert_key(b"c".to_vec());
        query.insert_key(b"a".to_vec());

        // since these are just keys a backwards query will keep same order
        let backwards_query = SizedQuery::new(query.clone(), None, None);
        let (elements, skipped) = Element::get_sized_query(
            &db.db,
            &[TEST_LEAF],
            &backwards_query,
            QueryOptions::default(),
            QueryKeyElementPairResultType,
            None,
            grove_version,
        )
        .unwrap()
        .expect("expected successful get_query");
        assert_eq!(
            elements.to_key_elements(),
            vec![
                (b"c".to_vec(), Element::new_item(b"ayyc".to_vec())),
                (b"a".to_vec(), Element::new_item(b"ayya".to_vec())),
            ]
        );
        assert_eq!(skipped, 0);

        // The limit will mean we will only get back 1 item
        let limit_query = SizedQuery::new(query.clone(), Some(1), None);
        let (elements, skipped) = Element::get_sized_query(
            &db.db,
            &[TEST_LEAF],
            &limit_query,
            QueryOptions::default(),
            QueryKeyElementPairResultType,
            None,
            grove_version,
        )
        .unwrap()
        .expect("expected successful get_query");
        assert_eq!(
            elements.to_key_elements(),
            vec![(b"c".to_vec(), Element::new_item(b"ayyc".to_vec())),]
        );
        assert_eq!(skipped, 0);

        // Test range query
        let mut query = Query::new_with_direction(true);
        query.insert_range(b"b".to_vec()..b"d".to_vec());
        query.insert_range(b"a".to_vec()..b"c".to_vec());
        let limit_query = SizedQuery::new(query.clone(), Some(2), None);
        let (elements, skipped) = Element::get_sized_query(
            &db.db,
            &[TEST_LEAF],
            &limit_query,
            QueryOptions::default(),
            QueryKeyElementPairResultType,
            None,
            grove_version,
        )
        .unwrap()
        .expect("expected successful get_query");
        assert_eq!(
            elements.to_key_elements(),
            vec![
                (b"a".to_vec(), Element::new_item(b"ayya".to_vec())),
                (b"b".to_vec(), Element::new_item(b"ayyb".to_vec()))
            ]
        );
        assert_eq!(skipped, 0);

        let limit_offset_query = SizedQuery::new(query.clone(), Some(2), Some(1));
        let (elements, skipped) = Element::get_sized_query(
            &db.db,
            &[TEST_LEAF],
            &limit_offset_query,
            QueryOptions::default(),
            QueryKeyElementPairResultType,
            None,
            grove_version,
        )
        .unwrap()
        .expect("expected successful get_query");
        assert_eq!(
            elements.to_key_elements(),
            vec![
                (b"b".to_vec(), Element::new_item(b"ayyb".to_vec())),
                (b"c".to_vec(), Element::new_item(b"ayyc".to_vec()))
            ]
        );
        assert_eq!(skipped, 1);

        // Test range query
        let mut query = Query::new_with_direction(false);
        query.insert_range(b"b".to_vec()..b"d".to_vec());
        query.insert_range(b"a".to_vec()..b"c".to_vec());

        let limit_offset_backwards_query = SizedQuery::new(query.clone(), Some(2), Some(1));
        let (elements, skipped) = Element::get_sized_query(
            &db.db,
            &[TEST_LEAF],
            &limit_offset_backwards_query,
            QueryOptions::default(),
            QueryKeyElementPairResultType,
            None,
            grove_version,
        )
        .unwrap()
        .expect("expected successful get_query");
        assert_eq!(
            elements.to_key_elements(),
            vec![
                (b"b".to_vec(), Element::new_item(b"ayyb".to_vec())),
                (b"a".to_vec(), Element::new_item(b"ayya".to_vec()))
            ]
        );
        assert_eq!(skipped, 1);

        // Test range inclusive query
        let mut query = Query::new_with_direction(true);
        query.insert_range_inclusive(b"b".to_vec()..=b"d".to_vec());
        query.insert_range(b"b".to_vec()..b"c".to_vec());
        let limit_full_query = SizedQuery::new(query.clone(), Some(5), Some(0));
        let (elements, skipped) = Element::get_sized_query(
            &db.db,
            &[TEST_LEAF],
            &limit_full_query,
            QueryOptions::default(),
            QueryKeyElementPairResultType,
            None,
            grove_version,
        )
        .unwrap()
        .expect("expected successful get_query");
        assert_eq!(
            elements.to_key_elements(),
            vec![
                (b"b".to_vec(), Element::new_item(b"ayyb".to_vec())),
                (b"c".to_vec(), Element::new_item(b"ayyc".to_vec())),
                (b"d".to_vec(), Element::new_item(b"ayyd".to_vec())),
            ]
        );
        assert_eq!(skipped, 0);

        let mut query = Query::new_with_direction(false);
        query.insert_range_inclusive(b"b".to_vec()..=b"d".to_vec());
        query.insert_range(b"b".to_vec()..b"c".to_vec());

        let limit_offset_backwards_query = SizedQuery::new(query.clone(), Some(2), Some(1));
        let (elements, skipped) = Element::get_sized_query(
            &db.db,
            &[TEST_LEAF],
            &limit_offset_backwards_query,
            QueryOptions::default(),
            QueryKeyElementPairResultType,
            None,
            grove_version,
        )
        .unwrap()
        .expect("expected successful get_query");
        assert_eq!(
            elements.to_key_elements(),
            vec![
                (b"c".to_vec(), Element::new_item(b"ayyc".to_vec())),
                (b"b".to_vec(), Element::new_item(b"ayyb".to_vec())),
            ]
        );
        assert_eq!(skipped, 1);

        // Test overlaps
        let mut query = Query::new_with_direction(false);
        query.insert_key(b"a".to_vec());
        query.insert_range(b"b".to_vec()..b"d".to_vec());
        query.insert_range(b"b".to_vec()..b"c".to_vec());
        let limit_backwards_query = SizedQuery::new(query.clone(), Some(2), Some(1));
        let (elements, skipped) = Element::get_sized_query(
            &db.db,
            &[TEST_LEAF],
            &limit_backwards_query,
            QueryOptions::default(),
            QueryKeyElementPairResultType,
            None,
            grove_version,
        )
        .unwrap()
        .expect("expected successful get_query");
        assert_eq!(
            elements.to_key_elements(),
            vec![
                (b"b".to_vec(), Element::new_item(b"ayyb".to_vec())),
                (b"a".to_vec(), Element::new_item(b"ayya".to_vec())),
            ]
        );
        assert_eq!(skipped, 1);
    }
}
