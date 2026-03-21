//! Proof operations

#[cfg(feature = "minimal")]
mod generate;
pub mod util;
mod verify;

use std::{collections::BTreeMap, fmt};

use bincode::{Decode, Encode};
use grovedb_merk::{
    proofs::{
        query::{Key, VerifyOptions},
        Decoder, Node, Op,
    },
    CryptoHash,
};
use grovedb_version::version::GroveVersion;

use crate::{
    operations::proof::util::{element_hex_to_ascii, hex_to_ascii, ProvedPathKeyValues},
    query_result_type::PathKeyOptionalElementTrio,
    Error, GroveDb, PathQuery,
};

#[derive(Debug, Clone, Copy, Encode, Decode)]
pub struct ProveOptions {
    /// This tells the proof system to decrease the available limit of the query
    /// by 1 in the case of empty subtrees. Generally this should be set to
    /// true. The case where this could be set to false is if there is a
    /// known structure where we know that there are only a few empty
    /// subtrees.
    ///
    /// !!! Warning !!! Be very careful:
    /// If this is set to `false` then you must be sure that the sub queries do
    /// not match many trees, Otherwise you could crash the system as the
    /// proof system goes through millions of subtrees and eventually runs
    /// out of memory
    pub decrease_limit_on_empty_sub_query_result: bool,
}

impl fmt::Display for ProveOptions {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        write!(
            f,
            "ProveOptions {{ decrease_limit_on_empty_sub_query_result: {} }}",
            self.decrease_limit_on_empty_sub_query_result
        )
    }
}

impl Default for ProveOptions {
    fn default() -> Self {
        ProveOptions {
            decrease_limit_on_empty_sub_query_result: true,
        }
    }
}

#[derive(Encode, Decode)]
pub struct LayerProof {
    pub merk_proof: Vec<u8>,
    pub lower_layers: BTreeMap<Key, LayerProof>,
}

#[derive(Encode, Decode)]
pub enum GroveDBProof {
    V0(GroveDBProofV0),
}

impl GroveDBProof {
    /// Verifies a query with options using the proof and returns the root hash
    /// and the query result.
    pub fn verify_with_options(
        &self,
        query: &PathQuery,
        options: VerifyOptions,
        grove_version: &GroveVersion,
    ) -> Result<(CryptoHash, Vec<PathKeyOptionalElementTrio>), Error> {
        GroveDb::verify_proof_internal(self, query, options, grove_version)
            .map(|(root_hash, _, results)| (root_hash, results))
    }

    /// Verifies a raw query using the proof and returns the root hash and the
    /// query result.
    pub fn verify_raw(
        &self,
        query: &PathQuery,
        grove_version: &GroveVersion,
    ) -> Result<(CryptoHash, ProvedPathKeyValues), Error> {
        GroveDb::verify_proof_raw_internal(
            self,
            query,
            VerifyOptions {
                absence_proofs_for_non_existing_searched_keys: false,
                verify_proof_succinctness: false,
                include_empty_trees_in_result: true,
            },
            grove_version,
        )
        .map(|(root_hash, _, results)| (root_hash, results))
    }

    /// Verifies a query using the proof and returns the root hash and the query
    /// result.
    pub fn verify(
        &self,
        query: &PathQuery,
        grove_version: &GroveVersion,
    ) -> Result<(CryptoHash, Vec<PathKeyOptionalElementTrio>), Error> {
        GroveDb::verify_proof_internal(
            self,
            query,
            VerifyOptions {
                absence_proofs_for_non_existing_searched_keys: false,
                verify_proof_succinctness: true,
                include_empty_trees_in_result: false,
            },
            grove_version,
        )
        .map(|(root_hash, _, results)| (root_hash, results))
    }

    /// Verifies a query with an absence proof and returns the root hash and the
    /// query result.
    pub fn verify_with_absence_proof(
        &self,
        query: &PathQuery,
        grove_version: &GroveVersion,
    ) -> Result<(CryptoHash, Vec<PathKeyOptionalElementTrio>), Error> {
        GroveDb::verify_proof_internal(
            self,
            query,
            VerifyOptions {
                absence_proofs_for_non_existing_searched_keys: true,
                verify_proof_succinctness: true,
                include_empty_trees_in_result: false,
            },
            grove_version,
        )
        .map(|(root_hash, _, results)| (root_hash, results))
    }

    /// Verifies a subset query using the proof and returns the root hash and
    /// the query result.
    pub fn verify_subset(
        &self,
        query: &PathQuery,
        grove_version: &GroveVersion,
    ) -> Result<(CryptoHash, Vec<PathKeyOptionalElementTrio>), Error> {
        GroveDb::verify_proof_internal(
            self,
            query,
            VerifyOptions {
                absence_proofs_for_non_existing_searched_keys: false,
                verify_proof_succinctness: false,
                include_empty_trees_in_result: false,
            },
            grove_version,
        )
        .map(|(root_hash, _, results)| (root_hash, results))
    }

    /// Verifies a subset query with an absence proof using the proof and
    /// returns the root hash and the query result.
    pub fn verify_subset_with_absence_proof(
        &self,
        query: &PathQuery,
        grove_version: &GroveVersion,
    ) -> Result<(CryptoHash, Vec<PathKeyOptionalElementTrio>), Error> {
        GroveDb::verify_proof_internal(
            self,
            query,
            VerifyOptions {
                absence_proofs_for_non_existing_searched_keys: true,
                verify_proof_succinctness: false,
                include_empty_trees_in_result: false,
            },
            grove_version,
        )
        .map(|(root_hash, _, results)| (root_hash, results))
    }
}

#[derive(Encode, Decode)]
pub struct GroveDBProofV0 {
    pub root_layer: LayerProof,
    pub prove_options: ProveOptions,
}

impl fmt::Display for LayerProof {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        writeln!(f, "LayerProof {{")?;
        writeln!(f, "  merk_proof: {}", decode_merk_proof(&self.merk_proof))?;
        if !self.lower_layers.is_empty() {
            writeln!(f, "  lower_layers: {{")?;
            for (key, layer_proof) in &self.lower_layers {
                writeln!(f, "    {} => {{", hex_to_ascii(key))?;
                for line in format!("{}", layer_proof).lines() {
                    writeln!(f, "      {}", line)?;
                }
                writeln!(f, "    }}")?;
            }
            writeln!(f, "  }}")?;
        }
        write!(f, "}}")
    }
}

impl fmt::Display for GroveDBProof {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        match self {
            GroveDBProof::V0(proof) => write!(f, "{}", proof),
        }
    }
}

impl fmt::Display for GroveDBProofV0 {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        writeln!(f, "GroveDBProofV0 {{")?;
        for line in format!("{}", self.root_layer).lines() {
            writeln!(f, "  {}", line)?;
        }
        write!(f, "}}")
    }
}

fn decode_merk_proof(proof: &[u8]) -> String {
    let mut result = String::new();
    let ops = Decoder::new(proof);

    for (i, op) in ops.enumerate() {
        match op {
            Ok(op) => {
                result.push_str(&format!("\n    {}: {}", i, op_to_string(&op)));
            }
            Err(e) => {
                result.push_str(&format!("\n    {}: Error decoding op: {}", i, e));
            }
        }
    }

    result
}

fn op_to_string(op: &Op) -> String {
    match op {
        Op::Push(node) => format!("Push({})", node_to_string(node)),
        Op::PushInverted(node) => format!("PushInverted({})", node_to_string(node)),
        Op::Parent => "Parent".to_string(),
        Op::Child => "Child".to_string(),
        Op::ParentInverted => "ParentInverted".to_string(),
        Op::ChildInverted => "ChildInverted".to_string(),
    }
}

fn node_to_string(node: &Node) -> String {
    match node {
        Node::Hash(hash) => format!("Hash(HASH[{}])", hex::encode(hash)),
        Node::KVHash(kv_hash) => format!("KVHash(HASH[{}])", hex::encode(kv_hash)),
        Node::KV(key, value) => {
            format!("KV({}, {})", hex_to_ascii(key), element_hex_to_ascii(value))
        }
        Node::KVValueHash(key, value, value_hash) => format!(
            "KVValueHash({}, {}, HASH[{}])",
            hex_to_ascii(key),
            element_hex_to_ascii(value),
            hex::encode(value_hash)
        ),
        Node::KVDigest(key, value_hash) => format!(
            "KVDigest({}, HASH[{}])",
            hex_to_ascii(key),
            hex::encode(value_hash)
        ),
        Node::KVDigestCount(key, value_hash, count) => format!(
            "KVDigestCount({}, HASH[{}], {})",
            hex_to_ascii(key),
            hex::encode(value_hash),
            count
        ),
        Node::KVRefValueHash(key, value, value_hash) => format!(
            "KVRefValueHash({}, {}, HASH[{}])",
            hex_to_ascii(key),
            element_hex_to_ascii(value),
            hex::encode(value_hash)
        ),
        Node::KVValueHashFeatureType(key, value, value_hash, feature_type) => format!(
            "KVValueHashFeatureType({}, {}, HASH[{}], {:?})",
            hex_to_ascii(key),
            element_hex_to_ascii(value),
            hex::encode(value_hash),
            feature_type
        ),
        Node::KVCount(key, value, count) => format!(
            "KVCount({}, {}, {})",
            hex_to_ascii(key),
            element_hex_to_ascii(value),
            count
        ),
        Node::KVHashCount(kv_hash, count) => {
            format!("KVHashCount(HASH[{}], {})", hex::encode(kv_hash), count)
        }
        Node::KVRefValueHashCount(key, value, value_hash, count) => format!(
            "KVRefValueHashCount({}, {}, HASH[{}], {})",
            hex_to_ascii(key),
            element_hex_to_ascii(value),
            hex::encode(value_hash),
            count
        ),
    }
}
