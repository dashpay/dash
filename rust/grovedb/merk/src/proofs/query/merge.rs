use indexmap::IndexMap;

use crate::proofs::{
    query::{
        common_path::CommonPathResult, query_item::QueryItem, QueryItemIntersectionResult,
        SubqueryBranch,
    },
    Query,
};

impl SubqueryBranch {
    fn merge_subquery(
        &self,
        other_default_branch_subquery: Option<Box<Query>>,
    ) -> Option<Box<Query>> {
        match (&self.subquery, other_default_branch_subquery) {
            (None, None) => None,
            (Some(subquery), None) => Some(subquery.clone()),
            (None, Some(subquery)) => Some(subquery),
            (Some(subquery), Some(other_subquery)) => {
                let mut merged_subquery = subquery.clone();
                merged_subquery.merge_with(*other_subquery);
                Some(merged_subquery)
            }
        }
    }

    pub fn merge(&self, other: &Self) -> Self {
        match (&self.subquery_path, &other.subquery_path) {
            (None, None) => {
                // they both just have subqueries without paths
                let subquery = self.merge_subquery(other.subquery.clone());
                SubqueryBranch {
                    subquery_path: None,
                    subquery,
                }
            }
            (Some(our_subquery_path), Some(their_subquery_path)) => {
                // They both have subquery paths

                if our_subquery_path.eq(their_subquery_path) {
                    // The subquery paths are the same
                    // We just need to merge the subqueries together
                    let subquery = self.merge_subquery(other.subquery.clone());
                    SubqueryBranch {
                        subquery_path: Some(our_subquery_path.clone()),
                        subquery,
                    }
                } else {
                    // We need to find the common path between the two subqueries
                    let CommonPathResult {
                        common_path,
                        mut left_path_leftovers,
                        mut right_path_leftovers,
                    } = CommonPathResult::from_paths(our_subquery_path, their_subquery_path);

                    let subquery_path = if common_path.is_empty() {
                        // There is no common path
                        // We set the subquery path to be None
                        None
                    } else {
                        // There is a common path
                        // We can use this common path as a common root
                        Some(common_path)
                    };

                    if !left_path_leftovers.is_empty() && !right_path_leftovers.is_empty() {
                        // Both left and right split but still have paths below them
                        // We take the top element from the left path leftovers and add a
                        // conditional subquery for each key

                        // We need to create a new subquery that will hold the conditional
                        // subqueries
                        let mut merged_query = Query::new();

                        // The key is also removed from the path as it is no needed in the subquery
                        let left_top_key = left_path_leftovers.remove(0);
                        let maybe_left_path_leftovers = if left_path_leftovers.is_empty() {
                            None
                        } else {
                            Some(left_path_leftovers)
                        };
                        merged_query.insert_key(left_top_key.clone());
                        merged_query.merge_conditional_boxed_subquery(
                            QueryItem::Key(left_top_key),
                            SubqueryBranch {
                                subquery_path: maybe_left_path_leftovers,
                                subquery: self.subquery.clone(),
                            },
                        );
                        let right_top_key = right_path_leftovers.remove(0);
                        let maybe_right_path_leftovers = if right_path_leftovers.is_empty() {
                            None
                        } else {
                            Some(right_path_leftovers)
                        };

                        merged_query.insert_key(right_top_key.clone());
                        merged_query.merge_conditional_boxed_subquery(
                            QueryItem::Key(right_top_key),
                            SubqueryBranch {
                                subquery_path: maybe_right_path_leftovers,
                                subquery: other.subquery.clone(),
                            },
                        );
                        SubqueryBranch {
                            subquery_path,
                            subquery: Some(Box::new(merged_query)),
                        }
                    } else if right_path_leftovers.is_empty() {
                        // this means our subquery path was longer
                        // which means we need to set the default to the right (other)
                        let mut merged_query = other.subquery.clone().unwrap_or_default();
                        let first_key = left_path_leftovers.remove(0);
                        let maybe_left_path_leftovers = if left_path_leftovers.is_empty() {
                            None
                        } else {
                            Some(left_path_leftovers)
                        };

                        merged_query.insert_key(first_key.clone());
                        // our subquery stays the same as we didn't change level
                        // add a conditional subquery for other
                        merged_query.merge_conditional_boxed_subquery(
                            QueryItem::Key(first_key),
                            SubqueryBranch {
                                subquery_path: maybe_left_path_leftovers,
                                subquery: self.subquery.clone(),
                            },
                        );
                        SubqueryBranch {
                            subquery_path,
                            subquery: Some(merged_query),
                        }
                    } else if left_path_leftovers.is_empty() {
                        let mut merged_query = self.subquery.clone().unwrap_or_default();
                        // this means our subquery path shorter
                        // we should keep our subquery
                        let other_first = right_path_leftovers.remove(0);

                        let maybe_right_path_leftovers = if right_path_leftovers.is_empty() {
                            None
                        } else {
                            Some(right_path_leftovers)
                        };
                        merged_query.insert_key(other_first.clone());
                        // our subquery stays the same as we didn't change level
                        // add a conditional subquery for other
                        merged_query.merge_conditional_boxed_subquery(
                            QueryItem::Key(other_first),
                            SubqueryBranch {
                                subquery_path: maybe_right_path_leftovers,
                                subquery: other.subquery.clone(),
                            },
                        );
                        SubqueryBranch {
                            subquery_path,
                            subquery: Some(merged_query),
                        }
                    } else {
                        unreachable!("Unreachable as both paths being equal already covered");
                    }
                }
            }
            (Some(our_subquery_path), None) => {
                // Ours has a subquery path, theirs does not.
                // We set the subquery path to None

                let mut our_subquery_path = our_subquery_path.clone();

                // take their subquery as it will be on a topmost layer
                let mut merged_subquery = other.subquery.clone().unwrap_or_default();

                // We need to add a conditional subquery for ours

                let our_top_key = our_subquery_path.remove(0);

                merged_subquery.insert_key(our_top_key.clone());

                let maybe_our_subquery_path = if our_subquery_path.is_empty() {
                    None
                } else {
                    Some(our_subquery_path)
                };
                // our subquery stays the same as we didn't change level
                // add a conditional subquery for other
                merged_subquery.merge_conditional_boxed_subquery(
                    // there are no conditional subquery branches yes
                    QueryItem::Key(our_top_key),
                    SubqueryBranch {
                        subquery_path: maybe_our_subquery_path,
                        subquery: self.subquery.clone(),
                    },
                );

                SubqueryBranch {
                    subquery_path: None,
                    subquery: Some(merged_subquery),
                }
            }
            (None, Some(their_subquery_path)) => {
                // They have a subquery path, we does not.
                // We set the subquery path to None

                let mut their_subquery_path = their_subquery_path.clone();

                // take our subquery as it will be on a topmost layer
                let mut merged_subquery = self.subquery.clone().unwrap_or_default();

                // The subquery_path is already set to None, no need to set it again

                let their_top_key = their_subquery_path.remove(0);

                merged_subquery.insert_key(their_top_key.clone());

                let maybe_their_subquery_path = if their_subquery_path.is_empty() {
                    None
                } else {
                    Some(their_subquery_path)
                };
                // their subquery stays the same as we didn't change level
                // add a conditional subquery for other
                merged_subquery.merge_conditional_boxed_subquery(
                    // there are no conditional subquery branches yes
                    QueryItem::Key(their_top_key),
                    SubqueryBranch {
                        subquery_path: maybe_their_subquery_path,
                        subquery: other.subquery.clone(),
                    },
                );

                SubqueryBranch {
                    subquery_path: None,
                    subquery: Some(merged_subquery),
                }
            }
        }
    }
}

#[cfg(any(feature = "minimal", feature = "verify"))]
impl Query {
    fn merge_default_subquerys_branch_subquery(
        &mut self,
        other_default_branch_subquery: Option<Box<Query>>,
    ) {
        if let Some(current_subquery) = self.default_subquery_branch.subquery.as_mut() {
            if let Some(other_subquery) = other_default_branch_subquery {
                current_subquery.merge_with(*other_subquery);
            }
        } else {
            // None existed yet
            self.default_subquery_branch.subquery = other_default_branch_subquery.clone();
        }
    }

    /// Merges the subquery for the query with the current subquery. Subqueries
    /// causes every element that is returned by the query to be subqueried
    /// or subqueried to the subquery_path/subquery if a subquery is
    /// present. Merging involves creating conditional subqueries in the
    /// subqueries subqueries and paths.
    pub fn merge_default_subquery_branch(&mut self, other_default_subquery_branch: SubqueryBranch) {
        match (
            &self.default_subquery_branch.subquery_path,
            &other_default_subquery_branch.subquery_path,
        ) {
            (None, None) => {
                // they both just have subqueries without paths
                self.merge_default_subquerys_branch_subquery(
                    other_default_subquery_branch.subquery,
                );
            }
            (Some(our_subquery_path), Some(their_subquery_path)) => {
                // They both have subquery paths

                if our_subquery_path.eq(their_subquery_path) {
                    // The subquery paths are the same
                    // We just need to merge the subqueries together
                    self.merge_default_subquerys_branch_subquery(
                        other_default_subquery_branch.subquery,
                    );
                } else {
                    // We need to find the common path between the two subqueries
                    let CommonPathResult {
                        common_path,
                        mut left_path_leftovers,
                        mut right_path_leftovers,
                    } = CommonPathResult::from_paths(our_subquery_path, their_subquery_path);

                    if common_path.is_empty() {
                        // There is no common path
                        // We set the subquery path to be None
                        self.default_subquery_branch.subquery_path = None;
                    } else {
                        // There is a common path
                        // We can use this common path as a common root
                        self.default_subquery_branch.subquery_path = Some(common_path)
                    }

                    if !left_path_leftovers.is_empty() && !right_path_leftovers.is_empty() {
                        // Both left and right split but still have paths below them
                        // We take the top element from the left path leftovers and add a
                        // conditional subquery for each key
                        // The key is also removed from the path as it is no needed in the subquery
                        let left_top_key = left_path_leftovers.remove(0);
                        let maybe_left_path_leftovers = if left_path_leftovers.is_empty() {
                            None
                        } else {
                            Some(left_path_leftovers)
                        };
                        self.merge_conditional_boxed_subquery(
                            QueryItem::Key(left_top_key),
                            SubqueryBranch {
                                subquery_path: maybe_left_path_leftovers,
                                subquery: self.default_subquery_branch.subquery.clone(),
                            },
                        );
                        let right_top_key = right_path_leftovers.remove(0);
                        let maybe_right_path_leftovers = if right_path_leftovers.is_empty() {
                            None
                        } else {
                            Some(right_path_leftovers)
                        };

                        self.merge_conditional_boxed_subquery(
                            QueryItem::Key(right_top_key),
                            SubqueryBranch {
                                subquery_path: maybe_right_path_leftovers,
                                subquery: other_default_subquery_branch.subquery.clone(),
                            },
                        );
                    } else if right_path_leftovers.is_empty() {
                        let left_subquery = self.default_subquery_branch.subquery.clone();
                        // this means our subquery path was longer
                        // which means we need to set the default to the right (other)
                        self.default_subquery_branch.subquery =
                            other_default_subquery_branch.subquery.clone();
                        let first_key = left_path_leftovers.remove(0);
                        let maybe_left_path_leftovers = if left_path_leftovers.is_empty() {
                            None
                        } else {
                            Some(left_path_leftovers)
                        };

                        // our subquery stays the same as we didn't change level
                        // add a conditional subquery for other
                        self.merge_conditional_boxed_subquery(
                            QueryItem::Key(first_key),
                            SubqueryBranch {
                                subquery_path: maybe_left_path_leftovers,
                                subquery: left_subquery,
                            },
                        );
                    } else if left_path_leftovers.is_empty() {
                        // this means our subquery path shorter
                        // we should keep our subquery
                        let other_first = right_path_leftovers.remove(0);

                        let maybe_right_path_leftovers = if right_path_leftovers.is_empty() {
                            None
                        } else {
                            Some(right_path_leftovers)
                        };
                        // our subquery stays the same as we didn't change level
                        // add a conditional subquery for other
                        self.merge_conditional_boxed_subquery(
                            QueryItem::Key(other_first),
                            SubqueryBranch {
                                subquery_path: maybe_right_path_leftovers,
                                subquery: other_default_subquery_branch.subquery.clone(),
                            },
                        );
                    } else {
                        unreachable!("Unreachable as both paths being equal already covered");
                    }
                }
            }
            (Some(our_subquery_path), None) => {
                // Ours has a subquery path, theirs does not.
                // We set the subquery path to None

                let mut our_subquery_path = our_subquery_path.clone();

                self.default_subquery_branch.subquery_path = None;
                self.default_subquery_branch.subquery =
                    other_default_subquery_branch.subquery.clone();
                // We need to add a conditional subquery for ours

                let our_top_key = our_subquery_path.remove(0);

                let maybe_our_subquery_path = if our_subquery_path.is_empty() {
                    None
                } else {
                    Some(our_subquery_path)
                };
                // our subquery stays the same as we didn't change level
                // add a conditional subquery for other
                self.merge_conditional_boxed_subquery(
                    QueryItem::Key(our_top_key),
                    SubqueryBranch {
                        subquery_path: maybe_our_subquery_path,
                        subquery: other_default_subquery_branch.subquery.clone(),
                    },
                );
            }
            (None, Some(their_subquery_path)) => {
                // They have a subquery path, we does not.
                // We set the subquery path to None

                let mut their_subquery_path = their_subquery_path.clone();

                // The subquery_path is already set to None, no need to set it again

                let their_top_key = their_subquery_path.remove(0);

                let maybe_their_subquery_path = if their_subquery_path.is_empty() {
                    None
                } else {
                    Some(their_subquery_path)
                };
                // our subquery stays the same as we didn't change level
                // add a conditional subquery for other
                self.merge_conditional_boxed_subquery(
                    QueryItem::Key(their_top_key),
                    SubqueryBranch {
                        subquery_path: maybe_their_subquery_path,
                        subquery: other_default_subquery_branch.subquery.clone(),
                    },
                );
            }
        }
    }

    pub fn merge_multiple(mut queries: Vec<Query>) -> Self {
        if queries.is_empty() {
            return Query::new();
        }
        // slight performance increase with swap remove as we don't care about the
        // ordering
        let mut merged_query = queries.swap_remove(0);
        for query in queries {
            let Query {
                mut items,
                default_subquery_branch,
                conditional_subquery_branches,
                ..
            } = query;
            // the searched for items are the union of all items
            merged_query.insert_items(items.clone());

            // // We now need to deal with subqueries
            // let QueryItemManyIntersectionResult{ in_both, ours, theirs } =
            // QueryItem::intersect_many_ordered(&mut merged_query.items, items);
            // // for the items that are in both we should set them to the merged subquery
            // branch
            //
            // // for the items that are in ours and theirs we should add conditional
            // subqueries if let Some(ours) = ours {
            //     for our_item in ours {
            //         merged_query
            //             .merge_conditional_boxed_subquery(our_item,
            // conditional_subquery_branch)     }
            // }
            //
            // if let Some(theirs) = theirs {
            //     for their_item in theirs {
            //         merged_query
            //             .merge_conditional_boxed_subquery(their_item,
            // conditional_subquery_branch)     }
            // }

            if let Some(conditional_subquery_branches) = conditional_subquery_branches {
                // if there are conditional subqueries
                // we need to remove from our items the conditional items

                for (conditional_item, conditional_subquery_branch) in conditional_subquery_branches
                {
                    merged_query.merge_conditional_boxed_subquery(
                        conditional_item.clone(),
                        conditional_subquery_branch,
                    );
                    if !items.is_empty() {
                        let intersection_result =
                            QueryItem::intersect_many_ordered(&mut items, vec![conditional_item]);
                        items = intersection_result.ours.unwrap_or_default();
                    }
                }
            }
            // if there are no conditional subquery items then things are easy
            // we create a conditional subquery item for all our items and add it to the
            // query
            for item in items {
                merged_query
                    .merge_conditional_boxed_subquery(item, default_subquery_branch.clone());
            }
        }
        merged_query
    }

    pub fn merge_with(&mut self, other: Query) {
        let Query {
            mut items,
            default_subquery_branch,
            conditional_subquery_branches,
            ..
        } = other;
        self.insert_items(items.clone());

        // let intersection_result = QueryItem::intersect_many_ordered(&mut self.items,
        // items); // merge query items as they point to the same context
        // for item in items {
        //     self.insert_item(item)
        // }

        // self.merge_default_subquery_branch(default_subquery_branch);
        if let Some(conditional_subquery_branches) = conditional_subquery_branches {
            for (conditional_item, conditional_subquery_branch) in conditional_subquery_branches {
                self.merge_conditional_boxed_subquery(
                    conditional_item.clone(),
                    conditional_subquery_branch,
                );

                if !items.is_empty() {
                    let intersection_result =
                        QueryItem::intersect_many_ordered(&mut items, vec![conditional_item]);
                    items = intersection_result.ours.unwrap_or_default();
                }
            }
        }
        for item in items {
            self.merge_conditional_boxed_subquery(item, default_subquery_branch.clone());
        }
    }

    /// Adds a conditional subquery. A conditional subquery replaces the default
    /// subquery and subquery_path if the item matches for the key. If
    /// multiple conditional subquery items match, then the first one that
    /// matches is used (in order that they were added).
    pub fn merge_conditional_boxed_subquery(
        &mut self,
        query_item_merging_in: QueryItem,
        subquery_branch_merging_in: SubqueryBranch,
    ) {
        if subquery_branch_merging_in.subquery.is_some()
            || subquery_branch_merging_in.subquery_path.is_some()
        {
            self.conditional_subquery_branches = Some(
                Self::merge_conditional_subquery_branches_with_new_at_query_item(
                    self.conditional_subquery_branches.take(),
                    query_item_merging_in,
                    subquery_branch_merging_in,
                ),
            );
        }
    }

    /// Adds a conditional subquery. A conditional subquery replaces the default
    /// subquery and subquery_path if the item matches for the key. If
    /// multiple conditional subquery items match, then the first one that
    /// matches is used (in order that they were added).
    pub fn merge_conditional_subquery_branches_with_new_at_query_item(
        conditional_subquery_branches: Option<IndexMap<QueryItem, SubqueryBranch>>,
        query_item_merging_in: QueryItem,
        subquery_branch_merging_in: SubqueryBranch,
    ) -> IndexMap<QueryItem, SubqueryBranch> {
        let mut merged_items: IndexMap<QueryItem, SubqueryBranch> = IndexMap::new();
        // first we need to check if there are already conditional subquery branches
        // because if there are none then we just assign the new conditional subquery
        // branch instead of merging it in
        if let Some(conditional_subquery_branches) = conditional_subquery_branches {
            // There were conditional subquery branches
            // We create a vector of the query item we are merging in
            // On the first loop this is a continuous query item (for example a range)
            // However as we find things that intersect with it, it might break
            // Example:
            // *On first pass:
            // **Current Subqueries:                   -----------------      --------
            // ----- **Conditional Subquery merging in:
            // ------------------------------------ **After first query:
            // --*****************----------------- We then feed back in
            // *On second pass:
            // **Current Subqueries:                   -----------------      --------
            // ----- **Conditional Subquery merging in:    --
            // ----------------- Lets say M is the one we are merging in and 1,
            // 2 and 3 are the previous conditional Suqueries
            // In the end we will have:              MM11111111111111111MMMMMM22222222MMM
            // 33333
            let mut sub_query_items_merging_in_vec = vec![query_item_merging_in];
            for (original_query_item, subquery_branch) in conditional_subquery_branches {
                let mut new_query_items_merging_in_vec = vec![];
                let mut hit = false;
                for sub_query_item_merging_in in sub_query_items_merging_in_vec {
                    let QueryItemIntersectionResult {
                        in_both,
                        ours_left,
                        ours_right,
                        theirs_left,
                        theirs_right,
                    } = original_query_item.intersect(&sub_query_item_merging_in);
                    if let Some(in_both) = in_both {
                        if !hit {
                            hit = true;
                        }
                        // merge the overlapping subquery branches
                        let merged_subquery_branch =
                            subquery_branch.merge(&subquery_branch_merging_in);
                        merged_items.insert(in_both, merged_subquery_branch);

                        match (ours_left, ours_right, theirs_left, theirs_right) {
                            (None, None, None, None) => {}
                            (Some(ours_left), None, None, None) => {
                                merged_items.insert(ours_left, subquery_branch.clone());
                            }
                            (None, Some(ours_right), None, None) => {
                                merged_items.insert(ours_right, subquery_branch.clone());
                            }
                            (Some(ours_left), Some(ours_right), None, None) => {
                                merged_items.insert(ours_left, subquery_branch.clone());
                                merged_items.insert(ours_right, subquery_branch.clone());
                            }
                            (None, None, Some(theirs_left), None) => {
                                new_query_items_merging_in_vec.push(theirs_left);
                            }
                            (Some(ours_left), None, Some(theirs_left), None) => {
                                merged_items.insert(ours_left, subquery_branch.clone());
                                new_query_items_merging_in_vec.push(theirs_left);
                            }
                            (None, Some(ours_right), Some(theirs_left), None) => {
                                merged_items.insert(ours_right, subquery_branch.clone());
                                new_query_items_merging_in_vec.push(theirs_left);
                            }
                            (Some(ours_left), Some(ours_right), Some(theirs_left), None) => {
                                merged_items.insert(ours_left, subquery_branch.clone());
                                merged_items.insert(ours_right, subquery_branch.clone());
                                new_query_items_merging_in_vec.push(theirs_left);
                            }
                            (None, None, None, Some(theirs_right)) => {
                                new_query_items_merging_in_vec.push(theirs_right);
                            }
                            (Some(ours_left), None, None, Some(theirs_right)) => {
                                merged_items.insert(ours_left, subquery_branch.clone());
                                new_query_items_merging_in_vec.push(theirs_right);
                            }
                            (None, Some(ours_right), None, Some(theirs_right)) => {
                                merged_items.insert(ours_right, subquery_branch.clone());
                                new_query_items_merging_in_vec.push(theirs_right);
                            }
                            (Some(ours_left), Some(ours_right), None, Some(theirs_right)) => {
                                merged_items.insert(ours_left, subquery_branch.clone());
                                merged_items.insert(ours_right, subquery_branch.clone());
                                new_query_items_merging_in_vec.push(theirs_right);
                            }
                            (None, None, Some(theirs_left), Some(theirs_right)) => {
                                new_query_items_merging_in_vec.push(theirs_left);
                                new_query_items_merging_in_vec.push(theirs_right);
                            }

                            (Some(ours_left), None, Some(theirs_left), Some(theirs_right)) => {
                                merged_items.insert(ours_left, subquery_branch.clone());
                                new_query_items_merging_in_vec.push(theirs_left);
                                new_query_items_merging_in_vec.push(theirs_right);
                            }
                            (None, Some(ours_right), Some(theirs_left), Some(theirs_right)) => {
                                merged_items.insert(ours_right, subquery_branch.clone());
                                new_query_items_merging_in_vec.push(theirs_left);
                                new_query_items_merging_in_vec.push(theirs_right);
                            }
                            (
                                Some(ours_left),
                                Some(ours_right),
                                Some(theirs_left),
                                Some(theirs_right),
                            ) => {
                                merged_items.insert(ours_left, subquery_branch.clone());
                                merged_items.insert(ours_right, subquery_branch.clone());
                                new_query_items_merging_in_vec.push(theirs_left);
                                new_query_items_merging_in_vec.push(theirs_right);
                            }
                        }
                    } else {
                        // there was no overlap
                        // re-add to merged_items
                        new_query_items_merging_in_vec.push(sub_query_item_merging_in);
                    }
                }
                if !hit {
                    merged_items.insert(original_query_item, subquery_branch.clone());
                }
                sub_query_items_merging_in_vec = new_query_items_merging_in_vec;
            }
            for split_item in sub_query_items_merging_in_vec {
                merged_items.insert(split_item, subquery_branch_merging_in.clone());
            }
        } else {
            merged_items.insert(query_item_merging_in, subquery_branch_merging_in);
        }
        merged_items
    }
}
