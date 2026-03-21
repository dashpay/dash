use std::collections::BTreeMap;

use serde::{Deserialize, Serialize};
use serde_with::{base64::Base64, serde_as};

pub type Key = Vec<u8>;
pub type Path = Vec<PathSegment>;
pub type PathSegment = Vec<u8>;
pub type SessionId = u64;

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct WithSession<R> {
    pub session_id: SessionId,
    pub request: R,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct NewSessionResponse {
    pub session_id: SessionId,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct DropSessionRequest {
    pub session_id: SessionId,
}

#[serde_as]
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct NodeFetchRequest {
    #[serde_as(as = "Vec<Base64>")]
    pub path: Path,
    #[serde_as(as = "Base64")]
    pub key: Key,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct RootFetchRequest;

#[serde_as]
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct NodeUpdate {
    #[serde_as(as = "Option<Base64>")]
    pub left_child: Option<Key>,
    #[serde_as(as = "Option<Base64>")]
    pub left_merk_hash: Option<CryptoHash>,
    #[serde_as(as = "Option<Base64>")]
    pub right_child: Option<Key>,
    #[serde_as(as = "Option<Base64>")]
    pub right_merk_hash: Option<CryptoHash>,
    #[serde_as(as = "Vec<Base64>")]
    pub path: Path,
    #[serde_as(as = "Base64")]
    pub key: Key,
    pub element: Element,
    pub feature_type: TreeFeatureType,
    #[serde_as(as = "Base64")]
    pub value_hash: CryptoHash,
    #[serde_as(as = "Base64")]
    pub kv_digest_hash: CryptoHash,
}

#[serde_as]
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub enum Reference {
    AbsolutePathReference {
        #[serde_as(as = "Vec<Base64>")]
        path: Path,
        #[serde_as(as = "Option<Base64>")]
        element_flags: Option<Vec<u8>>,
    },
    UpstreamRootHeightReference {
        n_keep: u32,
        #[serde_as(as = "Vec<Base64>")]
        path_append: Vec<PathSegment>,
        #[serde_as(as = "Option<Base64>")]
        element_flags: Option<Vec<u8>>,
    },
    UpstreamRootHeightWithParentPathAdditionReference {
        n_keep: u32,
        #[serde_as(as = "Vec<Base64>")]
        path_append: Vec<PathSegment>,
        #[serde_as(as = "Option<Base64>")]
        element_flags: Option<Vec<u8>>,
    },
    UpstreamFromElementHeightReference {
        n_remove: u32,
        #[serde_as(as = "Vec<Base64>")]
        path_append: Vec<PathSegment>,
        #[serde_as(as = "Option<Base64>")]
        element_flags: Option<Vec<u8>>,
    },
    CousinReference {
        #[serde_as(as = "Base64")]
        swap_parent: PathSegment,
        #[serde_as(as = "Option<Base64>")]
        element_flags: Option<Vec<u8>>,
    },
    RemovedCousinReference {
        #[serde_as(as = "Vec<Base64>")]
        swap_parent: Vec<PathSegment>,
        #[serde_as(as = "Option<Base64>")]
        element_flags: Option<Vec<u8>>,
    },
    SiblingReference {
        #[serde_as(as = "Base64")]
        sibling_key: Key,
        #[serde_as(as = "Option<Base64>")]
        element_flags: Option<Vec<u8>>,
    },
}

#[serde_as]
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub enum Element {
    Subtree {
        #[serde_as(as = "Option<Base64>")]
        root_key: Option<Key>,
        #[serde_as(as = "Option<Base64>")]
        element_flags: Option<Vec<u8>>,
    },
    Sumtree {
        #[serde_as(as = "Option<Base64>")]
        root_key: Option<Key>,
        sum: i64,
        #[serde_as(as = "Option<Base64>")]
        element_flags: Option<Vec<u8>>,
    },
    BigSumTree {
        #[serde_as(as = "Option<Base64>")]
        root_key: Option<Key>,
        sum: i128,
        #[serde_as(as = "Option<Base64>")]
        element_flags: Option<Vec<u8>>,
    },
    CountTree {
        #[serde_as(as = "Option<Base64>")]
        root_key: Option<Key>,
        count: u64,
        #[serde_as(as = "Option<Base64>")]
        element_flags: Option<Vec<u8>>,
    },
    CountSumTree {
        #[serde_as(as = "Option<Base64>")]
        root_key: Option<Key>,
        count: u64,
        sum: i64,
        #[serde_as(as = "Option<Base64>")]
        element_flags: Option<Vec<u8>>,
    },
    ProvableCountTree {
        #[serde_as(as = "Option<Base64>")]
        root_key: Option<Key>,
        count: u64,
        #[serde_as(as = "Option<Base64>")]
        element_flags: Option<Vec<u8>>,
    },
    ProvableCountSumTree {
        #[serde_as(as = "Option<Base64>")]
        root_key: Option<Key>,
        count: u64,
        sum: i64,
        #[serde_as(as = "Option<Base64>")]
        element_flags: Option<Vec<u8>>,
    },
    Item {
        #[serde_as(as = "Base64")]
        value: Vec<u8>,
        #[serde_as(as = "Option<Base64>")]
        element_flags: Option<Vec<u8>>,
    },
    SumItem {
        value: i64,
        #[serde_as(as = "Option<Base64>")]
        element_flags: Option<Vec<u8>>,
    },
    ItemWithSumItem {
        #[serde_as(as = "Base64")]
        value: Vec<u8>,
        sum_item_value: i64,
        #[serde_as(as = "Option<Base64>")]
        element_flags: Option<Vec<u8>>,
    },
    Reference(Reference),
}

#[serde_as]
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct PathQuery {
    #[serde_as(as = "Vec<Base64>")]
    pub path: Path,
    pub query: SizedQuery,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct SizedQuery {
    pub query: Query,
    pub limit: Option<u16>,
    pub offset: Option<u16>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct Query {
    pub items: Vec<QueryItem>,
    pub default_subquery_branch: SubqueryBranch,
    pub conditional_subquery_branches: Vec<(QueryItem, SubqueryBranch)>,
    pub left_to_right: bool,
}

#[serde_as]
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub enum QueryItem {
    Key(#[serde_as(as = "Base64")] Vec<u8>),
    Range {
        #[serde_as(as = "Base64")]
        start: Key,
        #[serde_as(as = "Base64")]
        end: Key,
    },
    RangeInclusive {
        #[serde_as(as = "Base64")]
        start: Key,
        #[serde_as(as = "Base64")]
        end: Key,
    },
    RangeFull,
    RangeFrom(#[serde_as(as = "Base64")] Key),
    RangeTo(#[serde_as(as = "Base64")] Key),
    RangeToInclusive(#[serde_as(as = "Base64")] Key),
    RangeAfter(#[serde_as(as = "Base64")] Key),
    RangeAfterTo {
        #[serde_as(as = "Base64")]
        after: Key,
        #[serde_as(as = "Base64")]
        to: Key,
    },
    RangeAfterToInclusive {
        #[serde_as(as = "Base64")]
        after: Key,
        #[serde_as(as = "Base64")]
        to: Key,
    },
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct SubqueryBranch {
    pub subquery_path: Option<Vec<PathSegment>>,
    pub subquery: Option<Box<Query>>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct Proof {
    pub root_layer: ProofLayer,
    pub prove_options: ProveOptions,
}

#[serde_as]
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct ProofLayer {
    pub merk_proof: Vec<MerkProofOp>,
    #[serde_as(as = "BTreeMap<Base64, _>")]
    pub lower_layers: BTreeMap<Key, ProofLayer>,
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub enum MerkProofOp {
    Push(MerkProofNode),
    PushInverted(MerkProofNode),
    Parent,
    Child,
    ParentInverted,
    ChildInverted,
}

pub type CryptoHash = [u8; 32];

#[serde_as]
#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub enum MerkProofNode {
    Hash(#[serde_as(as = "Base64")] CryptoHash),
    KVHash(#[serde_as(as = "Base64")] CryptoHash),
    KVDigest(
        #[serde_as(as = "Base64")] Key,
        #[serde_as(as = "Base64")] CryptoHash,
    ),
    KV(#[serde_as(as = "Base64")] Key, Element),
    KVValueHash(
        #[serde_as(as = "Base64")] Key,
        Element,
        #[serde_as(as = "Base64")] CryptoHash,
    ),
    KVValueHashFeatureType(
        #[serde_as(as = "Base64")] Key,
        Element,
        #[serde_as(as = "Base64")] CryptoHash,
        TreeFeatureType,
    ),
    KVRefValueHash(
        #[serde_as(as = "Base64")] Key,
        Element,
        #[serde_as(as = "Base64")] CryptoHash,
    ),
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub enum TreeFeatureType {
    BasicMerkNode,
    SummedMerkNode(i64),
    BigSummedMerkNode(i128),
    CountedMerkNode(u64),
    CountedSummedMerkNode(u64, i64),
    ProvableCountedMerkNode(u64),
    ProvableCountedSummedMerkNode(u64, i64),
}

#[derive(Debug, Clone, Serialize, Deserialize, PartialEq, Eq)]
pub struct ProveOptions {
    pub decrease_limit_on_empty_sub_query_result: bool,
}
