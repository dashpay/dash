//! Queries

mod grove_branch_query_result;
mod grove_trunk_query_result;
mod path_branch_chunk_query;
mod path_trunk_chunk_query;

use std::{
    borrow::{Cow, Cow::Borrowed},
    cmp::Ordering,
    fmt,
};

use bincode::{Decode, Encode};
#[cfg(any(feature = "minimal", feature = "verify"))]
pub use grove_branch_query_result::GroveBranchQueryResult;
#[cfg(any(feature = "minimal", feature = "verify"))]
pub use grove_trunk_query_result::{GroveTrunkQueryResult, LeafInfo};
#[cfg(any(feature = "minimal", feature = "verify"))]
use grovedb_merk::proofs::query::query_item::QueryItem;
use grovedb_merk::proofs::query::{Key, SubqueryBranch};
#[cfg(any(feature = "minimal", feature = "verify"))]
use grovedb_merk::proofs::Query;
use grovedb_version::{check_grovedb_v0, version::GroveVersion};
use indexmap::IndexMap;
#[cfg(any(feature = "minimal", feature = "verify"))]
pub use path_branch_chunk_query::PathBranchChunkQuery;
#[cfg(any(feature = "minimal", feature = "verify"))]
pub use path_trunk_chunk_query::PathTrunkChunkQuery;

use crate::operations::proof::util::hex_to_ascii;
#[cfg(any(feature = "minimal", feature = "verify"))]
use crate::query_result_type::PathKey;
#[cfg(any(feature = "minimal", feature = "verify"))]
use crate::Error;

#[cfg(any(feature = "minimal", feature = "verify"))]
#[derive(Debug, Clone, PartialEq, Encode, Decode)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
/// Path query
///
/// Represents a path to a specific GroveDB tree and a corresponding query to
/// apply to the given tree.
pub struct PathQuery {
    /// Path
    pub path: Vec<Vec<u8>>,
    /// Query
    pub query: SizedQuery,
}

#[cfg(any(feature = "minimal", feature = "verify"))]
impl fmt::Display for PathQuery {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "PathQuery {{ path: [")?;
        for (i, path_element) in self.path.iter().enumerate() {
            if i > 0 {
                write!(f, ", ")?;
            }
            write!(f, "{}", hex_to_ascii(path_element))?;
        }
        write!(f, "], query: {} }}", self.query)
    }
}

#[cfg(any(feature = "minimal", feature = "verify"))]
#[derive(Debug, Clone, PartialEq, Encode, Decode)]
#[cfg_attr(feature = "serde", derive(serde::Serialize, serde::Deserialize))]
/// Holds a query to apply to a tree and an optional limit/offset value.
/// Limit and offset values affect the size of the result set.
pub struct SizedQuery {
    /// Query
    pub query: Query,
    /// Limit
    pub limit: Option<u16>,
    /// Offset
    pub offset: Option<u16>,
}

#[cfg(any(feature = "minimal", feature = "verify"))]
impl fmt::Display for SizedQuery {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(f, "SizedQuery {{ query: {}", self.query)?;
        if let Some(limit) = self.limit {
            write!(f, ", limit: {}", limit)?;
        }
        if let Some(offset) = self.offset {
            write!(f, ", offset: {}", offset)?;
        }
        write!(f, " }}")
    }
}

#[cfg(any(feature = "minimal", feature = "verify"))]
impl SizedQuery {
    /// New sized query
    pub const fn new(query: Query, limit: Option<u16>, offset: Option<u16>) -> Self {
        Self {
            query,
            limit,
            offset,
        }
    }

    /// New sized query with one key
    pub fn new_single_key(key: Vec<u8>) -> Self {
        Self {
            query: Query::new_single_key(key),
            limit: None,
            offset: None,
        }
    }

    /// New sized query with one key
    pub fn new_single_query_item(query_item: QueryItem) -> Self {
        Self {
            query: Query::new_single_query_item(query_item),
            limit: None,
            offset: None,
        }
    }
}

#[cfg(any(feature = "minimal", feature = "verify"))]
impl PathQuery {
    /// New path query
    pub const fn new(path: Vec<Vec<u8>>, query: SizedQuery) -> Self {
        Self { path, query }
    }

    /// New path query with a single key
    pub fn new_single_key(path: Vec<Vec<u8>>, key: Vec<u8>) -> Self {
        Self {
            path,
            query: SizedQuery::new_single_key(key),
        }
    }

    /// New path query with a single query item
    pub fn new_single_query_item(path: Vec<Vec<u8>>, query_item: QueryItem) -> Self {
        Self {
            path,
            query: SizedQuery::new_single_query_item(query_item),
        }
    }

    /// New unsized path query
    pub const fn new_unsized(path: Vec<Vec<u8>>, query: Query) -> Self {
        let query = SizedQuery::new(query, None, None);
        Self { path, query }
    }

    /// The max depth of the query, this is the maximum layers we could get back
    /// from grovedb
    /// If the max depth can not be calculated we get None
    /// This would occur if the recursion level was too high
    pub fn max_depth(&self) -> Option<u16> {
        self.query.query.max_depth()
    }

    /// Gets the path of all terminal keys
    pub fn terminal_keys(
        &self,
        max_results: usize,
        grove_version: &GroveVersion,
    ) -> Result<Vec<PathKey>, Error> {
        check_grovedb_v0!(
            "merge",
            grove_version
                .grovedb_versions
                .path_query_methods
                .terminal_keys
        );
        let mut result: Vec<(Vec<Vec<u8>>, Vec<u8>)> = vec![];
        self.query
            .query
            .terminal_keys(self.path.clone(), max_results, &mut result)
            .map_err(Error::MerkError)?;
        Ok(result)
    }

    /// Combines multiple path queries into one equivalent path query
    pub fn merge(
        mut path_queries: Vec<&PathQuery>,
        grove_version: &GroveVersion,
    ) -> Result<Self, Error> {
        check_grovedb_v0!(
            "merge",
            grove_version.grovedb_versions.path_query_methods.merge
        );
        if path_queries.is_empty() {
            return Err(Error::InvalidInput(
                "merge function requires at least 1 path query",
            ));
        }
        if path_queries.len() == 1 {
            return Ok(path_queries.remove(0).clone());
        }

        let (common_path, next_index) = PathQuery::get_common_path(&path_queries);

        let mut queries_for_common_path_this_level: Vec<Query> = vec![];

        let mut queries_for_common_path_sub_level: Vec<SubqueryBranch> = vec![];

        // convert all the paths after the common path to queries
        path_queries.into_iter().try_for_each(|path_query| {
            if path_query.query.offset.is_some() {
                return Err(Error::NotSupported(
                    "can not merge pathqueries with offsets".to_string(),
                ));
            }
            if path_query.query.limit.is_some() {
                return Err(Error::NotSupported(
                    "can not merge pathqueries with limits, consider setting the limit after the \
                     merge"
                        .to_string(),
                ));
            }
            path_query
                .to_subquery_branch_with_offset_start_index(next_index)
                .map(|unsized_path_query| {
                    if unsized_path_query.subquery_path.is_none() {
                        queries_for_common_path_this_level
                            .push(*unsized_path_query.subquery.unwrap());
                    } else {
                        queries_for_common_path_sub_level.push(unsized_path_query);
                    }
                })
        })?;

        let mut merged_query = Query::merge_multiple(queries_for_common_path_this_level);
        // add conditional subqueries
        for sub_path_query in queries_for_common_path_sub_level {
            let SubqueryBranch {
                subquery_path,
                subquery,
            } = sub_path_query;
            let mut subquery_path =
                subquery_path.ok_or(Error::CorruptedCodeExecution("subquery path must exist"))?;
            let key = subquery_path.remove(0); // must exist
            merged_query.insert_item(QueryItem::Key(key.clone()));
            let rest_of_path = if subquery_path.is_empty() {
                None
            } else {
                Some(subquery_path)
            };
            let subquery_branch = SubqueryBranch {
                subquery_path: rest_of_path,
                subquery,
            };
            merged_query.merge_conditional_boxed_subquery(QueryItem::Key(key), subquery_branch);
        }

        Ok(PathQuery::new_unsized(common_path, merged_query))
    }

    /// Given a set of path queries, this returns an array of path keys that are
    /// common across all the path queries.
    /// Also returns the point at which they stopped being equal.
    fn get_common_path(path_queries: &[&PathQuery]) -> (Vec<Vec<u8>>, usize) {
        let min_path_length = path_queries
            .iter()
            .map(|path_query| path_query.path.len())
            .min()
            .expect("expect path_queries length to be 2 or more");

        let mut common_path = vec![];
        let mut level = 0;

        while level < min_path_length {
            let keys_at_level = path_queries
                .iter()
                .map(|path_query| &path_query.path[level])
                .collect::<Vec<_>>();
            let first_key = keys_at_level[0];

            let keys_are_uniform = keys_at_level.iter().all(|&curr_key| curr_key == first_key);

            if keys_are_uniform {
                common_path.push(first_key.to_vec());
                level += 1;
            } else {
                break;
            }
        }
        (common_path, level)
    }

    /// Given a path and a starting point, a query that is equivalent to the
    /// path is generated example: [a, b, c] =>
    ///     query a
    ///         cond a
    ///             query b
    ///                 cond b
    ///                    query c
    fn to_subquery_branch_with_offset_start_index(
        &self,
        start_index: usize,
    ) -> Result<SubqueryBranch, Error> {
        let path = &self.path;

        match path.len().cmp(&start_index) {
            Ordering::Equal => Ok(SubqueryBranch {
                subquery_path: None,
                subquery: Some(Box::new(self.query.query.clone())),
            }),
            Ordering::Less => Err(Error::CorruptedCodeExecution(
                "invalid start index for path query merge",
            )),
            _ => {
                let (_, remainder) = path.split_at(start_index);

                Ok(SubqueryBranch {
                    subquery_path: Some(remainder.to_vec()),
                    subquery: Some(Box::new(self.query.query.clone())),
                })
            }
        }
    }

    pub fn should_add_parent_tree_at_path(
        &self,
        path: &[&[u8]],
        grove_version: &GroveVersion,
    ) -> Result<bool, Error> {
        check_grovedb_v0!(
            "should_add_parent_tree_at_path",
            grove_version
                .grovedb_versions
                .path_query_methods
                .should_add_parent_tree_at_path
        );

        fn recursive_should_add_parent_tree_at_path<'b>(query: &'b Query, path: &[&[u8]]) -> bool {
            if path.is_empty() {
                return query.add_parent_tree_on_subquery;
            }

            let key = path[0];
            let path_after_top_removed = &path[1..];

            if let Some(conditional_branches) = &query.conditional_subquery_branches {
                for (query_item, subquery_branch) in conditional_branches {
                    if query_item.contains(key) {
                        if let Some(subquery_path) = &subquery_branch.subquery_path {
                            if path_after_top_removed.len() <= subquery_path.len() {
                                if path_after_top_removed
                                    .iter()
                                    .zip(subquery_path)
                                    .all(|(a, b)| *a == b.as_slice())
                                {
                                    return if path_after_top_removed.len() == subquery_path.len() {
                                        subquery_branch.subquery.as_ref().is_some_and(|subquery| {
                                            subquery.add_parent_tree_on_subquery
                                        })
                                    } else {
                                        false
                                    };
                                }
                            } else if path_after_top_removed
                                .iter()
                                .take(subquery_path.len())
                                .zip(subquery_path)
                                .all(|(a, b)| *a == b.as_slice())
                            {
                                if let Some(subquery) = &subquery_branch.subquery {
                                    return recursive_should_add_parent_tree_at_path(
                                        subquery,
                                        &path_after_top_removed[subquery_path.len()..],
                                    );
                                }
                            }
                        } else if let Some(subquery) = &subquery_branch.subquery {
                            return recursive_should_add_parent_tree_at_path(
                                subquery,
                                path_after_top_removed,
                            );
                        }

                        return false;
                    }
                }
            }

            if let Some(subquery_path) = &query.default_subquery_branch.subquery_path {
                if path_after_top_removed.len() <= subquery_path.len() {
                    if path_after_top_removed
                        .iter()
                        .zip(subquery_path)
                        .all(|(a, b)| *a == b.as_slice())
                    {
                        return if path_after_top_removed.len() == subquery_path.len() {
                            query
                                .default_subquery_branch
                                .subquery
                                .as_ref()
                                .is_some_and(|subquery| subquery.add_parent_tree_on_subquery)
                        } else {
                            false
                        };
                    }
                } else if path_after_top_removed
                    .iter()
                    .take(subquery_path.len())
                    .zip(subquery_path)
                    .all(|(a, b)| *a == b.as_slice())
                {
                    if let Some(subquery) = &query.default_subquery_branch.subquery {
                        return recursive_should_add_parent_tree_at_path(
                            subquery,
                            &path_after_top_removed[subquery_path.len()..],
                        );
                    }
                }
            } else if let Some(subquery) = &query.default_subquery_branch.subquery {
                return recursive_should_add_parent_tree_at_path(subquery, path_after_top_removed);
            }

            false
        }

        let self_path_len = self.path.len();
        let given_path_len = path.len();

        Ok(match given_path_len.cmp(&self_path_len) {
            Ordering::Less => false,
            Ordering::Equal => {
                if path.iter().zip(&self.path).all(|(a, b)| *a == b.as_slice()) {
                    self.query.query.add_parent_tree_on_subquery
                } else {
                    false
                }
            }
            Ordering::Greater => {
                if !self.path.iter().zip(path).all(|(a, b)| a.as_slice() == *b) {
                    return Ok(false);
                }
                recursive_should_add_parent_tree_at_path(&self.query.query, &path[self_path_len..])
            }
        })
    }

    pub fn query_items_at_path(
        &self,
        path: &[&[u8]],
        grove_version: &GroveVersion,
    ) -> Result<Option<SinglePathSubquery<'_>>, Error> {
        check_grovedb_v0!(
            "query_items_at_path",
            grove_version
                .grovedb_versions
                .path_query_methods
                .query_items_at_path
        );
        fn recursive_query_items<'b>(
            query: &'b Query,
            path: &[&[u8]],
        ) -> Option<SinglePathSubquery<'b>> {
            if path.is_empty() {
                return Some(SinglePathSubquery::from_query(query));
            }

            let key = path[0];
            let path_after_top_removed = &path[1..];

            if let Some(conditional_branches) = &query.conditional_subquery_branches {
                for (query_item, subquery_branch) in conditional_branches {
                    if query_item.contains(key) {
                        if let Some(subquery_path) = &subquery_branch.subquery_path {
                            if path_after_top_removed.len() <= subquery_path.len() {
                                if path_after_top_removed
                                    .iter()
                                    .zip(subquery_path)
                                    .all(|(a, b)| *a == b.as_slice())
                                {
                                    return if path_after_top_removed.len() == subquery_path.len() {
                                        subquery_branch.subquery.as_ref().map(|subquery| {
                                            SinglePathSubquery::from_query(subquery)
                                        })
                                    } else {
                                        let last_path_item = path.len() == subquery_path.len();
                                        let has_subquery = subquery_branch.subquery.is_some();
                                        Some(SinglePathSubquery::from_key_when_in_path(
                                            &subquery_path[path_after_top_removed.len()],
                                            last_path_item,
                                            has_subquery,
                                        ))
                                    };
                                }
                            } else if path_after_top_removed
                                .iter()
                                .take(subquery_path.len())
                                .zip(subquery_path)
                                .all(|(a, b)| *a == b.as_slice())
                            {
                                if let Some(subquery) = &subquery_branch.subquery {
                                    return recursive_query_items(
                                        subquery,
                                        &path_after_top_removed[subquery_path.len()..],
                                    );
                                }
                            }
                        } else if let Some(subquery) = &subquery_branch.subquery {
                            return recursive_query_items(subquery, path_after_top_removed);
                        }

                        return None;
                    }
                }
            }

            if let Some(subquery_path) = &query.default_subquery_branch.subquery_path {
                if path_after_top_removed.len() <= subquery_path.len() {
                    if path_after_top_removed
                        .iter()
                        .zip(subquery_path)
                        .all(|(a, b)| *a == b.as_slice())
                    {
                        // The paths are equal for example if we had a sub path of
                        // path : 1 / 2
                        // subquery : All items

                        // If we are asking what is the subquery when we are at 1 / 2
                        // we should get
                        return if path_after_top_removed.len() == subquery_path.len() {
                            query
                                .default_subquery_branch
                                .subquery
                                .as_ref()
                                .map(|subquery| SinglePathSubquery::from_query(subquery))
                        } else {
                            let last_path_item = path.len() == subquery_path.len();
                            let has_subquery = query.default_subquery_branch.subquery.is_some();
                            Some(SinglePathSubquery::from_key_when_in_path(
                                &subquery_path[path_after_top_removed.len()],
                                last_path_item,
                                has_subquery,
                            ))
                        };
                    }
                } else if path_after_top_removed
                    .iter()
                    .take(subquery_path.len())
                    .zip(subquery_path)
                    .all(|(a, b)| *a == b.as_slice())
                {
                    if let Some(subquery) = &query.default_subquery_branch.subquery {
                        return recursive_query_items(
                            subquery,
                            &path_after_top_removed[subquery_path.len()..],
                        );
                    }
                }
            } else if let Some(subquery) = &query.default_subquery_branch.subquery {
                return recursive_query_items(subquery, path_after_top_removed);
            }

            None
        }

        let self_path_len = self.path.len();
        let given_path_len = path.len();

        Ok(match given_path_len.cmp(&self_path_len) {
            Ordering::Less => {
                if path.iter().zip(&self.path).all(|(a, b)| *a == b.as_slice()) {
                    Some(SinglePathSubquery::from_key_when_in_path(
                        &self.path[given_path_len],
                        false,
                        true,
                    ))
                } else {
                    None
                }
            }
            Ordering::Equal => {
                if path.iter().zip(&self.path).all(|(a, b)| *a == b.as_slice()) {
                    Some(SinglePathSubquery::from_path_query(self))
                } else {
                    None
                }
            }
            Ordering::Greater => {
                if !self.path.iter().zip(path).all(|(a, b)| a.as_slice() == *b) {
                    return Ok(None);
                }
                recursive_query_items(&self.query.query, &path[self_path_len..])
            }
        })
    }
}

#[cfg(any(feature = "minimal", feature = "verify"))]
#[derive(Debug, Clone, PartialEq)]
pub enum HasSubquery<'a> {
    NoSubquery,
    Always,
    Conditionally(Cow<'a, IndexMap<QueryItem, SubqueryBranch>>),
}

#[cfg(any(feature = "minimal", feature = "verify"))]
impl fmt::Display for HasSubquery<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            HasSubquery::NoSubquery => write!(f, "NoSubquery"),
            HasSubquery::Always => write!(f, "Always"),
            HasSubquery::Conditionally(map) => {
                writeln!(f, "Conditionally {{")?;
                for (query_item, subquery_branch) in map.iter() {
                    writeln!(f, "  {query_item}: {subquery_branch},")?;
                }
                write!(f, "}}")
            }
        }
    }
}

impl HasSubquery<'_> {
    /// Checks to see if we have a subquery on a specific key
    pub fn has_subquery_on_key(&self, key: &[u8]) -> bool {
        match self {
            HasSubquery::NoSubquery => false,
            HasSubquery::Conditionally(conditionally) => conditionally
                .keys()
                .any(|query_item| query_item.contains(key)),
            HasSubquery::Always => true,
        }
    }
}

/// This represents a query where the items might be borrowed, it is used to get
/// subquery information
#[cfg(any(feature = "minimal", feature = "verify"))]
#[derive(Debug, Clone, PartialEq)]
pub struct SinglePathSubquery<'a> {
    /// Items
    pub items: Cow<'a, Vec<QueryItem>>,
    /// Default subquery branch
    pub has_subquery: HasSubquery<'a>,
    /// Left to right?
    pub left_to_right: bool,
    /// In the path of the path_query, or in a subquery path
    pub in_path: Option<Cow<'a, Key>>,
}

#[cfg(any(feature = "minimal", feature = "verify"))]
impl fmt::Display for SinglePathSubquery<'_> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        writeln!(f, "InternalCowItemsQuery {{")?;
        writeln!(f, "  items: [")?;
        for item in self.items.iter() {
            writeln!(f, "    {item},")?;
        }
        writeln!(f, "  ]")?;
        writeln!(f, "  has_subquery: {}", self.has_subquery)?;
        writeln!(f, "  left_to_right: {}", self.left_to_right)?;
        match &self.in_path {
            Some(path) => writeln!(f, "  in_path: Some({})", hex_to_ascii(path)),
            None => writeln!(f, "  in_path: None"),
        }?;
        write!(f, "}}")
    }
}

impl<'a> SinglePathSubquery<'a> {
    /// Checks to see if we have a subquery on a specific key
    pub fn has_subquery_or_matching_in_path_on_key(&self, key: &[u8]) -> bool {
        if self.has_subquery.has_subquery_on_key(key) {
            true
        } else if let Some(path) = self.in_path.as_ref() {
            path.as_slice() == key
        } else {
            false
        }
    }

    pub fn from_key_when_in_path(
        key: &'a Vec<u8>,
        subquery_is_last_path_item: bool,
        subquery_has_inner_subquery: bool,
    ) -> SinglePathSubquery<'a> {
        // in this case there should be no in_path, because we are trying to get this
        // level of items and nothing underneath
        let in_path = if subquery_is_last_path_item && !subquery_has_inner_subquery {
            None
        } else {
            Some(Borrowed(key))
        };
        SinglePathSubquery {
            items: Cow::Owned(vec![QueryItem::Key(key.clone())]),
            has_subquery: HasSubquery::NoSubquery,
            left_to_right: true,
            in_path,
        }
    }

    pub fn from_path_query(path_query: &PathQuery) -> SinglePathSubquery<'_> {
        Self::from_query(&path_query.query.query)
    }

    pub fn from_query(query: &Query) -> SinglePathSubquery<'_> {
        let has_subquery = if query.default_subquery_branch.subquery.is_some()
            || query.default_subquery_branch.subquery_path.is_some()
        {
            HasSubquery::Always
        } else if let Some(conditional) = query.conditional_subquery_branches.as_ref() {
            HasSubquery::Conditionally(Cow::Borrowed(conditional))
        } else {
            HasSubquery::NoSubquery
        };
        SinglePathSubquery {
            items: Cow::Borrowed(&query.items),
            has_subquery,
            left_to_right: query.left_to_right,
            in_path: None,
        }
    }
}

#[cfg(feature = "minimal")]
#[cfg(test)]
mod tests {
    use std::{borrow::Cow, ops::RangeFull};

    use bincode::{config::standard, decode_from_slice, encode_to_vec};
    use grovedb_merk::proofs::{
        query::{query_item::QueryItem, SubqueryBranch},
        Query,
    };
    use grovedb_version::version::GroveVersion;
    use indexmap::IndexMap;

    use crate::{
        query::{HasSubquery, SinglePathSubquery},
        query_result_type::QueryResultType,
        tests::{common::compare_result_tuples, make_deep_tree, TEST_LEAF},
        Element, GroveDb, PathQuery, SizedQuery,
    };

    #[test]
    fn test_same_path_different_query_merge() {
        let grove_version = GroveVersion::latest();
        let temp_db = make_deep_tree(grove_version);

        // starting with no subquery, just a single path and a key query
        let mut query_one = Query::new();
        query_one.insert_key(b"key1".to_vec());
        let path_query_one =
            PathQuery::new_unsized(vec![TEST_LEAF.to_vec(), b"innertree".to_vec()], query_one);

        let proof = temp_db
            .prove_query(&path_query_one, None, grove_version)
            .unwrap()
            .unwrap();
        let (_, result_set_one) =
            GroveDb::verify_query_raw(proof.as_slice(), &path_query_one, grove_version)
                .expect("should execute proof");
        assert_eq!(result_set_one.len(), 1);

        let mut query_two = Query::new();
        query_two.insert_key(b"key2".to_vec());
        let path_query_two =
            PathQuery::new_unsized(vec![TEST_LEAF.to_vec(), b"innertree".to_vec()], query_two);

        let proof = temp_db
            .prove_query(&path_query_two, None, grove_version)
            .unwrap()
            .unwrap();
        let (_, result_set_two) =
            GroveDb::verify_query_raw(proof.as_slice(), &path_query_two, grove_version)
                .expect("should execute proof");
        assert_eq!(result_set_two.len(), 1);

        let merged_path_query =
            PathQuery::merge(vec![&path_query_one, &path_query_two], grove_version)
                .expect("should merge path queries");

        let proof = temp_db
            .prove_query(&merged_path_query, None, grove_version)
            .unwrap()
            .unwrap();
        let (_, result_set_tree) =
            GroveDb::verify_query_raw(proof.as_slice(), &merged_path_query, grove_version)
                .expect("should execute proof");
        assert_eq!(result_set_tree.len(), 2);
    }

    #[test]
    fn test_different_same_length_path_with_different_query_merge() {
        let grove_version = GroveVersion::latest();
        // Tests for
        // [a, c, Q]
        // [a, m, Q]
        let temp_db = make_deep_tree(grove_version);

        let mut query_one = Query::new();
        query_one.insert_key(b"key1".to_vec());
        let path_query_one =
            PathQuery::new_unsized(vec![TEST_LEAF.to_vec(), b"innertree".to_vec()], query_one);

        let proof = temp_db
            .prove_query(&path_query_one, None, grove_version)
            .unwrap()
            .unwrap();
        let (_, result_set_one) =
            GroveDb::verify_query_raw(proof.as_slice(), &path_query_one, grove_version)
                .expect("should execute proof");
        assert_eq!(result_set_one.len(), 1);

        let mut query_two = Query::new();
        query_two.insert_key(b"key4".to_vec());
        let path_query_two =
            PathQuery::new_unsized(vec![TEST_LEAF.to_vec(), b"innertree4".to_vec()], query_two);

        let proof = temp_db
            .prove_query(&path_query_two, None, grove_version)
            .unwrap()
            .unwrap();
        let (_, result_set_two) =
            GroveDb::verify_query_raw(proof.as_slice(), &path_query_two, grove_version)
                .expect("should execute proof");
        assert_eq!(result_set_two.len(), 1);

        let merged_path_query =
            PathQuery::merge(vec![&path_query_one, &path_query_two], grove_version)
                .expect("expect to merge path queries");
        assert_eq!(merged_path_query.path, vec![TEST_LEAF.to_vec()]);
        assert_eq!(merged_path_query.query.query.items.len(), 2);

        let proof = temp_db
            .prove_query(&merged_path_query, None, grove_version)
            .unwrap()
            .unwrap();
        let (_, result_set_merged) =
            GroveDb::verify_query_raw(proof.as_slice(), &merged_path_query, grove_version)
                .expect("should execute proof");
        assert_eq!(result_set_merged.len(), 2);

        let keys = [b"key1".to_vec(), b"key4".to_vec()];
        let values = [b"value1".to_vec(), b"value4".to_vec()];
        let elements = values.map(|x| Element::new_item(x).serialize(grove_version).unwrap());
        let expected_result_set: Vec<(Vec<u8>, Vec<u8>)> = keys.into_iter().zip(elements).collect();
        compare_result_tuples(result_set_merged, expected_result_set);

        // longer length path queries
        let mut query_one = Query::new();
        query_one.insert_all();
        let path_query_one = PathQuery::new_unsized(
            vec![
                b"deep_leaf".to_vec(),
                b"deep_node_1".to_vec(),
                b"deeper_2".to_vec(),
            ],
            query_one.clone(),
        );

        let proof = temp_db
            .prove_query(&path_query_one, None, grove_version)
            .unwrap()
            .unwrap();
        let (_, result_set_one) =
            GroveDb::verify_query_raw(proof.as_slice(), &path_query_one, grove_version)
                .expect("should execute proof");
        assert_eq!(result_set_one.len(), 3);

        let mut query_two = Query::new();
        query_two.insert_all();

        let path_query_two = PathQuery::new_unsized(
            vec![
                b"deep_leaf".to_vec(),
                b"deep_node_2".to_vec(),
                b"deeper_4".to_vec(),
            ],
            query_two.clone(),
        );

        let proof = temp_db
            .prove_query(&path_query_two, None, grove_version)
            .unwrap()
            .unwrap();
        let (_, result_set_two) =
            GroveDb::verify_query_raw(proof.as_slice(), &path_query_two, grove_version)
                .expect("should execute proof");
        assert_eq!(result_set_two.len(), 2);

        let mut query_three = Query::new();
        query_three.insert_range_after(b"key7".to_vec()..);

        let path_query_three = PathQuery::new_unsized(
            vec![
                b"deep_leaf".to_vec(),
                b"deep_node_2".to_vec(),
                b"deeper_3".to_vec(),
            ],
            query_three.clone(),
        );

        let proof = temp_db
            .prove_query(&path_query_three, None, grove_version)
            .unwrap()
            .unwrap();
        let (_, result_set_two) =
            GroveDb::verify_query_raw(proof.as_slice(), &path_query_three, grove_version)
                .expect("should execute proof");
        assert_eq!(result_set_two.len(), 2);

        #[rustfmt::skip]
        mod explanation {

    // Tree Structure
    //                                   root
    //              /                      |                       \ (not representing Merk)
    // -----------------------------------------------------------------------------------------
    //         test_leaf            another_test_leaf                deep_leaf
    //       /           \             /         \              /                 \
    // -----------------------------------------------------------------------------------------
    //   innertree     innertree4  innertree2  innertree3  deep_node_1          deep_node_2
    //       |             |           |           |      /          \         /         \
    // -----------------------------------------------------------------------------------------
    //      k2,v2        k4,v4       k3,v3      k4,v4   deeper_1   deeper_2  deeper_3   deeper_4
    //     /     \         |                           |            |         |          |
    //  k1,v1    k3,v3   k5,v5                        /            /          |          |
    // -----------------------------------------------------------------------------------------
    //                                            k2,v2         k5,v5        k8,v8     k10,v10
    //                                           /     \        /    \       /    \       \
    //                                       k1,v1    k3,v3  k4,v4   k6,v6 k7,v7  k9,v9  k11,v11
    //                                                            ↑ (all 3)   ↑     (all 2) ↑
    //                                                      path_query_one    ↑   path_query_two
    //                                                                 path_query_three (2)
    //                                                                   (after 7, so {8,9})

        }

        let merged_path_query = PathQuery::merge(
            vec![&path_query_one, &path_query_two, &path_query_three],
            grove_version,
        )
        .expect("expect to merge path queries");
        assert_eq!(merged_path_query.path, vec![b"deep_leaf".to_vec()]);
        assert_eq!(merged_path_query.query.query.items.len(), 2);
        let conditional_subquery_branches = merged_path_query
            .query
            .query
            .conditional_subquery_branches
            .clone()
            .expect("expected to have conditional subquery branches");
        assert_eq!(conditional_subquery_branches.len(), 2);
        let (deep_node_1_query_item, deep_node_1_subquery_branch) =
            conditional_subquery_branches.first().unwrap();
        let (deep_node_2_query_item, deep_node_2_subquery_branch) =
            conditional_subquery_branches.last().unwrap();
        assert_eq!(
            deep_node_1_query_item,
            &QueryItem::Key(b"deep_node_1".to_vec())
        );
        assert_eq!(
            deep_node_2_query_item,
            &QueryItem::Key(b"deep_node_2".to_vec())
        );

        assert_eq!(
            deep_node_1_subquery_branch
                .subquery_path
                .as_ref()
                .expect("expected a subquery_path for deep_node_1"),
            &vec![b"deeper_2".to_vec()]
        );
        assert_eq!(
            *deep_node_1_subquery_branch
                .subquery
                .as_ref()
                .expect("expected a subquery for deep_node_1"),
            Box::new(query_one)
        );

        assert!(
            deep_node_2_subquery_branch.subquery_path.is_none(),
            "there should be no subquery path here"
        );
        let deep_node_2_subquery = deep_node_2_subquery_branch
            .subquery
            .as_ref()
            .expect("expected a subquery for deep_node_2")
            .as_ref();

        assert_eq!(deep_node_2_subquery.items.len(), 2);

        let deep_node_2_conditional_subquery_branches = deep_node_2_subquery
            .conditional_subquery_branches
            .as_ref()
            .expect("expected to have conditional subquery branches");
        assert_eq!(deep_node_2_conditional_subquery_branches.len(), 2);

        // deeper 4 was query 2
        let (deeper_4_query_item, deeper_4_subquery_branch) =
            deep_node_2_conditional_subquery_branches.first().unwrap();
        let (deeper_3_query_item, deeper_3_subquery_branch) =
            deep_node_2_conditional_subquery_branches.last().unwrap();

        assert_eq!(deeper_3_query_item, &QueryItem::Key(b"deeper_3".to_vec()));
        assert_eq!(deeper_4_query_item, &QueryItem::Key(b"deeper_4".to_vec()));

        assert!(
            deeper_3_subquery_branch.subquery_path.is_none(),
            "there should be no subquery path here"
        );
        assert_eq!(
            *deeper_3_subquery_branch
                .subquery
                .as_ref()
                .expect("expected a subquery for deeper_3"),
            Box::new(query_three)
        );

        assert!(
            deeper_4_subquery_branch.subquery_path.is_none(),
            "there should be no subquery path here"
        );
        assert_eq!(
            *deeper_4_subquery_branch
                .subquery
                .as_ref()
                .expect("expected a subquery for deeper_4"),
            Box::new(query_two)
        );

        let (result_set_merged, _) = temp_db
            .query_raw(
                &merged_path_query,
                true,
                true,
                true,
                QueryResultType::QueryPathKeyElementTrioResultType,
                None,
                grove_version,
            )
            .value
            .expect("expected to get results");
        assert_eq!(result_set_merged.len(), 7);

        let proof = temp_db
            .prove_query(&merged_path_query, None, grove_version)
            .unwrap()
            .unwrap();
        let (_, proved_result_set_merged) =
            GroveDb::verify_query_raw(proof.as_slice(), &merged_path_query, grove_version)
                .expect("should execute proof");
        assert_eq!(proved_result_set_merged.len(), 7);

        let keys = [
            b"key4".to_vec(),
            b"key5".to_vec(),
            b"key6".to_vec(),
            b"key8".to_vec(),
            b"key9".to_vec(),
            b"key10".to_vec(),
            b"key11".to_vec(),
        ];
        let values = [
            b"value4".to_vec(),
            b"value5".to_vec(),
            b"value6".to_vec(),
            b"value8".to_vec(),
            b"value9".to_vec(),
            b"value10".to_vec(),
            b"value11".to_vec(),
        ];
        let elements = values.map(|x| Element::new_item(x).serialize(grove_version).unwrap());
        let expected_result_set: Vec<(Vec<u8>, Vec<u8>)> = keys.into_iter().zip(elements).collect();
        compare_result_tuples(proved_result_set_merged, expected_result_set);
    }

    #[test]
    fn test_different_length_paths_merge() {
        let grove_version = GroveVersion::latest();
        let temp_db = make_deep_tree(grove_version);

        let mut query_one = Query::new();
        query_one.insert_all();

        let mut subq = Query::new();
        subq.insert_all();
        query_one.set_subquery(subq);

        let path_query_one = PathQuery::new_unsized(
            vec![b"deep_leaf".to_vec(), b"deep_node_1".to_vec()],
            query_one,
        );

        let proof = temp_db
            .prove_query(&path_query_one, None, grove_version)
            .unwrap()
            .unwrap();
        let (_, result_set_one) =
            GroveDb::verify_query_raw(proof.as_slice(), &path_query_one, grove_version)
                .expect("should execute proof");
        assert_eq!(result_set_one.len(), 6);

        let mut query_two = Query::new();
        query_two.insert_all();

        let path_query_two = PathQuery::new_unsized(
            vec![
                b"deep_leaf".to_vec(),
                b"deep_node_2".to_vec(),
                b"deeper_4".to_vec(),
            ],
            query_two,
        );

        let proof = temp_db
            .prove_query(&path_query_two, None, grove_version)
            .unwrap()
            .unwrap();
        let (_, result_set_two) =
            GroveDb::verify_query_raw(proof.as_slice(), &path_query_two, grove_version)
                .expect("should execute proof");
        assert_eq!(result_set_two.len(), 2);

        let merged_path_query =
            PathQuery::merge(vec![&path_query_one, &path_query_two], grove_version)
                .expect("expect to merge path queries");
        assert_eq!(merged_path_query.path, vec![b"deep_leaf".to_vec()]);

        let proof = temp_db
            .prove_query(&merged_path_query, None, grove_version)
            .unwrap()
            .unwrap();
        let (_, result_set_merged) =
            GroveDb::verify_query_raw(proof.as_slice(), &merged_path_query, grove_version)
                .expect("should execute proof");
        assert_eq!(result_set_merged.len(), 8);

        let keys = [
            b"key1".to_vec(),
            b"key2".to_vec(),
            b"key3".to_vec(),
            b"key4".to_vec(),
            b"key5".to_vec(),
            b"key6".to_vec(),
            b"key10".to_vec(),
            b"key11".to_vec(),
        ];
        let values = [
            b"value1".to_vec(),
            b"value2".to_vec(),
            b"value3".to_vec(),
            b"value4".to_vec(),
            b"value5".to_vec(),
            b"value6".to_vec(),
            b"value10".to_vec(),
            b"value11".to_vec(),
        ];
        let elements = values.map(|x| Element::new_item(x).serialize(grove_version).unwrap());
        let expected_result_set: Vec<(Vec<u8>, Vec<u8>)> = keys.into_iter().zip(elements).collect();
        compare_result_tuples(result_set_merged, expected_result_set);
    }

    #[test]
    fn test_same_path_and_different_path_query_merge() {
        let grove_version = GroveVersion::latest();
        let temp_db = make_deep_tree(grove_version);

        let mut query_one = Query::new();
        query_one.insert_key(b"key1".to_vec());
        let path_query_one =
            PathQuery::new_unsized(vec![TEST_LEAF.to_vec(), b"innertree".to_vec()], query_one);

        let proof = temp_db
            .prove_query(&path_query_one, None, grove_version)
            .unwrap()
            .unwrap();
        let (_, result_set) =
            GroveDb::verify_query_raw(proof.as_slice(), &path_query_one, grove_version)
                .expect("should execute proof");
        assert_eq!(result_set.len(), 1);

        let mut query_two = Query::new();
        query_two.insert_key(b"key2".to_vec());
        let path_query_two =
            PathQuery::new_unsized(vec![TEST_LEAF.to_vec(), b"innertree".to_vec()], query_two);

        let proof = temp_db
            .prove_query(&path_query_two, None, grove_version)
            .unwrap()
            .unwrap();
        let (_, result_set) =
            GroveDb::verify_query_raw(proof.as_slice(), &path_query_two, grove_version)
                .expect("should execute proof");
        assert_eq!(result_set.len(), 1);

        let mut query_three = Query::new();
        query_three.insert_all();
        let path_query_three = PathQuery::new_unsized(
            vec![TEST_LEAF.to_vec(), b"innertree4".to_vec()],
            query_three,
        );

        let proof = temp_db
            .prove_query(&path_query_three, None, grove_version)
            .unwrap()
            .unwrap();
        let (_, result_set) =
            GroveDb::verify_query_raw(proof.as_slice(), &path_query_three, grove_version)
                .expect("should execute proof");
        assert_eq!(result_set.len(), 2);

        let merged_path_query = PathQuery::merge(
            vec![&path_query_one, &path_query_two, &path_query_three],
            grove_version,
        )
        .expect("should merge three queries");

        let proof = temp_db
            .prove_query(&merged_path_query, None, grove_version)
            .unwrap()
            .unwrap();
        let (_, result_set) =
            GroveDb::verify_query_raw(proof.as_slice(), &merged_path_query, grove_version)
                .expect("should execute proof");
        assert_eq!(result_set.len(), 4);
    }

    #[test]
    fn test_equal_path_merge() {
        let grove_version = GroveVersion::latest();
        // [a, b, Q]
        // [a, b, Q2]
        // We should be able to merge this if Q and Q2 have no subqueries.

        let temp_db = make_deep_tree(grove_version);

        let mut query_one = Query::new();
        query_one.insert_key(b"key1".to_vec());
        let path_query_one =
            PathQuery::new_unsized(vec![TEST_LEAF.to_vec(), b"innertree".to_vec()], query_one);

        let proof = temp_db
            .prove_query(&path_query_one, None, grove_version)
            .unwrap()
            .unwrap();
        let (_, result_set) =
            GroveDb::verify_query_raw(proof.as_slice(), &path_query_one, grove_version)
                .expect("should execute proof");
        assert_eq!(result_set.len(), 1);

        let mut query_two = Query::new();
        query_two.insert_key(b"key2".to_vec());
        let path_query_two =
            PathQuery::new_unsized(vec![TEST_LEAF.to_vec(), b"innertree".to_vec()], query_two);

        let proof = temp_db
            .prove_query(&path_query_two, None, grove_version)
            .unwrap()
            .unwrap();
        let (_, result_set) =
            GroveDb::verify_query_raw(proof.as_slice(), &path_query_two, grove_version)
                .expect("should execute proof");
        assert_eq!(result_set.len(), 1);

        let merged_path_query =
            PathQuery::merge(vec![&path_query_one, &path_query_two], grove_version)
                .expect("should merge three queries");

        let proof = temp_db
            .prove_query(&merged_path_query, None, grove_version)
            .unwrap()
            .unwrap();
        let (_, result_set) =
            GroveDb::verify_query_raw(proof.as_slice(), &merged_path_query, grove_version)
                .expect("should execute proof");
        assert_eq!(result_set.len(), 2);

        // [a, b, Q]
        // [a, b, c, Q2] (rolled up to) [a, b, Q3] where Q3 combines [c, Q2]
        // this should fail as [a, b] is a subpath of [a, b, c]
        let mut query_one = Query::new();
        query_one.insert_all();
        let path_query_one = PathQuery::new_unsized(
            vec![b"deep_leaf".to_vec(), b"deep_node_1".to_vec()],
            query_one,
        );

        let proof = temp_db
            .prove_query(&path_query_one, None, grove_version)
            .unwrap()
            .unwrap();
        let (_, result_set) =
            GroveDb::verify_query_raw(proof.as_slice(), &path_query_one, grove_version)
                .expect("should execute proof");
        assert_eq!(result_set.len(), 2);

        let mut query_one = Query::new();
        query_one.insert_key(b"deeper_1".to_vec());

        let mut subq = Query::new();
        subq.insert_all();
        query_one.set_subquery(subq.clone());

        let path_query_two = PathQuery::new_unsized(
            vec![b"deep_leaf".to_vec(), b"deep_node_1".to_vec()],
            query_one,
        );

        let proof = temp_db
            .prove_query(&path_query_two, None, grove_version)
            .unwrap()
            .unwrap();
        let (_, result_set) =
            GroveDb::verify_query_raw(proof.as_slice(), &path_query_two, grove_version)
                .expect("should execute proof");
        assert_eq!(result_set.len(), 3);

        #[rustfmt::skip]
        mod explanation {

    // Tree Structure
    //                                   root
    //              /                      |                       \ (not representing Merk)
    // -----------------------------------------------------------------------------------------
    //         test_leaf            another_test_leaf                deep_leaf
    //       /           \             /         \              /                 \
    // -----------------------------------------------------------------------------------------
    //   innertree     innertree4  innertree2  innertree3  deep_node_1          deep_node_2
    //       |             |           |           |      /          \         /         \
    // -----------------------------------------------------------------------------------------
    //      k2,v2        k4,v4       k3,v3      k4,v4   deeper_1   deeper_2  deeper_3   deeper_4
    //     /     \         |                           |   ↑  (2)  ↑ |         |          |
    //  k1,v1    k3,v3   k5,v5                        /path_query_1 /          |          |
    // -----------------------------------------------------------------------------------------
    //                                            k2,v2         k5,v5        k8,v8     k10,v10
    //                                           /     \        /    \       /    \       \
    //                                       k1,v1    k3,v3  k4,v4   k6,v6 k7,v7  k9,v9  k11,v11
    //                                            ↑ (3)
    //                                       path_query_2



        }

        let merged_path_query =
            PathQuery::merge(vec![&path_query_one, &path_query_two], grove_version)
                .expect("expected to be able to merge path_query");

        // we expect the common path to be the path of both before merge
        assert_eq!(
            merged_path_query.path,
            vec![b"deep_leaf".to_vec(), b"deep_node_1".to_vec()]
        );

        // we expect all items (a range full)
        assert_eq!(merged_path_query.query.query.items.len(), 1);
        assert!(merged_path_query
            .query
            .query
            .items
            .iter()
            .all(|a| a == &QueryItem::RangeFull(RangeFull)));

        // we expect a conditional subquery on deeper 1 for all elements
        let conditional_subquery_branches = merged_path_query
            .query
            .query
            .conditional_subquery_branches
            .as_ref()
            .expect("expected conditional subquery branches");

        assert_eq!(conditional_subquery_branches.len(), 1);
        let (conditional_query_item, conditional_subquery_branch) =
            conditional_subquery_branches.first().unwrap();
        assert_eq!(
            conditional_query_item,
            &QueryItem::Key(b"deeper_1".to_vec())
        );

        assert_eq!(conditional_subquery_branch.subquery, Some(Box::new(subq)));

        assert_eq!(conditional_subquery_branch.subquery_path, None);

        let (result_set_merged, _) = temp_db
            .query_raw(
                &merged_path_query,
                true,
                true,
                true,
                QueryResultType::QueryPathKeyElementTrioResultType,
                None,
                grove_version,
            )
            .value
            .expect("expected to get results");
        assert_eq!(result_set_merged.len(), 4);

        let proof = temp_db
            .prove_query(&merged_path_query, None, grove_version)
            .unwrap()
            .unwrap();
        let (_, result_set) =
            GroveDb::verify_query_raw(proof.as_slice(), &merged_path_query, grove_version)
                .expect("should execute proof");
        assert_eq!(result_set.len(), 4);
    }

    #[test]
    fn test_path_query_items_with_subquery_and_inner_subquery_path() {
        let grove_version = GroveVersion::latest();
        // Constructing the keys and paths
        let root_path_key_1 = b"root_path_key_1".to_vec();
        let root_path_key_2 = b"root_path_key_2".to_vec();
        let root_item_key = b"root_item_key".to_vec();
        let subquery_path_key_1 = b"subquery_path_key_1".to_vec();
        let subquery_path_key_2 = b"subquery_path_key_2".to_vec();
        let subquery_item_key = b"subquery_item_key".to_vec();
        let inner_subquery_path_key = b"inner_subquery_path_key".to_vec();

        // Constructing the subquery
        let subquery = Query {
            items: vec![QueryItem::Key(subquery_item_key.clone())],
            default_subquery_branch: SubqueryBranch {
                subquery_path: Some(vec![inner_subquery_path_key.clone()]),
                subquery: None,
            },
            left_to_right: true,
            conditional_subquery_branches: None,
            add_parent_tree_on_subquery: false,
        };

        // Constructing the PathQuery
        let path_query = PathQuery {
            path: vec![root_path_key_1.clone(), root_path_key_2.clone()],
            query: SizedQuery {
                query: Query {
                    items: vec![QueryItem::Key(root_item_key.clone())],
                    default_subquery_branch: SubqueryBranch {
                        subquery_path: Some(vec![
                            subquery_path_key_1.clone(),
                            subquery_path_key_2.clone(),
                        ]),
                        subquery: Some(Box::new(subquery)),
                    },
                    left_to_right: true,
                    conditional_subquery_branches: None,
                    add_parent_tree_on_subquery: false,
                },
                limit: Some(2),
                offset: None,
            },
        };

        {
            let path = vec![root_path_key_1.as_slice()];
            let first = path_query
                .query_items_at_path(&path, grove_version)
                .expect("expected valid version")
                .expect("expected query items");

            assert_eq!(
                first,
                SinglePathSubquery {
                    items: Cow::Owned(vec![QueryItem::Key(root_path_key_2.clone())]),
                    has_subquery: HasSubquery::NoSubquery,
                    left_to_right: true,
                    in_path: Some(Cow::Borrowed(&root_path_key_2)),
                }
            );
        }

        {
            let path = vec![root_path_key_1.as_slice(), root_path_key_2.as_slice()];

            let second = path_query
                .query_items_at_path(&path, grove_version)
                .expect("expected valid version")
                .expect("expected query items");

            assert_eq!(
                second,
                SinglePathSubquery {
                    items: Cow::Owned(vec![QueryItem::Key(root_item_key.clone())]),
                    has_subquery: HasSubquery::Always, /* This is correct because there's a
                                                        * subquery for one item */
                    left_to_right: true,
                    in_path: None,
                }
            );
        }

        {
            let path = vec![
                root_path_key_1.as_slice(),
                root_path_key_2.as_slice(),
                root_item_key.as_slice(),
            ];

            let third = path_query
                .query_items_at_path(&path, grove_version)
                .expect("expected valid version")
                .expect("expected query items");

            assert_eq!(
                third,
                SinglePathSubquery {
                    items: Cow::Owned(vec![QueryItem::Key(subquery_path_key_1.clone())]),
                    has_subquery: HasSubquery::NoSubquery,
                    left_to_right: true,
                    in_path: Some(Cow::Borrowed(&subquery_path_key_1))
                }
            );
        }

        {
            let path = vec![
                root_path_key_1.as_slice(),
                root_path_key_2.as_slice(),
                root_item_key.as_slice(),
                subquery_path_key_1.as_slice(),
            ];

            let fourth = path_query
                .query_items_at_path(&path, grove_version)
                .expect("expected valid version")
                .expect("expected query items");

            assert_eq!(
                fourth,
                SinglePathSubquery {
                    items: Cow::Owned(vec![QueryItem::Key(subquery_path_key_2.clone())]),
                    has_subquery: HasSubquery::NoSubquery,
                    left_to_right: true,
                    in_path: Some(Cow::Borrowed(&subquery_path_key_2))
                }
            );
        }

        {
            let path = vec![
                root_path_key_1.as_slice(),
                root_path_key_2.as_slice(),
                root_item_key.as_slice(),
                subquery_path_key_1.as_slice(),
                subquery_path_key_2.as_slice(),
            ];

            let fifth = path_query
                .query_items_at_path(&path, grove_version)
                .expect("expected valid version")
                .expect("expected query items");

            assert_eq!(
                fifth,
                SinglePathSubquery {
                    items: Cow::Owned(vec![QueryItem::Key(subquery_item_key.clone())]),
                    has_subquery: HasSubquery::Always, /* This means that we should be able to
                                                        * add items underneath */
                    left_to_right: true,
                    in_path: None,
                }
            );
        }

        {
            let path = vec![
                root_path_key_1.as_slice(),
                root_path_key_2.as_slice(),
                root_item_key.as_slice(),
                subquery_path_key_1.as_slice(),
                subquery_path_key_2.as_slice(),
                subquery_item_key.as_slice(),
            ];

            let sixth = path_query
                .query_items_at_path(&path, grove_version)
                .expect("expected valid version")
                .expect("expected query items");

            assert_eq!(
                sixth,
                SinglePathSubquery {
                    items: Cow::Owned(vec![QueryItem::Key(inner_subquery_path_key.clone())]),
                    has_subquery: HasSubquery::NoSubquery,
                    left_to_right: true,
                    in_path: None,
                }
            );
        }
    }

    #[test]
    fn test_path_query_items_with_subquery_path() {
        let grove_version = GroveVersion::latest();
        // Constructing the keys and paths
        let root_path_key = b"higher".to_vec();
        let dash_key = b"dash".to_vec();
        let quantum_key = b"quantum".to_vec();

        // Constructing the PathQuery
        let path_query = PathQuery {
            path: vec![root_path_key.clone()],
            query: SizedQuery {
                query: Query {
                    items: vec![QueryItem::RangeFull(RangeFull)],
                    default_subquery_branch: SubqueryBranch {
                        subquery_path: Some(vec![quantum_key.clone()]),
                        subquery: None,
                    },
                    left_to_right: true,
                    conditional_subquery_branches: None,
                    add_parent_tree_on_subquery: false,
                },
                limit: Some(100),
                offset: None,
            },
        };

        // Validating the PathQuery structure
        {
            let path = vec![root_path_key.as_slice()];
            let first = path_query
                .query_items_at_path(&path, grove_version)
                .expect("expected valid version")
                .expect("expected query items");

            assert_eq!(
                first,
                SinglePathSubquery {
                    items: Cow::Owned(vec![QueryItem::RangeFull(RangeFull)]),
                    has_subquery: HasSubquery::Always,
                    left_to_right: true,
                    in_path: None,
                }
            );
        }

        {
            let path = vec![root_path_key.as_slice(), dash_key.as_slice()];

            let second = path_query
                .query_items_at_path(&path, grove_version)
                .expect("expected valid version")
                .expect("expected query items");

            assert_eq!(
                second,
                SinglePathSubquery {
                    items: Cow::Owned(vec![QueryItem::Key(quantum_key.clone())]),
                    has_subquery: HasSubquery::NoSubquery,
                    left_to_right: true,
                    in_path: None, // There should be no path because we are at the end of the path
                }
            );
        }
    }

    #[test]
    fn test_conditional_subquery_refusing_elements() {
        let grove_version = GroveVersion::latest();
        let empty_vec: Vec<u8> = vec![];
        let zero_vec: Vec<u8> = vec![0];

        let mut conditional_subquery_branches = IndexMap::new();
        conditional_subquery_branches.insert(
            QueryItem::Key(b"".to_vec()),
            SubqueryBranch {
                subquery_path: Some(vec![zero_vec.clone()]),
                subquery: Some(Query::new().into()),
            },
        );

        let path_query = PathQuery {
            path: vec![TEST_LEAF.to_vec()],
            query: SizedQuery {
                query: Query {
                    items: vec![QueryItem::RangeFull(RangeFull)],
                    default_subquery_branch: SubqueryBranch {
                        subquery_path: Some(vec![zero_vec.clone()]),
                        subquery: None,
                    },
                    left_to_right: true,
                    conditional_subquery_branches: Some(conditional_subquery_branches),
                    add_parent_tree_on_subquery: false,
                },
                limit: Some(100),
                offset: None,
            },
        };

        {
            let path = vec![TEST_LEAF, empty_vec.as_slice()];

            let second = path_query
                .query_items_at_path(&path, grove_version)
                .expect("expected valid version")
                .expect("expected query items");

            assert_eq!(
                second,
                SinglePathSubquery {
                    items: Cow::Owned(vec![QueryItem::Key(zero_vec.clone())]),
                    has_subquery: HasSubquery::NoSubquery,
                    left_to_right: true,
                    in_path: Some(Cow::Borrowed(&zero_vec)),
                }
            );
        }
    }

    #[test]
    fn test_complex_path_query_with_conditional_subqueries() {
        let grove_version = GroveVersion::latest();
        let identity_id =
            hex::decode("8b8948a6801501bbe0431e3d994dcf71cf5a2a0939fe51b0e600076199aba4fb")
                .unwrap();

        let key_20 = vec![20u8];

        let key_80 = vec![80u8];

        let inner_conditional_subquery_branches = IndexMap::from([(
            QueryItem::Key(vec![80]),
            SubqueryBranch {
                subquery_path: None,
                subquery: Some(Box::new(Query {
                    items: vec![QueryItem::RangeFull(RangeFull)],
                    default_subquery_branch: SubqueryBranch {
                        subquery_path: None,
                        subquery: None,
                    },
                    left_to_right: true,
                    conditional_subquery_branches: None,
                    add_parent_tree_on_subquery: false,
                })),
            },
        )]);

        let conditional_subquery_branches = IndexMap::from([
            (
                QueryItem::Key(vec![]),
                SubqueryBranch {
                    subquery_path: None,
                    subquery: Some(Box::new(Query {
                        items: vec![QueryItem::Key(identity_id.to_vec())],
                        default_subquery_branch: SubqueryBranch {
                            subquery_path: None,
                            subquery: None,
                        },
                        left_to_right: true,
                        conditional_subquery_branches: None,
                        add_parent_tree_on_subquery: false,
                    })),
                },
            ),
            (
                QueryItem::Key(vec![20]),
                SubqueryBranch {
                    subquery_path: Some(vec![identity_id.to_vec()]),
                    subquery: Some(Box::new(Query {
                        items: vec![QueryItem::Key(vec![80]), QueryItem::Key(vec![0xc0])],
                        default_subquery_branch: SubqueryBranch {
                            subquery_path: None,
                            subquery: None,
                        },
                        conditional_subquery_branches: Some(
                            inner_conditional_subquery_branches.clone(),
                        ),
                        left_to_right: true,
                        add_parent_tree_on_subquery: false,
                    })),
                },
            ),
        ]);

        let path_query = PathQuery {
            path: vec![],
            query: SizedQuery {
                query: Query {
                    items: vec![QueryItem::Key(vec![20]), QueryItem::Key(vec![96])],
                    default_subquery_branch: SubqueryBranch {
                        subquery_path: None,
                        subquery: None,
                    },
                    conditional_subquery_branches: Some(conditional_subquery_branches.clone()),
                    left_to_right: true,
                    add_parent_tree_on_subquery: false,
                },
                limit: Some(100),
                offset: None,
            },
        };

        assert_eq!(path_query.max_depth(), Some(4));

        {
            let path = vec![];
            let first = path_query
                .query_items_at_path(&path, grove_version)
                .expect("expected valid version")
                .expect("expected query items");

            assert_eq!(
                first,
                SinglePathSubquery {
                    items: Cow::Owned(vec![QueryItem::Key(vec![20]), QueryItem::Key(vec![96]),]),
                    has_subquery: HasSubquery::Conditionally(Cow::Borrowed(
                        &conditional_subquery_branches
                    )),
                    left_to_right: true,
                    in_path: None,
                }
            );
        }

        {
            let path = vec![key_20.as_slice()];
            let query = path_query
                .query_items_at_path(&path, grove_version)
                .expect("expected valid version")
                .expect("expected query items");

            assert_eq!(
                query,
                SinglePathSubquery {
                    items: Cow::Owned(vec![QueryItem::Key(identity_id.clone()),]),
                    has_subquery: HasSubquery::NoSubquery,
                    left_to_right: true,
                    in_path: Some(Cow::Borrowed(&identity_id)),
                }
            );
        }

        {
            let path = vec![key_20.as_slice(), identity_id.as_slice()];
            let query = path_query
                .query_items_at_path(&path, grove_version)
                .expect("expected valid version")
                .expect("expected query items");

            assert_eq!(
                query,
                SinglePathSubquery {
                    items: Cow::Owned(vec![QueryItem::Key(vec![80]), QueryItem::Key(vec![0xc0]),]),
                    has_subquery: HasSubquery::Conditionally(Cow::Borrowed(
                        &inner_conditional_subquery_branches
                    )),
                    left_to_right: true,
                    in_path: None,
                }
            );
        }

        {
            let path = vec![key_20.as_slice(), identity_id.as_slice(), key_80.as_slice()];
            let query = path_query
                .query_items_at_path(&path, grove_version)
                .expect("expected valid version")
                .expect("expected query items");

            assert_eq!(
                query,
                SinglePathSubquery {
                    items: Cow::Owned(vec![QueryItem::RangeFull(RangeFull)]),
                    has_subquery: HasSubquery::NoSubquery,
                    left_to_right: true,
                    in_path: None,
                }
            );
        }
    }

    #[test]
    fn test_max_depth_limit() {
        /// Creates a `Query` with nested `SubqueryBranch` up to the specified
        /// depth non-recursively.
        fn create_non_recursive_query(subquery_depth: usize) -> Query {
            let mut root_query = Query::new_range_full();
            let mut current_query = &mut root_query;

            for _ in 0..subquery_depth {
                let new_query = Query::new_range_full();
                current_query.default_subquery_branch = SubqueryBranch {
                    subquery_path: None,
                    subquery: Some(Box::new(new_query)),
                };
                current_query = current_query
                    .default_subquery_branch
                    .subquery
                    .as_mut()
                    .unwrap();
            }

            root_query
        }

        let query = create_non_recursive_query(100);

        assert_eq!(query.max_depth(), Some(101));

        let query = create_non_recursive_query(500);

        assert_eq!(query.max_depth(), None);
    }

    #[test]
    fn test_simple_path_query_serialization() {
        let path_query = PathQuery {
            path: vec![b"root".to_vec(), b"subtree".to_vec()],
            query: SizedQuery {
                query: Query {
                    items: vec![QueryItem::Key(b"key1".to_vec())],
                    default_subquery_branch: SubqueryBranch::default(),
                    conditional_subquery_branches: None,
                    left_to_right: true,
                    add_parent_tree_on_subquery: false,
                },
                limit: None,
                offset: None,
            },
        };

        let encoded = encode_to_vec(&path_query, standard()).unwrap();
        let decoded: PathQuery = decode_from_slice(&encoded, standard()).unwrap().0;

        assert_eq!(path_query, decoded);
    }

    #[test]
    fn test_range_query_serialization() {
        let path_query = PathQuery {
            path: vec![b"root".to_vec()],
            query: SizedQuery {
                query: Query {
                    items: vec![QueryItem::Range(b"a".to_vec()..b"z".to_vec())],
                    default_subquery_branch: SubqueryBranch::default(),
                    conditional_subquery_branches: None,
                    left_to_right: false,
                    add_parent_tree_on_subquery: false,
                },
                limit: Some(10),
                offset: Some(2),
            },
        };

        let encoded = encode_to_vec(&path_query, standard()).unwrap();
        let decoded: PathQuery = decode_from_slice(&encoded, standard()).unwrap().0;

        assert_eq!(path_query, decoded);
    }

    #[test]
    fn test_range_inclusive_query_serialization() {
        let path_query = PathQuery {
            path: vec![b"root".to_vec()],
            query: SizedQuery {
                query: Query {
                    items: vec![QueryItem::RangeInclusive(b"a".to_vec()..=b"z".to_vec())],
                    default_subquery_branch: SubqueryBranch::default(),
                    conditional_subquery_branches: None,
                    left_to_right: true,
                    add_parent_tree_on_subquery: false,
                },
                limit: Some(5),
                offset: None,
            },
        };

        let encoded = encode_to_vec(&path_query, standard()).unwrap();
        let decoded: PathQuery = decode_from_slice(&encoded, standard()).unwrap().0;

        assert_eq!(path_query, decoded);
    }

    #[test]
    fn test_conditional_subquery_serialization() {
        let mut conditional_branches = IndexMap::new();
        conditional_branches.insert(
            QueryItem::Key(b"key1".to_vec()),
            SubqueryBranch {
                subquery_path: Some(vec![b"conditional_path".to_vec()]),
                subquery: Some(Box::new(Query::default())),
            },
        );

        let path_query = PathQuery {
            path: vec![b"root".to_vec()],
            query: SizedQuery {
                query: Query {
                    items: vec![QueryItem::Key(b"key1".to_vec())],
                    default_subquery_branch: SubqueryBranch::default(),
                    conditional_subquery_branches: Some(conditional_branches),
                    left_to_right: true,
                    add_parent_tree_on_subquery: false,
                },
                limit: None,
                offset: None,
            },
        };

        let encoded = encode_to_vec(&path_query, standard()).unwrap();
        let decoded: PathQuery = decode_from_slice(&encoded, standard()).unwrap().0;

        assert_eq!(path_query, decoded);
    }

    #[test]
    fn test_empty_path_query_serialization() {
        let path_query = PathQuery {
            path: vec![],
            query: SizedQuery {
                query: Query::default(),
                limit: None,
                offset: None,
            },
        };

        let encoded = encode_to_vec(&path_query, standard()).unwrap();
        let decoded: PathQuery = decode_from_slice(&encoded, standard()).unwrap().0;

        assert_eq!(path_query, decoded);
    }

    #[test]
    fn test_path_query_with_multiple_keys() {
        let path_query = PathQuery {
            path: vec![b"root".to_vec()],
            query: SizedQuery {
                query: Query {
                    items: vec![
                        QueryItem::Key(b"key1".to_vec()),
                        QueryItem::Key(b"key2".to_vec()),
                        QueryItem::Key(b"key3".to_vec()),
                    ],
                    default_subquery_branch: SubqueryBranch::default(),
                    conditional_subquery_branches: None,
                    left_to_right: true,
                    add_parent_tree_on_subquery: false,
                },
                limit: None,
                offset: None,
            },
        };

        let encoded = encode_to_vec(&path_query, standard()).unwrap();
        let decoded: PathQuery = decode_from_slice(&encoded, standard()).unwrap().0;

        assert_eq!(path_query, decoded);
    }

    #[test]
    fn test_path_query_with_full_range() {
        let path_query = PathQuery {
            path: vec![b"root".to_vec()],
            query: SizedQuery {
                query: Query {
                    items: vec![QueryItem::RangeFull(RangeFull)],
                    default_subquery_branch: SubqueryBranch::default(),
                    conditional_subquery_branches: None,
                    left_to_right: false,
                    add_parent_tree_on_subquery: false,
                },
                limit: Some(100),
                offset: Some(10),
            },
        };

        let encoded = encode_to_vec(&path_query, standard()).unwrap();
        let decoded: PathQuery = decode_from_slice(&encoded, standard()).unwrap().0;

        assert_eq!(path_query, decoded);
    }

    #[test]
    fn test_path_query_with_complex_conditions() {
        let mut conditional_branches = IndexMap::new();
        conditional_branches.insert(
            QueryItem::Key(b"key1".to_vec()),
            SubqueryBranch {
                subquery_path: Some(vec![b"conditional_path1".to_vec()]),
                subquery: Some(Box::new(Query {
                    items: vec![QueryItem::Range(b"a".to_vec()..b"m".to_vec())],
                    default_subquery_branch: SubqueryBranch::default(),
                    conditional_subquery_branches: None,
                    left_to_right: true,
                    add_parent_tree_on_subquery: false,
                })),
            },
        );
        conditional_branches.insert(
            QueryItem::Range(b"n".to_vec()..b"z".to_vec()),
            SubqueryBranch {
                subquery_path: Some(vec![b"conditional_path2".to_vec()]),
                subquery: Some(Box::new(Query {
                    items: vec![QueryItem::Key(b"key2".to_vec())],
                    default_subquery_branch: SubqueryBranch::default(),
                    conditional_subquery_branches: None,
                    left_to_right: false,
                    add_parent_tree_on_subquery: false,
                })),
            },
        );

        let path_query = PathQuery {
            path: vec![b"root".to_vec()],
            query: SizedQuery {
                query: Query {
                    items: vec![QueryItem::Key(b"key3".to_vec())],
                    default_subquery_branch: SubqueryBranch::default(),
                    conditional_subquery_branches: Some(conditional_branches),
                    left_to_right: true,
                    add_parent_tree_on_subquery: false,
                },
                limit: Some(50),
                offset: Some(5),
            },
        };

        let encoded = encode_to_vec(&path_query, standard()).unwrap();
        let decoded: PathQuery = decode_from_slice(&encoded, standard()).unwrap().0;

        assert_eq!(path_query, decoded);
    }

    #[test]
    fn test_path_query_with_subquery_path() {
        let path_query = PathQuery {
            path: vec![b"root".to_vec()],
            query: SizedQuery {
                query: Query {
                    items: vec![QueryItem::Key(b"key1".to_vec())],
                    default_subquery_branch: SubqueryBranch {
                        subquery_path: Some(vec![b"subtree_path".to_vec()]),
                        subquery: Some(Box::new(Query {
                            items: vec![QueryItem::Key(b"key2".to_vec())],
                            default_subquery_branch: SubqueryBranch::default(),
                            conditional_subquery_branches: None,
                            left_to_right: true,
                            add_parent_tree_on_subquery: false,
                        })),
                    },
                    conditional_subquery_branches: None,
                    left_to_right: true,
                    add_parent_tree_on_subquery: false,
                },
                limit: None,
                offset: None,
            },
        };

        let encoded = encode_to_vec(&path_query, standard()).unwrap();
        let decoded: PathQuery = decode_from_slice(&encoded, standard()).unwrap().0;

        assert_eq!(path_query, decoded);
    }

    #[test]
    fn test_path_query_with_empty_query_items() {
        let path_query = PathQuery {
            path: vec![b"root".to_vec()],
            query: SizedQuery {
                query: Query {
                    items: vec![], // No items in the query
                    default_subquery_branch: SubqueryBranch::default(),
                    conditional_subquery_branches: None,
                    left_to_right: true,
                    add_parent_tree_on_subquery: false,
                },
                limit: Some(20),
                offset: None,
            },
        };

        let encoded = encode_to_vec(&path_query, standard()).unwrap();
        let decoded: PathQuery = decode_from_slice(&encoded, standard()).unwrap().0;

        assert_eq!(path_query, decoded);
    }

    #[test]
    fn test_should_add_parent_tree_at_path_empty_path() {
        let grove_version = GroveVersion::latest();

        // Test with add_parent_tree_on_subquery = true
        let mut query = Query::new();
        query.add_parent_tree_on_subquery = true;
        let path_query = PathQuery::new_unsized(vec![], query);

        // Empty path should return the query's add_parent_tree_on_subquery value
        let result = path_query.should_add_parent_tree_at_path(&[], grove_version);
        assert_eq!(result.unwrap(), true);

        // Test with add_parent_tree_on_subquery = false
        let mut query = Query::new();
        query.add_parent_tree_on_subquery = false;
        let path_query = PathQuery::new_unsized(vec![], query);

        let result = path_query.should_add_parent_tree_at_path(&[], grove_version);
        assert_eq!(result.unwrap(), false);
    }

    #[test]
    fn test_should_add_parent_tree_at_path_exact_match() {
        let grove_version = GroveVersion::latest();

        let mut query = Query::new();
        query.add_parent_tree_on_subquery = true;
        let path_query = PathQuery::new_unsized(vec![b"root".to_vec(), b"subtree".to_vec()], query);

        // Exact path match
        let path = vec![b"root".as_ref(), b"subtree".as_ref()];
        let result = path_query.should_add_parent_tree_at_path(&path, grove_version);
        assert_eq!(result.unwrap(), true);

        // Different path of same length
        let path = vec![b"root".as_ref(), b"other".as_ref()];
        let result = path_query.should_add_parent_tree_at_path(&path, grove_version);
        assert_eq!(result.unwrap(), false);
    }

    #[test]
    fn test_should_add_parent_tree_at_path_shorter_path() {
        let grove_version = GroveVersion::latest();

        let mut query = Query::new();
        query.add_parent_tree_on_subquery = true;
        let path_query = PathQuery::new_unsized(
            vec![b"root".to_vec(), b"subtree".to_vec(), b"leaf".to_vec()],
            query,
        );

        // Shorter path should return false
        let path = vec![b"root".as_ref()];
        let result = path_query.should_add_parent_tree_at_path(&path, grove_version);
        assert_eq!(result.unwrap(), false);

        let path = vec![b"root".as_ref(), b"subtree".as_ref()];
        let result = path_query.should_add_parent_tree_at_path(&path, grove_version);
        assert_eq!(result.unwrap(), false);
    }

    #[test]
    fn test_should_add_parent_tree_at_path_with_subqueries() {
        let grove_version = GroveVersion::latest();

        // Create a nested query structure
        let mut inner_query = Query::new();
        inner_query.add_parent_tree_on_subquery = true;
        inner_query.insert_key(b"inner_key".to_vec());

        let mut query = Query::new();
        query.add_parent_tree_on_subquery = false;
        query.insert_key(b"key1".to_vec());
        query.default_subquery_branch = SubqueryBranch {
            subquery_path: Some(vec![b"subpath".to_vec()]),
            subquery: Some(Box::new(inner_query)),
        };

        let path_query = PathQuery::new_unsized(vec![b"root".to_vec()], query);

        // Test path leading to the inner query
        let path = vec![b"root".as_ref(), b"key1".as_ref(), b"subpath".as_ref()];
        let result = path_query.should_add_parent_tree_at_path(&path, grove_version);
        assert_eq!(result.unwrap(), true); // Should return inner query's value

        // Test root path
        let path = vec![b"root".as_ref()];
        let result = path_query.should_add_parent_tree_at_path(&path, grove_version);
        assert_eq!(result.unwrap(), false); // Should return root query's value
    }

    #[test]
    fn test_should_add_parent_tree_at_path_conditional_subqueries() {
        let grove_version = GroveVersion::latest();

        // Create conditional subqueries
        let mut conditional_branches = IndexMap::new();

        let mut branch1_query = Query::new();
        branch1_query.add_parent_tree_on_subquery = true;
        conditional_branches.insert(
            QueryItem::Key(b"branch1".to_vec()),
            SubqueryBranch {
                subquery_path: None,
                subquery: Some(Box::new(branch1_query)),
            },
        );

        let mut branch2_query = Query::new();
        branch2_query.add_parent_tree_on_subquery = false;
        conditional_branches.insert(
            QueryItem::Key(b"branch2".to_vec()),
            SubqueryBranch {
                subquery_path: Some(vec![b"nested".to_vec()]),
                subquery: Some(Box::new(branch2_query)),
            },
        );

        let mut query = Query::new();
        query.add_parent_tree_on_subquery = false;
        query.conditional_subquery_branches = Some(conditional_branches);

        let path_query = PathQuery::new_unsized(vec![b"root".to_vec()], query);

        // Test path to branch1
        let path = vec![b"root".as_ref(), b"branch1".as_ref()];
        let result = path_query.should_add_parent_tree_at_path(&path, grove_version);
        assert_eq!(result.unwrap(), true);

        // Test path to branch2 with nested path
        let path = vec![b"root".as_ref(), b"branch2".as_ref(), b"nested".as_ref()];
        let result = path_query.should_add_parent_tree_at_path(&path, grove_version);
        assert_eq!(result.unwrap(), false);
    }

    #[test]
    fn test_should_add_parent_tree_at_path_deep_nesting() {
        let grove_version = GroveVersion::latest();

        // Create deeply nested query structure
        let mut level3_query = Query::new();
        level3_query.add_parent_tree_on_subquery = true;

        let mut level2_query = Query::new();
        level2_query.add_parent_tree_on_subquery = false;
        level2_query.insert_key(b"level3".to_vec());
        level2_query.default_subquery_branch = SubqueryBranch {
            subquery_path: None,
            subquery: Some(Box::new(level3_query)),
        };

        let mut level1_query = Query::new();
        level1_query.add_parent_tree_on_subquery = false;
        level1_query.insert_key(b"level2".to_vec());
        level1_query.default_subquery_branch = SubqueryBranch {
            subquery_path: None,
            subquery: Some(Box::new(level2_query)),
        };

        let mut root_query = Query::new();
        root_query.add_parent_tree_on_subquery = false;
        root_query.insert_key(b"level1".to_vec());
        root_query.default_subquery_branch = SubqueryBranch {
            subquery_path: None,
            subquery: Some(Box::new(level1_query)),
        };

        let path_query = PathQuery::new_unsized(vec![b"root".to_vec()], root_query);

        // Test various depths
        let path = vec![b"root".as_ref()];
        let result = path_query.should_add_parent_tree_at_path(&path, grove_version);
        assert_eq!(result.unwrap(), false);

        let path = vec![b"root".as_ref(), b"level1".as_ref()];
        let result = path_query.should_add_parent_tree_at_path(&path, grove_version);
        assert_eq!(result.unwrap(), false);

        let path = vec![b"root".as_ref(), b"level1".as_ref(), b"level2".as_ref()];
        let result = path_query.should_add_parent_tree_at_path(&path, grove_version);
        assert_eq!(result.unwrap(), false);

        let path = vec![
            b"root".as_ref(),
            b"level1".as_ref(),
            b"level2".as_ref(),
            b"level3".as_ref(),
        ];
        let result = path_query.should_add_parent_tree_at_path(&path, grove_version);
        assert_eq!(result.unwrap(), true);
    }

    #[test]
    fn test_should_add_parent_tree_at_path_nonexistent_path() {
        let grove_version = GroveVersion::latest();

        let mut query = Query::new();
        query.add_parent_tree_on_subquery = true;
        query.insert_key(b"existing".to_vec());

        let path_query = PathQuery::new_unsized(vec![b"root".to_vec()], query);

        // Path that doesn't exist in the query structure
        let path = vec![b"root".as_ref(), b"nonexistent".as_ref()];
        let result = path_query.should_add_parent_tree_at_path(&path, grove_version);
        assert_eq!(result.unwrap(), false);

        // Longer path that doesn't match
        let path = vec![
            b"root".as_ref(),
            b"existing".as_ref(),
            b"but_no_subquery".as_ref(),
        ];
        let result = path_query.should_add_parent_tree_at_path(&path, grove_version);
        assert_eq!(result.unwrap(), false);
    }

    #[test]
    fn test_should_add_parent_tree_at_path_version_gating() {
        // Test with latest version
        let grove_version = GroveVersion::latest();

        let mut query = Query::new();
        query.add_parent_tree_on_subquery = true;
        let path_query = PathQuery::new_unsized(vec![b"root".to_vec()], query);

        let result = path_query.should_add_parent_tree_at_path(&[b"root".as_ref()], grove_version);
        assert!(result.is_ok());
        assert_eq!(result.unwrap(), true);

        // Test with mismatched path
        let result = path_query.should_add_parent_tree_at_path(&[], grove_version);
        assert!(result.is_ok());
        assert_eq!(result.unwrap(), false);
    }
}
