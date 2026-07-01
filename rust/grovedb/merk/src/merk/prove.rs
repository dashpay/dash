use std::collections::LinkedList;

use grovedb_costs::{CostResult, CostsExt};
use grovedb_storage::StorageContext;
use grovedb_version::version::GroveVersion;

use crate::{
    proofs::{encode_into, query::QueryItem, Op as ProofOp, Query},
    tree::RefWalker,
    Error, Merk,
};

impl<'db, S> Merk<S>
where
    S: StorageContext<'db>,
{
    /// Creates a Merkle proof for the list of queried keys. For each key in the
    /// query, if the key is found in the store then the value will be proven to
    /// be in the tree. For each key in the query that does not exist in the
    /// tree, its absence will be proven by including boundary keys.
    ///
    /// The proof returned is in an encoded format which can be verified with
    /// `merk::verify`.
    ///
    /// This will fail if the keys in `query` are not sorted and unique. This
    /// check adds some overhead, so if you are sure your batch is sorted and
    /// unique you can use the unsafe `prove_unchecked` for a small performance
    /// gain.
    pub fn prove(
        &self,
        query: Query,
        limit: Option<u16>,
        grove_version: &GroveVersion,
    ) -> CostResult<ProofConstructionResult, Error> {
        let left_to_right = query.left_to_right;
        self.prove_unchecked(query, limit, left_to_right, grove_version)
            .map_ok(|(proof, limit)| {
                let mut bytes = Vec::with_capacity(128);
                encode_into(proof.iter(), &mut bytes);
                ProofConstructionResult::new(bytes, limit)
            })
    }

    /// Creates a Merkle proof for the list of queried keys. For each key in the
    /// query, if the key is found in the store then the value will be proven to
    /// be in the tree. For each key in the query that does not exist in the
    /// tree, its absence will be proven by including boundary keys.
    ///
    /// The proof returned is in an intermediate format to be later encoded
    ///
    /// This will fail if the keys in `query` are not sorted and unique. This
    /// check adds some overhead, so if you are sure your batch is sorted and
    /// unique you can use the unsafe `prove_unchecked` for a small performance
    /// gain.
    pub fn prove_without_encoding(
        &self,
        query: Query,
        limit: Option<u16>,
        grove_version: &GroveVersion,
    ) -> CostResult<ProofWithoutEncodingResult, Error> {
        let left_to_right = query.left_to_right;
        self.prove_unchecked(query, limit, left_to_right, grove_version)
            .map_ok(|(proof, limit)| ProofWithoutEncodingResult::new(proof, limit))
    }

    /// Creates a Merkle proof for the list of queried keys. For each key in
    /// the query, if the key is found in the store then the value will be
    /// proven to be in the tree. For each key in the query that does not
    /// exist in the tree, its absence will be proven by including
    /// boundary keys.
    /// The proof returned is in an encoded format which can be verified with
    /// `merk::verify`.
    ///
    /// This is unsafe because the keys in `query` must be sorted and unique -
    /// if they are not, there will be undefined behavior. For a safe version
    /// of this method which checks to ensure the batch is sorted and
    /// unique, see `prove`.
    pub fn prove_unchecked<Q, I>(
        &self,
        query: I,
        limit: Option<u16>,
        left_to_right: bool,
        grove_version: &GroveVersion,
    ) -> CostResult<Proof, Error>
    where
        Q: Into<QueryItem>,
        I: IntoIterator<Item = Q>,
    {
        let query_vec: Vec<QueryItem> = query.into_iter().map(Into::into).collect();

        self.use_tree_mut(|maybe_tree| {
            maybe_tree
                .ok_or(Error::CorruptedCodeExecution(
                    "Cannot create proof for empty tree",
                ))
                .wrap_with_cost(Default::default())
                .flat_map_ok(|tree| {
                    let mut ref_walker = RefWalker::new(tree, self.source());
                    ref_walker.create_proof(
                        query_vec.as_slice(),
                        limit,
                        left_to_right,
                        grove_version,
                    )
                })
                .map_ok(|(proof, _, status, ..)| (proof, status.limit))
        })
    }

    /// Creates a Merkle proof for the list of queried keys. For each key in
    /// the query, if the key is found in the store then the value will be
    /// proven to be in the tree. For each key in the query that does not
    /// exist in the tree, its absence will be proven by including
    /// boundary keys.
    /// The proof returned is in an encoded format which can be verified with
    /// `merk::verify`.
    ///
    /// This is unsafe because the keys in `query` must be sorted and unique -
    /// if they are not, there will be undefined behavior. For a safe version
    /// of this method which checks to ensure the batch is sorted and
    /// unique, see `prove`.
    pub fn prove_unchecked_query_items(
        &self,
        query_items: &[QueryItem],
        limit: Option<u16>,
        left_to_right: bool,
        grove_version: &GroveVersion,
    ) -> CostResult<Proof, Error> {
        self.use_tree_mut(|maybe_tree| {
            maybe_tree
                .ok_or(Error::CorruptedCodeExecution(
                    "Cannot create proof for empty tree",
                ))
                .wrap_with_cost(Default::default())
                .flat_map_ok(|tree| {
                    let mut ref_walker = RefWalker::new(tree, self.source());
                    ref_walker.create_proof(query_items, limit, left_to_right, grove_version)
                })
                .map_ok(|(proof, _, status, ..)| (proof, status.limit))
        })
    }
}

type Proof = (LinkedList<ProofOp>, Option<u16>);

/// Proof construction result
pub struct ProofConstructionResult {
    /// Proof
    pub proof: Vec<u8>,
    /// Limit
    pub limit: Option<u16>,
}

impl ProofConstructionResult {
    /// New ProofConstructionResult
    pub fn new(proof: Vec<u8>, limit: Option<u16>) -> Self {
        Self { proof, limit }
    }
}

/// Proof without encoding result
pub struct ProofWithoutEncodingResult {
    /// Proof
    pub proof: LinkedList<ProofOp>,
    /// Limit
    pub limit: Option<u16>,
}

impl ProofWithoutEncodingResult {
    /// New ProofWithoutEncodingResult
    pub fn new(proof: LinkedList<ProofOp>, limit: Option<u16>) -> Self {
        Self { proof, limit }
    }
}
