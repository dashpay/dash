//! Generate proof operations

use std::collections::BTreeMap;

use grovedb_costs::{
    cost_return_on_error, cost_return_on_error_default, cost_return_on_error_into,
    cost_return_on_error_no_add, CostResult, CostsExt, OperationCost,
};
use grovedb_merk::{
    proofs::{encode_into, query::QueryItem, Node, Op},
    tree::value_hash,
    Merk, ProofWithoutEncodingResult, TreeFeatureType,
};
use grovedb_storage::StorageContext;
use grovedb_version::{check_grovedb_v0_with_cost, version::GroveVersion};

#[cfg(feature = "proof_debug")]
use crate::query_result_type::QueryResultType;
use crate::{
    operations::proof::{
        util::hex_to_ascii, GroveDBProof, GroveDBProofV0, LayerProof, ProveOptions,
    },
    query::PathTrunkChunkQuery,
    reference_path::path_from_reference_path_type,
    Element, Error, GroveDb, PathQuery,
};

impl GroveDb {
    /// Prove one or more path queries.
    /// If we have more than one path query, we merge into a single path query
    /// before proving.
    pub fn prove_query_many(
        &self,
        query: Vec<&PathQuery>,
        prove_options: Option<ProveOptions>,
        grove_version: &GroveVersion,
    ) -> CostResult<Vec<u8>, Error> {
        check_grovedb_v0_with_cost!(
            "prove_query_many",
            grove_version
                .grovedb_versions
                .operations
                .proof
                .prove_query_many
        );
        if query.len() > 1 {
            let query = cost_return_on_error_default!(PathQuery::merge(query, grove_version));
            self.prove_query(&query, prove_options, grove_version)
        } else {
            self.prove_query(query[0], prove_options, grove_version)
        }
    }

    /// Generate a minimalistic proof for a given path query
    /// doesn't allow for subset verification
    /// Proofs generated with this can only be verified by the path query used
    /// to generate them.
    pub fn prove_query(
        &self,
        path_query: &PathQuery,
        prove_options: Option<ProveOptions>,
        grove_version: &GroveVersion,
    ) -> CostResult<Vec<u8>, Error> {
        check_grovedb_v0_with_cost!(
            "prove_query_many",
            grove_version.grovedb_versions.operations.proof.prove_query
        );
        let mut cost = OperationCost::default();
        let proof = cost_return_on_error!(
            &mut cost,
            self.prove_query_non_serialized(path_query, prove_options, grove_version)
        );
        #[cfg(feature = "proof_debug")]
        {
            println!("constructed proof is {}", proof);
        }
        let config = bincode::config::standard()
            .with_big_endian()
            .with_no_limit();
        let encoded_proof = cost_return_on_error_no_add!(
            cost,
            bincode::encode_to_vec(proof, config)
                .map_err(|e| Error::CorruptedData(format!("unable to encode proof {}", e)))
        );
        Ok(encoded_proof).wrap_with_cost(cost)
    }

    /// Generates a proof and does not serialize the result
    pub fn prove_query_non_serialized(
        &self,
        path_query: &PathQuery,
        prove_options: Option<ProveOptions>,
        grove_version: &GroveVersion,
    ) -> CostResult<GroveDBProof, Error> {
        let mut cost = OperationCost::default();

        let prove_options = prove_options.unwrap_or_default();

        if path_query.query.offset.is_some() && path_query.query.offset != Some(0) {
            return Err(Error::InvalidQuery(
                "proved path queries can not have offsets",
            ))
            .wrap_with_cost(cost);
        }

        if path_query.query.limit == Some(0) {
            return Err(Error::InvalidQuery(
                "proved path queries can not be for limit 0",
            ))
            .wrap_with_cost(cost);
        }

        #[cfg(feature = "proof_debug")]
        {
            // we want to query raw because we want the references to not be resolved at
            // this point

            let values = cost_return_on_error!(
                &mut cost,
                self.query_raw(
                    path_query,
                    false,
                    prove_options.decrease_limit_on_empty_sub_query_result,
                    false,
                    QueryResultType::QueryPathKeyElementTrioResultType,
                    None,
                    grove_version,
                )
            )
            .0;

            println!("values are {}", values);

            let precomputed_result_map = cost_return_on_error!(
                &mut cost,
                self.query_raw(
                    path_query,
                    false,
                    prove_options.decrease_limit_on_empty_sub_query_result,
                    false,
                    QueryResultType::QueryPathKeyElementTrioResultType,
                    None,
                    grove_version,
                )
            )
            .0
            .to_btree_map_level_results();

            println!("precomputed results are {}", precomputed_result_map);
        }

        let mut limit = path_query.query.limit;

        let root_layer = cost_return_on_error!(
            &mut cost,
            self.prove_subqueries(
                vec![],
                path_query,
                &mut limit,
                &prove_options,
                grove_version
            )
        );

        Ok(GroveDBProof::V0(GroveDBProofV0 {
            root_layer,
            prove_options,
        }))
        .wrap_with_cost(cost)
    }

    /// Perform a pre-order traversal of the tree based on the provided
    /// subqueries
    fn prove_subqueries(
        &self,
        path: Vec<&[u8]>,
        path_query: &PathQuery,
        overall_limit: &mut Option<u16>,
        prove_options: &ProveOptions,
        grove_version: &GroveVersion,
    ) -> CostResult<LayerProof, Error> {
        let mut cost = OperationCost::default();

        let tx = self.start_transaction();

        let query = cost_return_on_error_no_add!(
            cost,
            path_query
                .query_items_at_path(path.as_slice(), grove_version)
                .and_then(|query_items| {
                    query_items.ok_or(Error::CorruptedPath(format!(
                        "prove subqueries: path {} should be part of path_query {}",
                        path.iter()
                            .map(|a| hex_to_ascii(a))
                            .collect::<Vec<_>>()
                            .join("/"),
                        path_query
                    )))
                })
        );

        let subtree = cost_return_on_error!(
            &mut cost,
            self.open_transactional_merk_at_path(path.as_slice().into(), &tx, None, grove_version)
        );

        let limit = if path.len() < path_query.path.len() {
            // There is no need for a limit because we are only asking for a single item
            None
        } else {
            *overall_limit
        };

        let mut merk_proof = cost_return_on_error!(
            &mut cost,
            self.generate_merk_proof(
                &subtree,
                &query.items,
                query.left_to_right,
                limit,
                grove_version
            )
        );

        #[cfg(feature = "proof_debug")]
        {
            println!(
                "generated merk proof at level path level [{}], limit is {:?}, {}",
                path.iter()
                    .map(|a| hex_to_ascii(a))
                    .collect::<Vec<_>>()
                    .join("/"),
                overall_limit,
                if query.left_to_right {
                    "left to right"
                } else {
                    "right to left"
                }
            );
        }

        let mut lower_layers = BTreeMap::new();

        let mut has_a_result_at_level = false;
        let mut done_with_results = false;

        for op in merk_proof.proof.iter_mut() {
            done_with_results |= overall_limit == &Some(0);
            // Check if node should preserve its special type before destructuring
            // We need this flag to avoid converting it to Node::KV later
            // - KVValueHashFeatureType: used by ProvableCountTree for trees/references
            // - KVCount: used by ProvableCountTree for Items (tamper-resistant with count)
            let should_preserve_node_type = matches!(
                op,
                Op::Push(Node::KVValueHashFeatureType(..))
                    | Op::PushInverted(Node::KVValueHashFeatureType(..))
                    | Op::Push(Node::KVCount(..))
                    | Op::PushInverted(Node::KVCount(..))
            );
            // Extract count if present for ProvableCountTree references
            let count_for_ref = match op {
                Op::Push(Node::KVValueHashFeatureType(_, _, _, ft))
                | Op::PushInverted(Node::KVValueHashFeatureType(_, _, _, ft)) => match ft {
                    TreeFeatureType::ProvableCountedMerkNode(count) => Some(*count),
                    _ => None,
                },
                _ => None,
            };
            match op {
                Op::Push(node) | Op::PushInverted(node) => match node {
                    Node::KV(key, value)
                    | Node::KVValueHash(key, value, ..)
                    | Node::KVCount(key, value, _)
                    | Node::KVValueHashFeatureType(key, value, ..)
                        if !done_with_results =>
                    {
                        let elem = Element::deserialize(value, grove_version);
                        match elem {
                            Ok(Element::Reference(reference_path, ..)) => {
                                let absolute_path = cost_return_on_error_into!(
                                    &mut cost,
                                    path_from_reference_path_type(
                                        reference_path,
                                        &path.to_vec(),
                                        Some(key.as_slice())
                                    )
                                    .wrap_with_cost(OperationCost::default())
                                );

                                let referenced_elem = cost_return_on_error_into!(
                                    &mut cost,
                                    self.follow_reference(
                                        absolute_path.as_slice().into(),
                                        true,
                                        None,
                                        grove_version
                                    )
                                );

                                let serialized_referenced_elem =
                                    referenced_elem.serialize(grove_version);
                                if serialized_referenced_elem.is_err() {
                                    return Err(Error::CorruptedData(String::from(
                                        "unable to serialize element",
                                    )))
                                    .wrap_with_cost(cost);
                                }

                                // Use KVRefValueHashCount if in ProvableCountTree,
                                // otherwise use KVRefValueHash
                                *node = if let Some(count) = count_for_ref {
                                    Node::KVRefValueHashCount(
                                        key.to_owned(),
                                        serialized_referenced_elem.expect("confirmed ok above"),
                                        value_hash(value).unwrap_add_cost(&mut cost),
                                        count,
                                    )
                                } else {
                                    Node::KVRefValueHash(
                                        key.to_owned(),
                                        serialized_referenced_elem.expect("confirmed ok above"),
                                        value_hash(value).unwrap_add_cost(&mut cost),
                                    )
                                };
                                if let Some(limit) = overall_limit.as_mut() {
                                    *limit -= 1;
                                }
                                has_a_result_at_level |= true;
                            }
                            Ok(Element::Item(..)) if !done_with_results => {
                                #[cfg(feature = "proof_debug")]
                                {
                                    println!("found {}", hex_to_ascii(key));
                                }
                                // Only convert to Node::KV if not already a special node type
                                // - KVValueHashFeatureType: preserves feature_type for trees/refs
                                // - KVCount: preserves count for Items in ProvableCountTree
                                if !should_preserve_node_type {
                                    *node = Node::KV(key.to_owned(), value.to_owned());
                                }
                                if let Some(limit) = overall_limit.as_mut() {
                                    *limit -= 1;
                                }
                                has_a_result_at_level |= true;
                            }
                            Ok(Element::Tree(Some(_), _))
                            | Ok(Element::SumTree(Some(_), ..))
                            | Ok(Element::BigSumTree(Some(_), ..))
                            | Ok(Element::CountTree(Some(_), ..))
                            | Ok(Element::CountSumTree(Some(_), ..))
                            | Ok(Element::ProvableCountTree(Some(_), ..))
                            | Ok(Element::ProvableCountSumTree(Some(_), ..))
                                if !done_with_results
                                    && query.has_subquery_or_matching_in_path_on_key(key) =>
                            {
                                #[cfg(feature = "proof_debug")]
                                {
                                    println!(
                                        "found tree {}, query is {}",
                                        hex_to_ascii(key),
                                        query
                                    );
                                }
                                // We only want to check in sub nodes for the proof if the tree has
                                // elements
                                let mut lower_path = path.clone();
                                lower_path.push(key.as_slice());

                                let previous_limit = *overall_limit;

                                let layer_proof = cost_return_on_error!(
                                    &mut cost,
                                    self.prove_subqueries(
                                        lower_path,
                                        path_query,
                                        overall_limit,
                                        prove_options,
                                        grove_version,
                                    )
                                );

                                if previous_limit != *overall_limit {
                                    // a lower layer updated the limit, don't subtract 1 at this
                                    // level
                                    has_a_result_at_level |= true;
                                }
                                lower_layers.insert(key.clone(), layer_proof);
                            }

                            Ok(Element::Tree(..))
                            | Ok(Element::SumTree(..))
                            | Ok(Element::BigSumTree(..))
                            | Ok(Element::CountTree(..))
                            | Ok(Element::ProvableCountTree(..))
                            | Ok(Element::CountSumTree(..))
                            | Ok(Element::ProvableCountSumTree(..))
                                if !done_with_results =>
                            {
                                #[cfg(feature = "proof_debug")]
                                {
                                    println!(
                                        "found tree {}, no subquery query is {:?}",
                                        hex_to_ascii(key),
                                        query
                                    );
                                }
                                if let Some(limit) = overall_limit.as_mut() {
                                    *limit -= 1;
                                }
                                has_a_result_at_level |= true;
                            }
                            // todo: transform the unused trees into a Hash or KVHash to make proof
                            // smaller Ok(Element::Tree(..)) if
                            // done_with_results => {     *node =
                            // Node::Hash()     // we are done with the
                            // results, we can modify the proof to alter
                            // }
                            _ => continue,
                        }
                    }
                    _ => continue,
                },
                _ => continue,
            }
        }

        if !has_a_result_at_level
            && !done_with_results
            && prove_options.decrease_limit_on_empty_sub_query_result
        {
            #[cfg(feature = "proof_debug")]
            {
                println!(
                    "no results at level {}",
                    path.iter()
                        .map(|a| hex_to_ascii(a))
                        .collect::<Vec<_>>()
                        .join("/")
                );
            }
            if let Some(limit) = overall_limit.as_mut() {
                *limit -= 1;
            }
        }

        let mut serialized_merk_proof = Vec::with_capacity(1024);
        encode_into(merk_proof.proof.iter(), &mut serialized_merk_proof);

        Ok(LayerProof {
            merk_proof: serialized_merk_proof,
            lower_layers,
        })
        .wrap_with_cost(cost)
    }

    /// Generates query proof given a subtree and appends the result to a proof
    /// list
    fn generate_merk_proof<'a, S>(
        &self,
        subtree: &'a Merk<S>,
        query_items: &[QueryItem],
        left_to_right: bool,
        limit: Option<u16>,
        grove_version: &GroveVersion,
    ) -> CostResult<ProofWithoutEncodingResult, Error>
    where
        S: StorageContext<'a> + 'a,
    {
        subtree
            .prove_unchecked_query_items(query_items, limit, left_to_right, grove_version)
            .map_ok(|(proof, limit)| ProofWithoutEncodingResult::new(proof, limit))
            .map_err(|e| {
                Error::InternalError(format!(
                    "failed to generate proof for query_items [{}] error is : {}",
                    query_items
                        .iter()
                        .map(|e| e.to_string())
                        .collect::<Vec<_>>()
                        .join(", "),
                    e
                ))
            })
    }

    /// Generate a trunk chunk proof for a tree at the given path.
    ///
    /// This retrieves the top N levels of a count-based tree, returning a proof
    /// that can be verified to obtain a `TrunkQueryResult`.
    ///
    /// # Arguments
    /// * `query` - The path trunk chunk query containing the path and max_depth
    /// * `grove_version` - The grove version for compatibility
    ///
    /// # Returns
    /// A serialized `TrunkChunkProof` that can be verified
    pub fn prove_trunk_chunk(
        &self,
        query: &PathTrunkChunkQuery,
        grove_version: &GroveVersion,
    ) -> CostResult<Vec<u8>, Error> {
        let mut cost = OperationCost::default();

        let proof = cost_return_on_error!(
            &mut cost,
            self.prove_trunk_chunk_non_serialized(query, grove_version)
        );

        let config = bincode::config::standard()
            .with_big_endian()
            .with_no_limit();
        let encoded_proof = cost_return_on_error_no_add!(
            cost,
            bincode::encode_to_vec(proof, config)
                .map_err(|e| Error::CorruptedData(format!("unable to encode proof {}", e)))
        );

        Ok(encoded_proof).wrap_with_cost(cost)
    }

    /// Generate a trunk chunk proof without serializing.
    ///
    /// Returns a `GroveDBProof` with the standard `LayerProof` hierarchy.
    /// The path is navigated layer by layer, and at the target tree the
    /// merk_proof contains the trunk chunk proof (not a query proof).
    pub fn prove_trunk_chunk_non_serialized(
        &self,
        query: &PathTrunkChunkQuery,
        grove_version: &GroveVersion,
    ) -> CostResult<GroveDBProof, Error> {
        let mut cost = OperationCost::default();

        let tx = self.start_transaction();

        // Build the proof from the target tree back to the root
        // We collect proofs for each layer, then nest them
        let path_slices: Vec<&[u8]> = query.path.iter().map(|p| p.as_slice()).collect();

        // First, generate the trunk proof for the target tree
        let target_tree = cost_return_on_error!(
            &mut cost,
            self.open_transactional_merk_at_path(
                path_slices.as_slice().into(),
                &tx,
                None,
                grove_version
            )
        );

        // Perform the trunk query
        let trunk_result = cost_return_on_error!(
            &mut cost,
            target_tree
                .trunk_query(query.max_depth, query.min_depth, grove_version)
                .map_err(Error::MerkError)
        );

        // Encode the trunk proof ops
        let mut trunk_proof_encoded = Vec::new();
        encode_into(trunk_result.proof.iter(), &mut trunk_proof_encoded);

        // Start with the innermost LayerProof (the trunk proof at target tree)
        let mut current_layer = LayerProof {
            merk_proof: trunk_proof_encoded,
            lower_layers: BTreeMap::new(),
        };

        // Build nested LayerProofs from inside out (target -> root)
        for i in (0..query.path.len()).rev() {
            let current_path: Vec<&[u8]> = path_slices[..i].to_vec();
            let key = query.path[i].clone();

            // Open the merk at the current path
            let subtree = cost_return_on_error!(
                &mut cost,
                self.open_transactional_merk_at_path(
                    current_path.as_slice().into(),
                    &tx,
                    None,
                    grove_version
                )
            );

            // Generate a proof for the path segment key
            let query_item = QueryItem::Key(key.clone());
            let merk_proof = cost_return_on_error!(
                &mut cost,
                self.generate_merk_proof(&subtree, &[query_item], true, None, grove_version)
            );

            // Encode the merk proof
            let mut encoded_proof = Vec::new();
            encode_into(merk_proof.proof.iter(), &mut encoded_proof);

            // Create the new layer with the current layer as a lower layer
            let mut lower_layers = BTreeMap::new();
            lower_layers.insert(key, current_layer);

            current_layer = LayerProof {
                merk_proof: encoded_proof,
                lower_layers,
            };
        }

        Ok(GroveDBProof::V0(GroveDBProofV0 {
            root_layer: current_layer,
            prove_options: ProveOptions::default(),
        }))
        .wrap_with_cost(cost)
    }

    /// Generate a serialized branch chunk proof.
    ///
    /// Navigates to the specified key in the tree at the given path,
    /// then returns a proof of the subtree rooted at that key.
    /// The proof can be verified against the `Node::Hash` from a trunk query's
    /// terminal node.
    pub fn prove_branch_chunk(
        &self,
        query: &crate::query::PathBranchChunkQuery,
        grove_version: &GroveVersion,
    ) -> CostResult<Vec<u8>, Error> {
        let mut cost = OperationCost::default();

        let branch_result = cost_return_on_error!(
            &mut cost,
            self.prove_branch_chunk_non_serialized(query, grove_version)
        );

        // Encode just the proof ops - the verifier will execute them
        let mut encoded_proof = Vec::new();
        encode_into(branch_result.proof.iter(), &mut encoded_proof);

        Ok(encoded_proof).wrap_with_cost(cost)
    }

    /// Generate a branch chunk proof without serializing.
    ///
    /// Returns a `BranchQueryResult` containing the proof ops and branch root
    /// hash. The `branch_root_hash` should match a `Node::Hash` from the
    /// trunk query's terminal nodes.
    pub fn prove_branch_chunk_non_serialized(
        &self,
        query: &crate::query::PathBranchChunkQuery,
        grove_version: &GroveVersion,
    ) -> CostResult<grovedb_merk::BranchQueryResult, Error> {
        let mut cost = OperationCost::default();

        let tx = self.start_transaction();

        let path_slices: Vec<&[u8]> = query.path.iter().map(|p| p.as_slice()).collect();

        // Open the target tree and perform the branch query
        let target_tree = cost_return_on_error!(
            &mut cost,
            self.open_transactional_merk_at_path(
                path_slices.as_slice().into(),
                &tx,
                None,
                grove_version
            )
        );

        // Perform the branch query - returns BranchQueryResult directly
        let branch_result = cost_return_on_error!(
            &mut cost,
            target_tree
                .branch_query(&query.key, query.depth, grove_version)
                .map_err(Error::MerkError)
        );

        Ok(branch_result).wrap_with_cost(cost)
    }
}
