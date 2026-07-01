use std::collections::{BTreeMap, BTreeSet};

use grovedb_merk::{
    calculate_chunk_depths, calculate_max_tree_depth_from_count,
    element::tree_type::ElementTreeTypeExtensions,
    proofs::{
        execute,
        query::{PathKey, VerifyOptions},
        Decoder, Node, Op, Query,
    },
    tree::{combine_hash, value_hash},
    CryptoHash, TreeFeatureType,
};
use grovedb_version::{
    check_grovedb_v0, version::GroveVersion, TryFromVersioned, TryIntoVersioned,
};

#[cfg(feature = "proof_debug")]
use crate::operations::proof::util::{
    hex_to_ascii, path_as_slices_hex_to_ascii, path_hex_to_ascii,
};
use crate::{
    operations::proof::{
        util::{ProvedPathKeyOptionalValue, ProvedPathKeyValues},
        GroveDBProof, GroveDBProofV0, LayerProof, ProveOptions,
    },
    query::{GroveTrunkQueryResult, PathTrunkChunkQuery},
    query_result_type::PathKeyOptionalElementTrio,
    Element, Error, GroveDb, PathQuery,
};

impl GroveDb {
    pub fn verify_query_with_options(
        proof: &[u8],
        query: &PathQuery,
        options: VerifyOptions,
        grove_version: &GroveVersion,
    ) -> Result<(CryptoHash, Vec<PathKeyOptionalElementTrio>), Error> {
        check_grovedb_v0!(
            "verify_query_with_options",
            grove_version
                .grovedb_versions
                .operations
                .proof
                .verify_query_with_options
        );
        if options.absence_proofs_for_non_existing_searched_keys {
            // must have a limit
            query.query.limit.ok_or(Error::NotSupported(
                "limits must be set in verify_query_with_absence_proof".to_string(),
            ))?;
        }

        // must have no offset
        if query.query.offset.is_some() {
            return Err(Error::NotSupported(
                "offsets in path queries are not supported for proofs".to_string(),
            ));
        }

        let config = bincode::config::standard()
            .with_big_endian()
            .with_no_limit();
        let grovedb_proof: GroveDBProof = bincode::decode_from_slice(proof, config)
            .map_err(|e| Error::CorruptedData(format!("unable to decode proof: {}", e)))?
            .0;

        let (root_hash, _, result) =
            Self::verify_proof_internal(&grovedb_proof, query, options, grove_version)?;

        Ok((root_hash, result))
    }

    /// The point of this query is to get the parent tree information which will
    /// be present because we are querying in a subtree
    pub fn verify_query_get_parent_tree_info_with_options(
        proof: &[u8],
        query: &PathQuery,
        options: VerifyOptions,
        grove_version: &GroveVersion,
    ) -> Result<(CryptoHash, TreeFeatureType, Vec<PathKeyOptionalElementTrio>), Error> {
        check_grovedb_v0!(
            "verify_query_get_parent_tree_info_with_options",
            grove_version
                .grovedb_versions
                .operations
                .proof
                .verify_query_get_parent_tree_info_with_options
        );

        if query.query.query.has_subquery() {
            return Err(Error::NotSupported(
                "getting the parent tree info is not available when using subqueries".to_string(),
            ));
        }
        if options.absence_proofs_for_non_existing_searched_keys {
            // must have a limit
            query.query.limit.ok_or(Error::NotSupported(
                "limits must be set in verify_query_with_absence_proof".to_string(),
            ))?;
        }

        // must have no offset
        if query.query.offset.is_some() {
            return Err(Error::NotSupported(
                "offsets in path queries are not supported for proofs".to_string(),
            ));
        }

        let config = bincode::config::standard()
            .with_big_endian()
            .with_no_limit();
        let grovedb_proof: GroveDBProof = bincode::decode_from_slice(proof, config)
            .map_err(|e| Error::CorruptedData(format!("unable to decode proof: {}", e)))?
            .0;

        let (root_hash, tree_feature_type, result) =
            Self::verify_proof_internal(&grovedb_proof, query, options, grove_version)?;

        let tree_feature_type = tree_feature_type.ok_or(Error::InvalidProof(
            query.clone(),
            "query had no parent tree info, maybe it was for for root tree".to_string(),
        ))?;

        Ok((root_hash, tree_feature_type, result))
    }

    pub fn verify_query_raw(
        proof: &[u8],
        query: &PathQuery,
        grove_version: &GroveVersion,
    ) -> Result<(CryptoHash, ProvedPathKeyValues), Error> {
        check_grovedb_v0!(
            "verify_query_raw",
            grove_version
                .grovedb_versions
                .operations
                .proof
                .verify_query_raw
        );
        let config = bincode::config::standard()
            .with_big_endian()
            .with_no_limit();
        let grovedb_proof: GroveDBProof = bincode::decode_from_slice(proof, config)
            .map_err(|e| Error::CorruptedData(format!("unable to decode proof: {}", e)))?
            .0;

        let (root_hash, _, result) = Self::verify_proof_raw_internal(
            &grovedb_proof,
            query,
            VerifyOptions {
                absence_proofs_for_non_existing_searched_keys: false,
                verify_proof_succinctness: false,
                include_empty_trees_in_result: true,
            },
            grove_version,
        )?;

        Ok((root_hash, result))
    }

    pub(crate) fn verify_proof_internal(
        proof: &GroveDBProof,
        query: &PathQuery,
        options: VerifyOptions,
        grove_version: &GroveVersion,
    ) -> Result<
        (
            CryptoHash,
            Option<TreeFeatureType>,
            Vec<PathKeyOptionalElementTrio>,
        ),
        Error,
    > {
        match proof {
            GroveDBProof::V0(proof_v0) => {
                Self::verify_proof_v0_internal(proof_v0, query, options, grove_version)
            }
        }
    }

    fn verify_proof_v0_internal(
        proof: &GroveDBProofV0,
        query: &PathQuery,
        options: VerifyOptions,
        grove_version: &GroveVersion,
    ) -> Result<
        (
            CryptoHash,
            Option<TreeFeatureType>,
            Vec<PathKeyOptionalElementTrio>,
        ),
        Error,
    > {
        let mut result = Vec::new();
        let mut limit = query.query.limit;
        let mut last_tree_feature_type = None;
        let root_hash = Self::verify_layer_proof(
            &proof.root_layer,
            &proof.prove_options,
            query,
            &mut limit,
            &[],
            &mut result,
            &mut last_tree_feature_type,
            &options,
            grove_version,
        )?;

        if options.absence_proofs_for_non_existing_searched_keys {
            // must have a limit
            let max_results = query.query.limit.ok_or(Error::NotSupported(
                "limits must be set in verify_query_with_absence_proof".to_string(),
            ))? as usize;

            let terminal_keys = query.terminal_keys(max_results, grove_version)?;

            // convert the result set to a btree map
            let mut result_set_as_map: BTreeMap<PathKey, Option<Element>> = result
                .into_iter()
                .map(|(path, key, element)| ((path, key), element))
                .collect();
            #[cfg(feature = "proof_debug")]
            {
                println!(
                    "terminal keys are [{}] \n result set is [{}]",
                    terminal_keys
                        .iter()
                        .map(|(path, key)| format!(
                            "path: {} key: {}",
                            path_hex_to_ascii(path),
                            hex_to_ascii(key)
                        ))
                        .collect::<Vec<_>>()
                        .join(", "),
                    result_set_as_map
                        .iter()
                        .map(|((path, key), e)| {
                            let element_string = if let Some(e) = e {
                                e.to_string()
                            } else {
                                "None".to_string()
                            };
                            format!(
                                "path: {} key: {} element: {}",
                                path_hex_to_ascii(path),
                                hex_to_ascii(key),
                                element_string,
                            )
                        })
                        .collect::<Vec<_>>()
                        .join(", ")
                );
            }

            result = terminal_keys
                .into_iter()
                .map(|terminal_key| {
                    let element = result_set_as_map.remove(&terminal_key).flatten();
                    (terminal_key.0, terminal_key.1, element)
                })
                .collect();
        }

        Ok((root_hash, last_tree_feature_type, result))
    }

    pub(crate) fn verify_proof_raw_internal(
        proof: &GroveDBProof,
        query: &PathQuery,
        options: VerifyOptions,
        grove_version: &GroveVersion,
    ) -> Result<(CryptoHash, Option<TreeFeatureType>, ProvedPathKeyValues), Error> {
        match proof {
            GroveDBProof::V0(proof_v0) => {
                Self::verify_proof_raw_internal_v0(proof_v0, query, options, grove_version)
            }
        }
    }

    fn verify_proof_raw_internal_v0(
        proof: &GroveDBProofV0,
        query: &PathQuery,
        options: VerifyOptions,
        grove_version: &GroveVersion,
    ) -> Result<(CryptoHash, Option<TreeFeatureType>, ProvedPathKeyValues), Error> {
        let mut result = Vec::new();
        let mut limit = query.query.limit;
        let mut last_tree_feature_type = None;
        let root_hash = Self::verify_layer_proof(
            &proof.root_layer,
            &proof.prove_options,
            query,
            &mut limit,
            &[],
            &mut result,
            &mut last_tree_feature_type,
            &options,
            grove_version,
        )?;
        Ok((root_hash, last_tree_feature_type, result))
    }

    fn verify_layer_proof<T>(
        layer_proof: &LayerProof,
        prove_options: &ProveOptions,
        query: &PathQuery,
        limit_left: &mut Option<u16>,
        current_path: &[&[u8]],
        result: &mut Vec<T>,
        last_parent_tree_type: &mut Option<TreeFeatureType>,
        options: &VerifyOptions,
        grove_version: &GroveVersion,
    ) -> Result<CryptoHash, Error>
    where
        T: TryFromVersioned<ProvedPathKeyOptionalValue>,
        Error: From<<T as TryFromVersioned<ProvedPathKeyOptionalValue>>::Error>,
    {
        check_grovedb_v0!(
            "verify_layer_proof",
            grove_version
                .grovedb_versions
                .operations
                .proof
                .verify_layer_proof
        );
        let internal_query = query
            .query_items_at_path(current_path, grove_version)?
            .ok_or(Error::CorruptedPath(format!(
                "verify raw: path {} should be part of path_query {}",
                current_path
                    .iter()
                    .map(hex::encode)
                    .collect::<Vec<_>>()
                    .join("/"),
                query
            )))?;

        let level_query = Query {
            items: internal_query.items.to_vec(),
            left_to_right: internal_query.left_to_right,
            ..Default::default()
        };

        let (root_hash, merk_result) = level_query
            .execute_proof(
                &layer_proof.merk_proof,
                *limit_left,
                internal_query.left_to_right,
            )
            .unwrap()
            .map_err(|e| {
                eprintln!("{e}");
                Error::InvalidProof(
                    query.clone(),
                    format!("Invalid proof verification parameters: {}", e),
                )
            })?;

        #[cfg(feature = "proof_debug")]
        {
            println!(
                "\nDEBUG: Layer proof verification at path {:?}",
                current_path.iter().map(hex::encode).collect::<Vec<_>>()
            );
            println!("  Calculated root hash: {}", hex::encode(&root_hash));
            if let Some(parent_type) = last_parent_tree_type {
                println!("  Parent tree type: {:?}", parent_type);
            }
        }
        #[cfg(feature = "proof_debug")]
        {
            println!(
                "current path {} \n merk result is {}",
                path_as_slices_hex_to_ascii(current_path),
                merk_result
            );
        }

        let mut verified_keys = BTreeSet::new();

        if merk_result.result_set.is_empty() {
            if prove_options.decrease_limit_on_empty_sub_query_result {
                limit_left.iter_mut().for_each(|limit| *limit -= 1);
            }
        } else {
            for proved_key_value in merk_result.result_set {
                let mut path = current_path.to_vec();
                let key = &proved_key_value.key;
                let hash = &proved_key_value.proof;
                if let Some(value_bytes) = &proved_key_value.value {
                    let element = Element::deserialize(value_bytes, grove_version)?;

                    verified_keys.insert(key.clone());

                    if let Some(lower_layer) = layer_proof.lower_layers.get(key) {
                        #[cfg(feature = "proof_debug")]
                        {
                            println!("lower layer had key {}", hex_to_ascii(key));
                        }
                        match element {
                            Element::Tree(Some(_), _)
                            | Element::SumTree(Some(_), ..)
                            | Element::BigSumTree(Some(_), ..)
                            | Element::CountTree(Some(_), ..)
                            | Element::CountSumTree(Some(_), ..)
                            | Element::ProvableCountTree(Some(_), ..)
                            | Element::ProvableCountSumTree(Some(_), ..) => {
                                path.push(key);
                                *last_parent_tree_type = element.tree_feature_type();
                                if query.query_items_at_path(&path, grove_version)?.is_none() {
                                    // We are actually looking for the tree
                                    let path_key_optional_value =
                                        ProvedPathKeyOptionalValue::from_proved_key_value(
                                            path.iter().map(|p| p.to_vec()).collect(),
                                            proved_key_value,
                                        );
                                    #[cfg(feature = "proof_debug")]
                                    {
                                        println!(
                                            "pushing {} limit left after is {:?}",
                                            &path_key_optional_value, limit_left
                                        );
                                    }
                                    result.push(
                                        path_key_optional_value
                                            .try_into_versioned(grove_version)?,
                                    );

                                    limit_left.iter_mut().for_each(|limit| *limit -= 1);
                                    if limit_left == &Some(0) {
                                        break;
                                    }
                                } else {
                                    if query.should_add_parent_tree_at_path(
                                        current_path,
                                        grove_version,
                                    )? {
                                        let path_key_optional_value =
                                            ProvedPathKeyOptionalValue::from_proved_key_value(
                                                path.iter().map(|p| p.to_vec()).collect(),
                                                proved_key_value.clone(),
                                            );

                                        result.push(
                                            path_key_optional_value
                                                .try_into_versioned(grove_version)?,
                                        );
                                    }
                                    let lower_hash = Self::verify_layer_proof(
                                        lower_layer,
                                        prove_options,
                                        query,
                                        limit_left,
                                        &path,
                                        result,
                                        last_parent_tree_type,
                                        options,
                                        grove_version,
                                    )?;

                                    #[cfg(feature = "proof_debug")]
                                    {
                                        println!("\nDEBUG: Lower layer verification completed");
                                        println!(
                                            "  Path: {:?}",
                                            path.iter()
                                                .map(|p| hex_to_ascii(p))
                                                .collect::<Vec<_>>()
                                        );
                                        println!(
                                            "  Lower layer root hash: {}",
                                            hex::encode(&lower_hash)
                                        );
                                        println!("  Parent tree type: {:?}", last_parent_tree_type);
                                    }
                                    let combined_root_hash =
                                        combine_hash(value_hash(value_bytes).value(), &lower_hash)
                                            .value()
                                            .to_owned();

                                    #[cfg(feature = "proof_debug")]
                                    {
                                        println!("\nDEBUG: Tree element verification");
                                        println!("  Key: {}", hex_to_ascii(key));
                                        println!(
                                            "  Element type: {:?}",
                                            element.tree_feature_type()
                                        );
                                        println!("  Value bytes: {}", hex::encode(value_bytes));
                                        println!(
                                            "  Value bytes hash: {}",
                                            hex::encode(value_hash(value_bytes).value())
                                        );
                                        println!(
                                            "  Lower layer hash: {}",
                                            hex::encode(&lower_hash)
                                        );
                                        println!(
                                            "  Combined hash: {}",
                                            hex::encode(&combined_root_hash)
                                        );
                                        println!("  Expected hash: {}", hex::encode(hash));
                                    }
                                    if hash != &combined_root_hash {
                                        return Err(Error::InvalidProof(
                                            query.clone(),
                                            format!(
                                                "Mismatch in lower layer hash, expected {}, got {}",
                                                hex::encode(hash),
                                                hex::encode(combined_root_hash)
                                            ),
                                        ));
                                    }
                                    if limit_left == &Some(0) {
                                        break;
                                    }
                                }
                            }
                            Element::Tree(None, _)
                            | Element::SumTree(None, ..)
                            | Element::BigSumTree(None, ..)
                            | Element::CountTree(None, ..)
                            | Element::CountSumTree(None, ..)
                            | Element::ProvableCountTree(None, ..)
                            | Element::ProvableCountSumTree(None, ..)
                            | Element::SumItem(..)
                            | Element::Item(..)
                            | Element::ItemWithSumItem(..)
                            | Element::Reference(..) => {
                                return Err(Error::InvalidProof(
                                    query.clone(),
                                    "Proof has lower layer for a non Tree.".to_string(),
                                ));
                            }
                        }
                    } else if element.is_any_item()
                        || !internal_query.has_subquery_or_matching_in_path_on_key(key)
                            && (options.include_empty_trees_in_result
                                || !matches!(element, Element::Tree(None, _)))
                    {
                        let path_key_optional_value =
                            ProvedPathKeyOptionalValue::from_proved_key_value(
                                path.iter().map(|p| p.to_vec()).collect(),
                                proved_key_value,
                            );
                        #[cfg(feature = "proof_debug")]
                        {
                            println!(
                                "pushing {} limit left after is {:?}",
                                &path_key_optional_value, limit_left
                            );
                        }
                        result.push(path_key_optional_value.try_into_versioned(grove_version)?);

                        limit_left.iter_mut().for_each(|limit| *limit -= 1);
                        if limit_left == &Some(0) {
                            break;
                        }
                    } else {
                        #[cfg(feature = "proof_debug")]
                        {
                            println!(
                                "we have subquery on key {} with value {}: {}",
                                hex_to_ascii(key),
                                element,
                                level_query
                            )
                        }
                    }
                }
            }
        }

        Ok(root_hash)
    }

    pub fn verify_query(
        proof: &[u8],
        query: &PathQuery,
        grove_version: &GroveVersion,
    ) -> Result<(CryptoHash, Vec<PathKeyOptionalElementTrio>), Error> {
        check_grovedb_v0!(
            "verify_query",
            grove_version.grovedb_versions.operations.proof.verify_query
        );
        Self::verify_query_with_options(
            proof,
            query,
            VerifyOptions {
                absence_proofs_for_non_existing_searched_keys: false,
                verify_proof_succinctness: true,
                include_empty_trees_in_result: false,
            },
            grove_version,
        )
    }

    pub fn verify_subset_query(
        proof: &[u8],
        query: &PathQuery,
        grove_version: &GroveVersion,
    ) -> Result<(CryptoHash, Vec<PathKeyOptionalElementTrio>), Error> {
        check_grovedb_v0!(
            "verify_subset_query",
            grove_version
                .grovedb_versions
                .operations
                .proof
                .verify_subset_query
        );
        Self::verify_query_with_options(
            proof,
            query,
            VerifyOptions {
                absence_proofs_for_non_existing_searched_keys: false,
                verify_proof_succinctness: false,
                include_empty_trees_in_result: false,
            },
            grove_version,
        )
    }

    /// The point of this query is to get the parent tree information which will
    /// be present because we are querying in a subtree
    pub fn verify_query_get_parent_tree_info(
        proof: &[u8],
        query: &PathQuery,
        grove_version: &GroveVersion,
    ) -> Result<(CryptoHash, TreeFeatureType, Vec<PathKeyOptionalElementTrio>), Error> {
        check_grovedb_v0!(
            "verify_query_get_parent_tree_info",
            grove_version.grovedb_versions.operations.proof.verify_query
        );
        Self::verify_query_get_parent_tree_info_with_options(
            proof,
            query,
            VerifyOptions {
                absence_proofs_for_non_existing_searched_keys: false,
                verify_proof_succinctness: true,
                include_empty_trees_in_result: false,
            },
            grove_version,
        )
    }

    /// The point of this query is to get the parent tree information which will
    /// be present because we are querying in a subtree
    pub fn verify_subset_query_get_parent_tree_info(
        proof: &[u8],
        query: &PathQuery,
        grove_version: &GroveVersion,
    ) -> Result<(CryptoHash, TreeFeatureType, Vec<PathKeyOptionalElementTrio>), Error> {
        check_grovedb_v0!(
            "verify_subset_query_get_parent_tree_info",
            grove_version.grovedb_versions.operations.proof.verify_query
        );
        Self::verify_query_get_parent_tree_info_with_options(
            proof,
            query,
            VerifyOptions {
                absence_proofs_for_non_existing_searched_keys: false,
                verify_proof_succinctness: false,
                include_empty_trees_in_result: false,
            },
            grove_version,
        )
    }

    pub fn verify_query_with_absence_proof(
        proof: &[u8],
        query: &PathQuery,
        grove_version: &GroveVersion,
    ) -> Result<(CryptoHash, Vec<PathKeyOptionalElementTrio>), Error> {
        check_grovedb_v0!(
            "verify_query_with_absence_proof",
            grove_version
                .grovedb_versions
                .operations
                .proof
                .verify_query_with_absence_proof
        );
        Self::verify_query_with_options(
            proof,
            query,
            VerifyOptions {
                absence_proofs_for_non_existing_searched_keys: true,
                verify_proof_succinctness: true,
                include_empty_trees_in_result: false,
            },
            grove_version,
        )
    }

    pub fn verify_subset_query_with_absence_proof(
        proof: &[u8],
        query: &PathQuery,
        grove_version: &GroveVersion,
    ) -> Result<(CryptoHash, Vec<PathKeyOptionalElementTrio>), Error> {
        check_grovedb_v0!(
            "verify_subset_query_with_absence_proof",
            grove_version
                .grovedb_versions
                .operations
                .proof
                .verify_subset_query_with_absence_proof
        );
        Self::verify_query_with_options(
            proof,
            query,
            VerifyOptions {
                absence_proofs_for_non_existing_searched_keys: true,
                verify_proof_succinctness: false,
                include_empty_trees_in_result: false,
            },
            grove_version,
        )
    }

    /// Verify subset proof with a chain of path query functions.
    /// After subset verification with the first path query, the result if
    /// passed to the next path query generation function which generates a
    /// new path query Apply the new path query, and pass the result to the
    /// next ... This is useful for verifying proofs with multiple path
    /// queries that depend on one another.
    pub fn verify_query_with_chained_path_queries<C>(
        proof: &[u8],
        first_query: &PathQuery,
        chained_path_queries: Vec<C>,
        grove_version: &GroveVersion,
    ) -> Result<(CryptoHash, Vec<Vec<PathKeyOptionalElementTrio>>), Error>
    where
        C: Fn(Vec<PathKeyOptionalElementTrio>) -> Option<PathQuery>,
    {
        check_grovedb_v0!(
            "verify_query_with_chained_path_queries",
            grove_version
                .grovedb_versions
                .operations
                .proof
                .verify_query_with_chained_path_queries
        );
        let mut results = vec![];

        let (last_root_hash, elements) =
            Self::verify_subset_query(proof, first_query, grove_version)?;
        results.push(elements);

        // Process the chained path queries
        Self::process_chained_path_queries(
            proof,
            last_root_hash,
            chained_path_queries,
            grove_version,
            &mut results,
        )?;

        Ok((last_root_hash, results))
    }

    /// Processes each chained path query and verifies it.
    pub(in crate::operations::proof) fn process_chained_path_queries<C>(
        proof: &[u8],
        last_root_hash: CryptoHash,
        chained_path_queries: Vec<C>,
        grove_version: &GroveVersion,
        results: &mut Vec<Vec<PathKeyOptionalElementTrio>>,
    ) -> Result<(), Error>
    where
        C: Fn(Vec<PathKeyOptionalElementTrio>) -> Option<PathQuery>,
    {
        for path_query_generator in chained_path_queries {
            let new_path_query = path_query_generator(results[results.len() - 1].clone()).ok_or(
                Error::InvalidInput("one of the path query generators returns no path query"),
            )?;

            let (new_root_hash, new_elements) =
                Self::verify_subset_query(proof, &new_path_query, grove_version)?;

            if new_root_hash != last_root_hash {
                return Err(Error::InvalidProof(
                    new_path_query,
                    format!(
                        "Root hash for different path queries do not match, first is {}, this one \
                         is {}",
                        hex::encode(last_root_hash),
                        hex::encode(new_root_hash)
                    ),
                ));
            }

            results.push(new_elements);
        }

        Ok(())
    }

    /// Verifies a trunk chunk proof and returns a `GroveTrunkQueryResult`.
    ///
    /// This method verifies a proof generated by `prove_trunk_chunk`, walking
    /// through the path layers and verifying each one. At the target tree,
    /// it decodes and executes the trunk proof to extract the elements and
    /// leaf keys.
    ///
    /// # Arguments
    /// * `proof` - The serialized proof bytes
    /// * `query` - The path trunk chunk query (used to navigate the proof)
    /// * `grove_version` - The GroveDB version for element deserialization
    ///
    /// # Returns
    /// A tuple of:
    /// * `CryptoHash` - The root hash of the entire GroveDB
    /// * `GroveTrunkQueryResult` - The verified result with elements, leaf
    ///   keys, chunk depths, and tree depth
    pub fn verify_trunk_chunk_proof(
        proof: &[u8],
        query: &PathTrunkChunkQuery,
        grove_version: &GroveVersion,
    ) -> Result<(CryptoHash, GroveTrunkQueryResult), Error> {
        let config = bincode::config::standard()
            .with_big_endian()
            .with_no_limit();
        let grovedb_proof: GroveDBProof = bincode::decode_from_slice(proof, config)
            .map_err(|e| Error::CorruptedData(format!("unable to decode proof: {}", e)))?
            .0;

        match grovedb_proof {
            GroveDBProof::V0(proof_v0) => {
                Self::verify_trunk_chunk_proof_v0(&proof_v0, query, grove_version)
            }
        }
    }

    fn verify_trunk_chunk_proof_v0(
        proof: &GroveDBProofV0,
        query: &PathTrunkChunkQuery,
        grove_version: &GroveVersion,
    ) -> Result<(CryptoHash, GroveTrunkQueryResult), Error> {
        // Collect layer info as we walk down the path for later verification
        struct LayerInfo {
            value_bytes: Vec<u8>,
            expected_hash: CryptoHash,
            /// The root hash of this layer's merk tree (used as child hash for
            /// parent layer)
            layer_root_hash: CryptoHash,
        }
        let mut layer_infos: Vec<LayerInfo> = Vec::new();

        let mut current_layer = &proof.root_layer;
        let mut current_path: Vec<Vec<u8>> = Vec::new();
        let mut count: Option<u64> = None;
        let mut grovedb_root_hash: Option<CryptoHash> = None;

        // Walk through each path segment, verifying layer proofs
        for (i, path_segment) in query.path.iter().enumerate() {
            // Create a simple key query for this path segment
            let key_query = Query {
                items: vec![grovedb_merk::proofs::query::QueryItem::Key(
                    path_segment.clone(),
                )],
                ..Default::default()
            };

            // Execute the proof to verify and get the root hash
            let (layer_root_hash, result) = key_query
                .execute_proof(&current_layer.merk_proof, None, true)
                .unwrap()
                .map_err(|e| {
                    Error::InvalidProof(
                        PathQuery::new_unsized(current_path.clone(), key_query.clone()),
                        format!("Invalid proof at path layer {}: {}", i, e),
                    )
                })?;

            // Store the root hash of the first layer as the GroveDB root hash
            if i == 0 {
                grovedb_root_hash = Some(layer_root_hash);
            }

            // Find the element for this key in the result set
            let mut found_value_bytes: Option<Vec<u8>> = None;
            let mut found_hash: Option<CryptoHash> = None;

            for proved_key_value in &result.result_set {
                if proved_key_value.key == *path_segment {
                    found_hash = Some(proved_key_value.proof);
                    if let Some(value_bytes) = &proved_key_value.value {
                        found_value_bytes = Some(value_bytes.clone());

                        // On the last path segment, extract the count from the CountTree element
                        if i == query.path.len() - 1 {
                            let element = Element::deserialize(value_bytes, grove_version)?;
                            count = Self::extract_count_from_element(&element);
                        }
                    }
                    break;
                }
            }

            let value_bytes = found_value_bytes.ok_or_else(|| {
                Error::InvalidProof(
                    PathQuery::new_unsized(current_path.clone(), key_query.clone()),
                    format!(
                        "Path segment {} not found in proof result",
                        hex::encode(path_segment)
                    ),
                )
            })?;

            let expected_hash = found_hash.ok_or_else(|| {
                Error::InvalidProof(
                    PathQuery::new_unsized(current_path.clone(), key_query.clone()),
                    format!(
                        "No hash found for path segment {}",
                        hex::encode(path_segment)
                    ),
                )
            })?;

            // Store layer info for later verification
            layer_infos.push(LayerInfo {
                value_bytes,
                expected_hash,
                layer_root_hash,
            });

            // Move to the next layer
            current_layer = current_layer
                .lower_layers
                .get(path_segment)
                .ok_or_else(|| {
                    Error::InvalidProof(
                        PathQuery::new_unsized(current_path.clone(), key_query),
                        format!(
                            "Missing lower layer for path segment {}",
                            hex::encode(path_segment)
                        ),
                    )
                })?;

            current_path.push(path_segment.clone());
        }

        // Ensure we got a count from the element
        let count = count.ok_or_else(|| {
            Error::InvalidProof(
                PathQuery::new_unsized(current_path.clone(), Query::default()),
                "Could not extract count from path - target is not a count tree element"
                    .to_string(),
            )
        })?;

        let tree_depth = calculate_max_tree_depth_from_count(count);
        let chunk_depths = calculate_chunk_depths(tree_depth, query.max_depth);

        // Now we're at the target layer - decode and execute the trunk proof
        let decoder = Decoder::new(&current_layer.merk_proof);
        let ops: Vec<Op> = decoder
            .collect::<Result<Vec<_>, _>>()
            .map_err(|e| Error::CorruptedData(format!("Failed to decode trunk proof: {}", e)))?;

        // Execute the proof to build the tree structure and get its root hash
        // Use collapse=false to preserve the full tree structure for element extraction
        let target_tree = execute(ops.iter().map(|op| Ok(op.clone())), false, |_| Ok(()))
            .unwrap()
            .map_err(|e| {
                Error::InvalidProof(
                    PathQuery::new_unsized(current_path.clone(), Query::default()),
                    format!("Failed to execute trunk proof: {}", e),
                )
            })?;

        let mut lower_hash = target_tree.hash().unwrap();

        // Verify the cryptographic chain from trunk up through all layers
        // Walk backwards through the layer infos, verifying each step
        for (i, layer_info) in layer_infos.iter().rev().enumerate() {
            let combined_hash =
                combine_hash(value_hash(&layer_info.value_bytes).value(), &lower_hash)
                    .value()
                    .to_owned();

            if combined_hash != layer_info.expected_hash {
                return Err(Error::InvalidProof(
                    PathQuery::new_unsized(current_path.clone(), Query::default()),
                    format!(
                        "Hash mismatch at layer {} from bottom: expected {}, got {}",
                        i,
                        hex::encode(layer_info.expected_hash),
                        hex::encode(combined_hash)
                    ),
                ));
            }

            // For the next iteration, use this layer's merk tree root hash.
            // This is what the parent layer uses as the child hash for this subtree.
            lower_hash = layer_info.layer_root_hash;
        }

        // Extract elements and leaf keys from the proof tree
        // The max depth for the trunk is the first chunk depth
        let max_depth = chunk_depths.first().copied().unwrap_or(0) as usize;
        let mut elements = BTreeMap::new();
        let mut leaf_keys = BTreeMap::new();
        Self::extract_elements_and_leaf_keys(
            &target_tree,
            &mut elements,
            &mut leaf_keys,
            0,
            max_depth,
            grove_version,
        )?;

        let grovedb_root_hash = grovedb_root_hash.ok_or_else(|| {
            Error::InvalidProof(
                PathQuery::new_unsized(Vec::new(), Query::default()),
                "Empty path - no root hash computed".to_string(),
            )
        })?;

        let trunk_result = GroveTrunkQueryResult {
            elements,
            leaf_keys,
            chunk_depths,
            max_tree_depth: tree_depth,
            tree: target_tree,
        };

        Ok((grovedb_root_hash, trunk_result))
    }

    /// Recursively extract elements and leaf keys from a proof tree.
    ///
    /// Elements are nodes with key-value data that can be deserialized.
    /// Leaf keys are nodes that have at least one `Node::Hash` child, mapped
    /// to their LeafInfo (hash + optional count for branch verification).
    ///
    /// # Arguments
    /// * `tree` - The proof tree to extract from
    /// * `elements` - Output map of key -> Element
    /// * `leaf_keys` - Output map of key -> LeafInfo (hash + count)
    /// * `current_depth` - Current depth in the tree (0 = root)
    /// * `max_depth` - Maximum allowed depth (nodes beyond this should be Hash)
    /// * `grove_version` - Version for Element deserialization
    fn extract_elements_and_leaf_keys(
        tree: &grovedb_merk::proofs::tree::Tree,
        elements: &mut BTreeMap<Vec<u8>, Element>,
        leaf_keys: &mut BTreeMap<Vec<u8>, crate::query::LeafInfo>,
        current_depth: usize,
        max_depth: usize,
        grove_version: &GroveVersion,
    ) -> Result<(), Error> {
        // Check that we haven't exceeded the max depth
        if current_depth > max_depth {
            return Err(Error::InvalidProof(
                PathQuery::new_unsized(Vec::new(), Query::default()),
                format!(
                    "Trunk proof exceeds max depth: current depth {} > max depth {}",
                    current_depth, max_depth
                ),
            ));
        }

        // Check for inconsistent depth: if one child is Hash and the other exists
        // but is not Hash, the proof has inconsistent truncation depth
        let left_is_hash = tree
            .left
            .as_ref()
            .map(|c| matches!(c.tree.node, Node::Hash(_)));
        let right_is_hash = tree
            .right
            .as_ref()
            .map(|c| matches!(c.tree.node, Node::Hash(_)));

        // If both children exist, they must both be Hash or both be non-Hash
        if let (Some(left_hash), Some(right_hash)) = (left_is_hash, right_is_hash) {
            if left_hash != right_hash {
                return Err(Error::InvalidProof(
                    PathQuery::new_unsized(Vec::new(), Query::default()),
                    "Inconsistent trunk proof depth: one child is Hash while the other is not"
                        .to_string(),
                ));
            }
        }

        // Extract key and value from this node - must exist for valid trunk proofs
        let (key, value) = Self::get_key_value_from_node(&tree.node).ok_or_else(|| {
            Error::InvalidProof(
                PathQuery::new_unsized(Vec::new(), Query::default()),
                format!(
                    "Trunk proof contains node without key/value data: {:?}",
                    tree.node
                ),
            )
        })?;

        let element = Element::deserialize(&value, grove_version)?;
        elements.insert(key.clone(), element);

        // Check if this node has Hash children (making it a leaf)
        let has_hash_child = left_is_hash.unwrap_or(false) || right_is_hash.unwrap_or(false);

        if has_hash_child {
            // Store the node's hash and count as LeafInfo for branch queries.
            // When a branch query is made for this key, the branch proof's root hash
            // should match this node's hash.
            let node_hash = tree.hash().unwrap();

            // Extract count from TreeFeatureType if available
            // Note: KVHashCount is not included as it never reaches this code path
            let count = match &tree.node {
                Node::KVValueHashFeatureType(_, _, _, feature_type) => feature_type.count(),
                Node::KVCount(_, _, count) => Some(*count),
                Node::KVRefValueHashCount(_, _, _, count) => Some(*count),
                _ => None,
            };

            leaf_keys.insert(
                key,
                crate::query::LeafInfo {
                    hash: node_hash,
                    count,
                },
            );
        }

        // Recurse into non-Hash children
        if let Some(left) = &tree.left {
            if !matches!(left.tree.node, Node::Hash(_)) {
                Self::extract_elements_and_leaf_keys(
                    &left.tree,
                    elements,
                    leaf_keys,
                    current_depth + 1,
                    max_depth,
                    grove_version,
                )?;
            }
        }
        if let Some(right) = &tree.right {
            if !matches!(right.tree.node, Node::Hash(_)) {
                Self::extract_elements_and_leaf_keys(
                    &right.tree,
                    elements,
                    leaf_keys,
                    current_depth + 1,
                    max_depth,
                    grove_version,
                )?;
            }
        }

        Ok(())
    }

    /// Extract key and value from a node if it has both.
    fn get_key_value_from_node(node: &Node) -> Option<(Vec<u8>, Vec<u8>)> {
        match node {
            Node::KV(key, value)
            | Node::KVValueHash(key, value, ..)
            | Node::KVValueHashFeatureType(key, value, ..)
            | Node::KVCount(key, value, ..)
            | Node::KVRefValueHash(key, value, ..)
            | Node::KVRefValueHashCount(key, value, ..) => Some((key.clone(), value.clone())),
            // These nodes don't have values, only key+hash or just hash
            Node::KVDigest(..)
            | Node::KVDigestCount(..)
            | Node::Hash(_)
            | Node::KVHash(_)
            | Node::KVHashCount(..) => None,
        }
    }

    /// Extract the count from a CountTree, CountSumTree, ProvableCountTree,
    /// or ProvableCountSumTree element.
    fn extract_count_from_element(element: &Element) -> Option<u64> {
        match element {
            Element::CountTree(_, count, _)
            | Element::CountSumTree(_, count, ..)
            | Element::ProvableCountTree(_, count, _)
            | Element::ProvableCountSumTree(_, count, ..) => Some(*count),
            _ => None,
        }
    }

    /// Verify a serialized branch chunk proof.
    ///
    /// # Arguments
    /// * `proof` - The serialized proof bytes
    /// * `query` - The path branch chunk query
    /// * `expected_root_hash` - The expected root hash of the branch (from
    ///   parent trunk/branch proof)
    /// * `grove_version` - The GroveDB version for element deserialization
    ///
    /// # Returns
    /// `GroveBranchQueryResult` containing:
    /// - Deserialized GroveDB Elements
    /// - Leaf keys with their hashes for subsequent branch queries (if more
    ///   depth remains)
    /// - The verified branch root hash
    pub fn verify_branch_chunk_proof(
        proof: &[u8],
        query: &crate::query::PathBranchChunkQuery,
        expected_root_hash: CryptoHash,
        grove_version: &GroveVersion,
    ) -> Result<crate::query::GroveBranchQueryResult, Error> {
        // Decode the proof ops
        let decoder = Decoder::new(proof);
        let ops: Vec<Op> = decoder
            .collect::<Result<Vec<_>, _>>()
            .map_err(|e| Error::CorruptedData(format!("Failed to decode branch proof: {}", e)))?;

        // Execute the proof to build the tree structure and get its root hash
        // Use collapse=false to preserve the full tree structure for element extraction
        let branch_tree = execute(ops.iter().map(|op| Ok(op.clone())), false, |_| Ok(()))
            .unwrap()
            .map_err(|e| {
                Error::InvalidProof(
                    PathQuery::new_unsized(query.path.clone(), Query::default()),
                    format!("Failed to execute branch proof: {}", e),
                )
            })?;

        let branch_root_hash = branch_tree.hash().unwrap();

        // Verify the computed hash matches the expected hash from the parent proof
        if branch_root_hash != expected_root_hash {
            return Err(Error::InvalidProof(
                PathQuery::new_unsized(query.path.clone(), Query::default()),
                format!(
                    "Branch root hash mismatch: expected {}, got {}",
                    hex::encode(expected_root_hash),
                    hex::encode(branch_root_hash)
                ),
            ));
        }

        // Extract elements and leaf keys from the proof tree
        let mut elements = BTreeMap::new();
        let mut leaf_keys = BTreeMap::new();
        Self::extract_elements_and_leaf_keys(
            &branch_tree,
            &mut elements,
            &mut leaf_keys,
            0,
            query.depth as usize,
            grove_version,
        )?;

        Ok(crate::query::GroveBranchQueryResult {
            elements,
            leaf_keys,
            branch_root_hash,
            tree: branch_tree,
        })
    }
}
