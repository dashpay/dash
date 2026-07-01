//! GroveDB debugging support module.

use std::{
    collections::{BTreeMap, HashMap},
    fs,
    sync::{Arc, Weak},
    time::{Duration, Instant, SystemTime},
};

use axum::{extract::State, http::StatusCode, response::IntoResponse, routing::post, Json, Router};
use grovedb_merk::{
    debugger::NodeDbg,
    proofs::{Decoder, Node, Op},
    tree::value_hash,
    TreeFeatureType,
};
use grovedb_path::SubtreePath;
use grovedb_version::version::GroveVersion;
use grovedbg_types::{
    DropSessionRequest, MerkProofNode, MerkProofOp, NewSessionResponse, NodeFetchRequest,
    NodeUpdate, Path, PathQuery, Query, QueryItem, SessionId, SizedQuery, SubqueryBranch,
    WithSession,
};
use indexmap::IndexMap;
use tempfile::tempdir;
use tokio::{
    net::ToSocketAddrs,
    select,
    sync::{RwLock, RwLockReadGuard},
    time::sleep,
};
use tokio_util::sync::CancellationToken;
use tower_http::services::ServeDir;

use crate::{
    operations::proof::{GroveDBProof, LayerProof, ProveOptions},
    query_result_type::{QueryResultElement, QueryResultElements, QueryResultType},
    reference_path::ReferencePathType,
    GroveDb,
};

const GROVEDBG_ZIP: &[u8] = include_bytes!(concat!(env!("OUT_DIR"), "/grovedbg.zip"));

const SESSION_TIMEOUT: Duration = Duration::from_secs(60 * 10);

pub(super) fn start_visualizer<A>(grovedb: Weak<GroveDb>, addr: A)
where
    A: ToSocketAddrs + Send + 'static,
{
    std::thread::spawn(move || {
        let grovedbg_tmp =
            tempfile::tempdir().expect("cannot create tempdir for grovedbg contents");
        let grovedbg_zip = grovedbg_tmp.path().join("grovedbg.zip");
        let grovedbg_www = grovedbg_tmp.path().join("grovedbg_www");

        fs::write(&grovedbg_zip, GROVEDBG_ZIP).expect("cannot crate grovedbg.zip");
        zip_extensions::inflate::zip_extract::zip_extract(&grovedbg_zip, &grovedbg_www)
            .expect("cannot extract grovedbg contents");

        let cancellation_token = CancellationToken::new();

        let state: AppState = AppState {
            cancellation_token: cancellation_token.clone(),
            grovedb,
            sessions: Default::default(),
        };

        let app = Router::new()
            .route("/new_session", post(new_session))
            .route("/drop_session", post(drop_session))
            .route("/fetch_node", post(fetch_node))
            .route("/fetch_root_node", post(fetch_root_node))
            .route("/prove_path_query", post(prove_path_query))
            .route("/fetch_with_path_query", post(fetch_with_path_query))
            .fallback_service(ServeDir::new(grovedbg_www))
            .with_state(state.clone());

        let rt = tokio::runtime::Runtime::new().unwrap();

        let cloned_cancellation_token = cancellation_token.clone();
        rt.spawn(async move {
            loop {
                select! {
                    _ = cloned_cancellation_token.cancelled() => break,
                    _ = sleep(Duration::from_secs(10)) => {
                        let now = Instant::now();
                        let mut lock = state.sessions.write().await;
                        let to_delete: Vec<SessionId> = lock.iter().filter_map(
                            |(id, session)|
                                (session.last_access < now - SESSION_TIMEOUT).then_some(*id)
                        ).collect();

                        to_delete.into_iter().for_each(|id| { lock.remove(&id); });
                    }
                }
            }
        });

        rt.block_on(async move {
            let listener = tokio::net::TcpListener::bind(addr)
                .await
                .expect("can't bind visualizer port");
            axum::serve(listener, app)
                .with_graceful_shutdown(async move {
                    cancellation_token.cancelled().await;
                })
                .await
                .unwrap()
        });
    });
}

#[derive(Clone)]
struct AppState {
    cancellation_token: CancellationToken,
    grovedb: Weak<GroveDb>,
    sessions: Arc<RwLock<HashMap<SessionId, Session>>>,
}

impl AppState {
    fn verify_running(&self) -> Result<(), AppError> {
        if self.grovedb.strong_count() == 0 {
            self.cancellation_token.cancel();
            Err(AppError::Closed)
        } else {
            Ok(())
        }
    }

    async fn new_session(&self) -> Result<SessionId, AppError> {
        let grovedb = self.grovedb.upgrade().ok_or(AppError::Closed)?;
        let id = SystemTime::now()
            .duration_since(SystemTime::UNIX_EPOCH)
            .expect("time went backwards")
            .as_secs();
        self.sessions
            .write()
            .await
            .insert(id, Session::new(&grovedb)?);

        Ok(id)
    }

    async fn drop_session(&self, id: SessionId) {
        self.sessions.write().await.remove(&id);
    }

    async fn get_checkpointed_grovedb(
        &self,
        id: SessionId,
    ) -> Result<RwLockReadGuard<GroveDb>, AppError> {
        self.verify_running()?;
        let mut lock = self.sessions.write().await;
        if let Some(session) = lock.get_mut(&id) {
            session.last_access = Instant::now();
            Ok(RwLockReadGuard::map(lock.downgrade(), |l| {
                &l.get(&id)
                    .as_ref()
                    .expect("checked above")
                    .checkpointed_grovedb
            }))
        } else {
            Err(AppError::NoSession)
        }
    }
}

struct Session {
    last_access: Instant,
    _tempdir: tempfile::TempDir,
    checkpointed_grovedb: GroveDb,
}

impl Session {
    fn new(grovedb: &GroveDb) -> Result<Self, AppError> {
        let tempdir = tempdir().map_err(|e| AppError::Any(e.to_string()))?;
        let path = tempdir.path().join("grovedbg_session");
        grovedb
            .create_checkpoint(&path)
            .map_err(|e| AppError::Any(e.to_string()))?;
        let checkpointed_grovedb = GroveDb::open(path).map_err(|e| AppError::Any(e.to_string()))?;
        Ok(Session {
            last_access: Instant::now(),
            _tempdir: tempdir,
            checkpointed_grovedb,
        })
    }
}

enum AppError {
    Closed,
    NoSession,
    Any(String),
}

impl IntoResponse for AppError {
    fn into_response(self) -> axum::response::Response {
        match self {
            AppError::Closed => {
                (StatusCode::SERVICE_UNAVAILABLE, "GroveDB is closed").into_response()
            }
            AppError::NoSession => {
                (StatusCode::UNAUTHORIZED, "No session with this id").into_response()
            }
            AppError::Any(e) => (StatusCode::INTERNAL_SERVER_ERROR, e).into_response(),
        }
    }
}

impl<E: std::error::Error> From<E> for AppError {
    fn from(err: E) -> Self {
        Self::Any(err.to_string())
    }
}

async fn new_session(State(state): State<AppState>) -> Result<Json<NewSessionResponse>, AppError> {
    Ok(Json(NewSessionResponse {
        session_id: state.new_session().await?,
    }))
}

async fn drop_session(
    State(state): State<AppState>,
    Json(DropSessionRequest { session_id }): Json<DropSessionRequest>,
) {
    state.drop_session(session_id).await;
}

async fn fetch_node(
    State(state): State<AppState>,
    Json(WithSession {
        session_id,
        request: NodeFetchRequest { path, key },
    }): Json<WithSession<NodeFetchRequest>>,
) -> Result<Json<Option<NodeUpdate>>, AppError> {
    let db = state.get_checkpointed_grovedb(session_id).await?;
    let transaction = db.start_transaction();

    let merk = db
        .open_transactional_merk_at_path(
            path.as_slice().into(),
            &transaction,
            None,
            GroveVersion::latest(),
        )
        .unwrap()?;
    let node = merk.get_node_dbg(&key)?;

    if let Some(node) = node {
        let node_update: NodeUpdate = node_to_update(path, node)?;
        Ok(Json(Some(node_update)))
    } else {
        Ok(None.into())
    }
}

async fn fetch_root_node(
    State(state): State<AppState>,
    Json(WithSession {
        session_id,
        request: (),
    }): Json<WithSession<()>>,
) -> Result<Json<Option<NodeUpdate>>, AppError> {
    let db = state.get_checkpointed_grovedb(session_id).await?;
    let transaction = db.start_transaction();

    let merk = db
        .open_transactional_merk_at_path(
            SubtreePath::empty(),
            &transaction,
            None,
            GroveVersion::latest(),
        )
        .unwrap()?;

    let node = merk.get_root_node_dbg()?;

    if let Some(node) = node {
        let node_update: NodeUpdate = node_to_update(Vec::new(), node)?;
        Ok(Json(Some(node_update)))
    } else {
        Ok(None.into())
    }
}

async fn prove_path_query(
    State(state): State<AppState>,
    Json(WithSession {
        session_id,
        request: json_path_query,
    }): Json<WithSession<PathQuery>>,
) -> Result<Json<grovedbg_types::Proof>, AppError> {
    let db = state.get_checkpointed_grovedb(session_id).await?;

    let path_query = path_query_to_grovedb(json_path_query);

    let grovedb_proof = db
        .prove_query_non_serialized(&path_query, None, GroveVersion::latest())
        .unwrap()?;
    Ok(Json(proof_to_grovedbg(grovedb_proof)?))
}

async fn fetch_with_path_query(
    State(state): State<AppState>,
    Json(WithSession {
        session_id,
        request: json_path_query,
    }): Json<WithSession<PathQuery>>,
) -> Result<Json<Vec<grovedbg_types::NodeUpdate>>, AppError> {
    let db = state.get_checkpointed_grovedb(session_id).await?;

    let path_query = path_query_to_grovedb(json_path_query);

    let grovedb_query_result = db
        .query_raw(
            &path_query,
            false,
            true,
            false,
            QueryResultType::QueryPathKeyElementTrioResultType,
            None,
            GroveVersion::latest(),
        )
        .unwrap()?
        .0;
    Ok(Json(query_result_to_grovedbg(&db, grovedb_query_result)?))
}

fn query_result_to_grovedbg(
    db: &GroveDb,
    query_result: QueryResultElements,
) -> Result<Vec<NodeUpdate>, crate::Error> {
    let mut result = Vec::new();
    let transaction = db.start_transaction();

    let mut last_merk: Option<(Vec<Vec<u8>>, grovedb_merk::Merk<_>)> = None;

    for qr in query_result.elements.into_iter() {
        if let QueryResultElement::PathKeyElementTrioResultItem((path, key, _)) = qr {
            let merk: &grovedb_merk::Merk<_> = match &mut last_merk {
                Some((last_merk_path, last_merk)) if last_merk_path == &path => last_merk,
                _ => {
                    last_merk = Some((
                        path.clone(),
                        db.open_transactional_merk_at_path(
                            path.as_slice().into(),
                            &transaction,
                            None,
                            GroveVersion::latest(),
                        )
                        .unwrap()?,
                    ));
                    &last_merk.as_ref().unwrap().1
                }
            };

            if let Some(node) = merk.get_node_dbg(&key)? {
                result.push(node_to_update(path, node)?);
            }
        }
    }
    Ok(result)
}

fn proof_to_grovedbg(proof: GroveDBProof) -> Result<grovedbg_types::Proof, crate::Error> {
    match proof {
        GroveDBProof::V0(p) => Ok(grovedbg_types::Proof {
            root_layer: proof_layer_to_grovedbg(p.root_layer)?,
            prove_options: prove_options_to_grovedbg(p.prove_options),
        }),
    }
}

fn proof_layer_to_grovedbg(
    proof_layer: LayerProof,
) -> Result<grovedbg_types::ProofLayer, crate::Error> {
    Ok(grovedbg_types::ProofLayer {
        merk_proof: merk_proof_to_grovedbg(&proof_layer.merk_proof)?,
        lower_layers: proof_layer
            .lower_layers
            .into_iter()
            .map(|(k, v)| proof_layer_to_grovedbg(v).map(|layer| (k, layer)))
            .collect::<Result<BTreeMap<Vec<u8>, grovedbg_types::ProofLayer>, crate::Error>>()?,
    })
}

fn merk_proof_to_grovedbg(merk_proof: &[u8]) -> Result<Vec<MerkProofOp>, crate::Error> {
    let decoder = Decoder::new(merk_proof);
    decoder
        .map(|op_result| {
            op_result
                .map_err(crate::Error::MerkError)
                .and_then(merk_proof_op_to_grovedbg)
        })
        .collect::<Result<Vec<MerkProofOp>, _>>()
}
fn merk_proof_op_to_grovedbg(op: Op) -> Result<MerkProofOp, crate::Error> {
    Ok(match op {
        Op::Push(node) => MerkProofOp::Push(merk_proof_node_to_grovedbg(node)?),
        Op::PushInverted(node) => MerkProofOp::PushInverted(merk_proof_node_to_grovedbg(node)?),
        Op::Parent => MerkProofOp::Parent,
        Op::Child => MerkProofOp::Child,
        Op::ParentInverted => MerkProofOp::ParentInverted,
        Op::ChildInverted => MerkProofOp::ChildInverted,
    })
}

fn merk_proof_node_to_grovedbg(node: Node) -> Result<MerkProofNode, crate::Error> {
    Ok(match node {
        Node::Hash(hash) => MerkProofNode::Hash(hash),
        Node::KVHash(hash) => MerkProofNode::KVHash(hash),
        Node::KVDigest(key, hash) => MerkProofNode::KVDigest(key, hash),
        Node::KVDigestCount(key, hash, count) => {
            // KVDigestCount is like KVDigest but with count for ProvableCountTree
            // Use KVValueHashFeatureType for debug display since grovedbg_types may not
            // have KVDigestCount
            MerkProofNode::KVValueHashFeatureType(
                key,
                grovedbg_types::Element::Item {
                    value: vec![],
                    element_flags: None,
                },
                hash,
                grovedbg_types::TreeFeatureType::ProvableCountedMerkNode(count),
            )
        }
        Node::KV(key, value) => {
            let element = crate::Element::deserialize(&value, GroveVersion::latest())?;
            MerkProofNode::KV(key, element_to_grovedbg(element))
        }
        Node::KVValueHash(key, value, hash) => {
            let element = crate::Element::deserialize(&value, GroveVersion::latest())?;
            MerkProofNode::KVValueHash(key, element_to_grovedbg(element), hash)
        }
        Node::KVValueHashFeatureType(key, value, hash, feature_type) => {
            let element = crate::Element::deserialize(&value, GroveVersion::latest())?;
            let node_feature_type = match feature_type {
                TreeFeatureType::BasicMerkNode => grovedbg_types::TreeFeatureType::BasicMerkNode,
                TreeFeatureType::SummedMerkNode(sum) => {
                    grovedbg_types::TreeFeatureType::SummedMerkNode(sum)
                }
                TreeFeatureType::BigSummedMerkNode(sum) => {
                    grovedbg_types::TreeFeatureType::BigSummedMerkNode(sum)
                }
                TreeFeatureType::CountedMerkNode(count) => {
                    grovedbg_types::TreeFeatureType::CountedMerkNode(count)
                }
                TreeFeatureType::CountedSummedMerkNode(count, sum) => {
                    grovedbg_types::TreeFeatureType::CountedSummedMerkNode(count, sum)
                }
                TreeFeatureType::ProvableCountedMerkNode(count) => {
                    grovedbg_types::TreeFeatureType::ProvableCountedMerkNode(count)
                }
                TreeFeatureType::ProvableCountedSummedMerkNode(count, sum) => {
                    grovedbg_types::TreeFeatureType::ProvableCountedSummedMerkNode(count, sum)
                }
            };
            MerkProofNode::KVValueHashFeatureType(
                key,
                element_to_grovedbg(element),
                hash,
                node_feature_type,
            )
        }
        Node::KVRefValueHash(key, value, hash) => {
            let element = crate::Element::deserialize(&value, GroveVersion::latest())?;
            MerkProofNode::KVRefValueHash(key, element_to_grovedbg(element), hash)
        }
        Node::KVCount(key, value, count) => {
            let element = crate::Element::deserialize(&value, GroveVersion::latest())?;
            let val_hash = value_hash(&value).unwrap();
            MerkProofNode::KVValueHashFeatureType(
                key,
                element_to_grovedbg(element),
                val_hash,
                grovedbg_types::TreeFeatureType::ProvableCountedMerkNode(count),
            )
        }
        Node::KVHashCount(hash, count) => MerkProofNode::KVValueHashFeatureType(
            vec![],
            grovedbg_types::Element::Item {
                value: vec![],
                element_flags: None,
            },
            hash,
            grovedbg_types::TreeFeatureType::ProvableCountedMerkNode(count),
        ),
        Node::KVRefValueHashCount(key, value, hash, count) => {
            let element = crate::Element::deserialize(&value, GroveVersion::latest())?;
            // Note: Treating as KVValueHashFeatureType for debug display purposes
            // since grovedbg_types may not have KVRefValueHashCount
            MerkProofNode::KVValueHashFeatureType(
                key,
                element_to_grovedbg(element),
                hash,
                grovedbg_types::TreeFeatureType::ProvableCountedMerkNode(count),
            )
        }
    })
}

fn prove_options_to_grovedbg(options: ProveOptions) -> grovedbg_types::ProveOptions {
    grovedbg_types::ProveOptions {
        decrease_limit_on_empty_sub_query_result: options.decrease_limit_on_empty_sub_query_result,
    }
}

fn path_query_to_grovedb(query: PathQuery) -> crate::PathQuery {
    let PathQuery {
        path,
        query:
            SizedQuery {
                limit,
                offset,
                query: inner_query,
            },
    } = query;

    crate::PathQuery {
        path,
        query: crate::SizedQuery {
            query: query_to_grovedb(inner_query),
            limit,
            offset,
        },
    }
}

fn query_to_grovedb(query: Query) -> crate::Query {
    crate::Query {
        items: query.items.into_iter().map(query_item_to_grovedb).collect(),
        default_subquery_branch: subquery_branch_to_grovedb(query.default_subquery_branch),
        conditional_subquery_branches: conditional_subquery_branches_to_grovedb(
            query.conditional_subquery_branches,
        ),
        left_to_right: query.left_to_right,
        add_parent_tree_on_subquery: false,
    }
}

fn conditional_subquery_branches_to_grovedb(
    conditional_subquery_branches: Vec<(QueryItem, SubqueryBranch)>,
) -> Option<IndexMap<crate::QueryItem, grovedb_merk::proofs::query::SubqueryBranch>> {
    if conditional_subquery_branches.is_empty() {
        None
    } else {
        Some(
            conditional_subquery_branches
                .into_iter()
                .map(|(item, branch)| {
                    (
                        query_item_to_grovedb(item),
                        subquery_branch_to_grovedb(branch),
                    )
                })
                .collect(),
        )
    }
}

fn subquery_branch_to_grovedb(
    subquery_branch: SubqueryBranch,
) -> grovedb_merk::proofs::query::SubqueryBranch {
    grovedb_merk::proofs::query::SubqueryBranch {
        subquery_path: subquery_branch.subquery_path,
        subquery: subquery_branch
            .subquery
            .map(|q| Box::new(query_to_grovedb(*q))),
    }
}

fn query_item_to_grovedb(item: QueryItem) -> crate::QueryItem {
    match item {
        QueryItem::Key(x) => crate::QueryItem::Key(x),
        QueryItem::Range { start, end } => crate::QueryItem::Range(start..end),
        QueryItem::RangeInclusive { start, end } => crate::QueryItem::RangeInclusive(start..=end),
        QueryItem::RangeFull => crate::QueryItem::RangeFull(..),
        QueryItem::RangeFrom(x) => crate::QueryItem::RangeFrom(x..),
        QueryItem::RangeTo(x) => crate::QueryItem::RangeTo(..x),
        QueryItem::RangeToInclusive(x) => crate::QueryItem::RangeToInclusive(..=x),
        QueryItem::RangeAfter(x) => crate::QueryItem::RangeAfter(x..),
        QueryItem::RangeAfterTo { after, to } => crate::QueryItem::RangeAfterTo(after..to),
        QueryItem::RangeAfterToInclusive { after, to } => {
            crate::QueryItem::RangeAfterToInclusive(after..=to)
        }
    }
}

fn element_to_grovedbg(element: crate::Element) -> grovedbg_types::Element {
    match element {
        crate::Element::Item(value, element_flags) => grovedbg_types::Element::Item {
            value,
            element_flags,
        },
        crate::Element::Tree(root_key, element_flags) => grovedbg_types::Element::Subtree {
            root_key,
            element_flags,
        },
        crate::Element::Reference(
            ReferencePathType::AbsolutePathReference(path),
            _,
            element_flags,
        ) => grovedbg_types::Element::Reference(grovedbg_types::Reference::AbsolutePathReference {
            path,
            element_flags,
        }),
        crate::Element::Reference(
            ReferencePathType::UpstreamRootHeightReference(n_keep, path_append),
            _,
            element_flags,
        ) => grovedbg_types::Element::Reference(
            grovedbg_types::Reference::UpstreamRootHeightReference {
                n_keep: n_keep.into(),
                path_append,
                element_flags,
            },
        ),
        crate::Element::Reference(
            ReferencePathType::UpstreamRootHeightWithParentPathAdditionReference(
                n_keep,
                path_append,
            ),
            _,
            element_flags,
        ) => grovedbg_types::Element::Reference(
            grovedbg_types::Reference::UpstreamRootHeightWithParentPathAdditionReference {
                n_keep: n_keep.into(),
                path_append,
                element_flags,
            },
        ),
        crate::Element::Reference(
            ReferencePathType::UpstreamFromElementHeightReference(n_remove, path_append),
            _,
            element_flags,
        ) => grovedbg_types::Element::Reference(
            grovedbg_types::Reference::UpstreamFromElementHeightReference {
                n_remove: n_remove.into(),
                path_append,
                element_flags,
            },
        ),
        crate::Element::Reference(
            ReferencePathType::CousinReference(swap_parent),
            _,
            element_flags,
        ) => grovedbg_types::Element::Reference(grovedbg_types::Reference::CousinReference {
            swap_parent,
            element_flags,
        }),
        crate::Element::Reference(
            ReferencePathType::RemovedCousinReference(swap_parent),
            _,
            element_flags,
        ) => {
            grovedbg_types::Element::Reference(grovedbg_types::Reference::RemovedCousinReference {
                swap_parent,
                element_flags,
            })
        }
        crate::Element::Reference(
            ReferencePathType::SiblingReference(sibling_key),
            _,
            element_flags,
        ) => grovedbg_types::Element::Reference(grovedbg_types::Reference::SiblingReference {
            sibling_key,
            element_flags,
        }),
        crate::Element::SumItem(value, element_flags) => grovedbg_types::Element::SumItem {
            value,
            element_flags,
        },
        crate::Element::ItemWithSumItem(value, sum_value, element_flags) => {
            grovedbg_types::Element::ItemWithSumItem {
                value,
                sum_item_value: sum_value,
                element_flags,
            }
        }
        crate::Element::SumTree(root_key, sum, element_flags) => grovedbg_types::Element::Sumtree {
            root_key,
            sum,
            element_flags,
        },
        crate::Element::BigSumTree(root_key, sum, element_flags) => {
            grovedbg_types::Element::BigSumTree {
                root_key,
                sum,
                element_flags,
            }
        }
        crate::Element::CountTree(root_key, count, element_flags) => {
            grovedbg_types::Element::CountTree {
                root_key,
                count,
                element_flags,
            }
        }
        crate::Element::CountSumTree(root_key, count, sum, element_flags) => {
            grovedbg_types::Element::CountSumTree {
                root_key,
                count,
                sum,
                element_flags,
            }
        }
        crate::Element::ProvableCountTree(root_key, count, element_flags) => {
            grovedbg_types::Element::ProvableCountTree {
                root_key,
                count,
                element_flags,
            }
        }
        crate::Element::ProvableCountSumTree(root_key, count, sum, element_flags) => {
            grovedbg_types::Element::ProvableCountSumTree {
                root_key,
                count,
                sum,
                element_flags,
            }
        }
    }
}

fn node_to_update(
    path: Path,
    NodeDbg {
        key,
        value,
        left_child,
        left_merk_hash,
        right_child,
        right_merk_hash,
        value_hash,
        kv_digest_hash,
        feature_type,
    }: NodeDbg,
) -> Result<NodeUpdate, crate::Error> {
    let grovedb_element = crate::Element::deserialize(&value, GroveVersion::latest())?;

    let element = element_to_grovedbg(grovedb_element);

    Ok(NodeUpdate {
        path,
        key,
        element,
        left_child,
        left_merk_hash,
        right_child,
        right_merk_hash,
        feature_type: match feature_type {
            TreeFeatureType::BasicMerkNode => grovedbg_types::TreeFeatureType::BasicMerkNode,
            TreeFeatureType::SummedMerkNode(sum) => {
                grovedbg_types::TreeFeatureType::SummedMerkNode(sum)
            }
            TreeFeatureType::BigSummedMerkNode(sum) => {
                grovedbg_types::TreeFeatureType::BigSummedMerkNode(sum)
            }
            TreeFeatureType::CountedMerkNode(count) => {
                grovedbg_types::TreeFeatureType::CountedMerkNode(count)
            }
            TreeFeatureType::CountedSummedMerkNode(count, sum) => {
                grovedbg_types::TreeFeatureType::CountedSummedMerkNode(count, sum)
            }
            TreeFeatureType::ProvableCountedMerkNode(count) => {
                grovedbg_types::TreeFeatureType::ProvableCountedMerkNode(count)
            }
            TreeFeatureType::ProvableCountedSummedMerkNode(count, sum) => {
                grovedbg_types::TreeFeatureType::ProvableCountedSummedMerkNode(count, sum)
            }
        },
        value_hash,
        kv_digest_hash,
    })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn element_to_grovedbg_converts_item_with_sum_item() {
        let flags = Some(vec![1, 2, 3]);
        let element = crate::Element::ItemWithSumItem(b"dbg".to_vec(), -5, flags.clone());
        match element_to_grovedbg(element) {
            grovedbg_types::Element::ItemWithSumItem {
                value,
                sum_item_value,
                element_flags,
            } => {
                assert_eq!(value, b"dbg");
                assert_eq!(sum_item_value, -5);
                assert_eq!(element_flags, flags);
            }
            _ => panic!("unexpected debugger conversion"),
        }
    }
}
