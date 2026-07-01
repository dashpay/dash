use std::collections::VecDeque;

use ed::Encode;
use grovedb_costs::CostsExt;
use grovedb_storage::StorageContext;
use grovedb_version::version::GroveVersion;

use crate::{
    error::Error,
    proofs::{
        chunk::{
            chunk_op::ChunkOp,
            error::ChunkError,
            util::{
                chunk_height, chunk_index_from_traversal_instruction,
                chunk_index_from_traversal_instruction_with_recovery,
                generate_traversal_instruction, generate_traversal_instruction_as_vec_bytes,
                number_of_chunks, vec_bytes_as_traversal_instruction,
            },
        },
        Node, Op,
    },
    Error::ChunkingError,
    Merk,
};

/// ChunkProof for replication of a single subtree
#[derive(Debug)]
pub struct SubtreeChunk {
    chunk: Vec<Op>,
    next_index: Option<usize>,
    remaining_limit: Option<usize>,
}

impl SubtreeChunk {
    pub fn new(chunk: Vec<Op>, next_index: Option<usize>, remaining_limit: Option<usize>) -> Self {
        Self {
            chunk,
            next_index,
            remaining_limit,
        }
    }
}

/// ChunkProof for the replication of multiple subtrees.
#[derive(Debug)]
pub struct MultiChunk {
    pub chunk: Vec<ChunkOp>,
    pub next_index: Option<Vec<u8>>,
    pub remaining_limit: Option<usize>,
}

impl MultiChunk {
    pub fn new(
        chunk: Vec<ChunkOp>,
        next_index: Option<Vec<u8>>,
        remaining_limit: Option<usize>,
    ) -> Self {
        Self {
            chunk,
            next_index,
            remaining_limit,
        }
    }
}

/// A `ChunkProducer` allows the creation of chunk proofs, used for trustlessly
/// replicating entire Merk trees. Chunks can be generated on the fly in a
/// random order, or iterated in order for slightly better performance.
pub struct ChunkProducer<'db, S> {
    /// Represents the max height of the Merk tree
    height: usize,
    /// Represents the index of the next chunk
    index: usize,
    merk: &'db Merk<S>,
}

impl<'db, S> ChunkProducer<'db, S>
where
    S: StorageContext<'db>,
{
    /// Creates a new `ChunkProducer` for the given `Merk` instance
    pub fn new(merk: &'db Merk<S>) -> Result<Self, Error> {
        let tree_height = merk
            .height()
            .ok_or(Error::ChunkingError(ChunkError::EmptyTree(
                "cannot create chunk producer for empty Merk",
            )))?;
        Ok(Self {
            height: tree_height as usize,
            index: 1,
            merk,
        })
    }

    /// Gets the chunk with the given index. Errors if the index is out of
    /// bounds or the tree is empty - the number of chunks can be checked by
    /// calling `producer.len()`.
    pub fn chunk_with_index(
        &mut self,
        chunk_index: usize,
        grove_version: &GroveVersion,
    ) -> Result<(Vec<Op>, Option<usize>), Error> {
        let traversal_instructions = generate_traversal_instruction(self.height, chunk_index)?;
        self.chunk_internal(chunk_index, traversal_instructions, grove_version)
    }

    /// Returns the chunk at a given chunk id.
    pub fn chunk(
        &mut self,
        chunk_id: &[u8],
        grove_version: &GroveVersion,
    ) -> Result<(Vec<Op>, Option<Vec<u8>>), Error> {
        let traversal_instructions = vec_bytes_as_traversal_instruction(chunk_id)?;
        let chunk_index = chunk_index_from_traversal_instruction_with_recovery(
            traversal_instructions.as_slice(),
            self.height,
        )?;
        let (chunk, next_index) =
            self.chunk_internal(chunk_index, traversal_instructions, grove_version)?;
        let next_chunk_id = next_index
            .map(|index| generate_traversal_instruction_as_vec_bytes(self.height, index))
            .transpose()?;
        Ok((chunk, next_chunk_id))
    }

    /// Returns the chunk at the given index
    /// Assumes index and traversal_instructions represents the same information
    fn chunk_internal(
        &mut self,
        index: usize,
        traversal_instructions: Vec<bool>,
        grove_version: &GroveVersion,
    ) -> Result<(Vec<Op>, Option<usize>), Error> {
        // ensure that the chunk index is within bounds
        let max_chunk_index = self.len();
        if index < 1 || index > max_chunk_index {
            return Err(ChunkingError(ChunkError::OutOfBounds(
                "chunk index out of bounds",
            )));
        }

        self.index = index + 1;

        let chunk_height = chunk_height(self.height, index).unwrap();

        let tree_type = self.merk.tree_type;
        let chunk = self
            .merk
            .walk(|maybe_walker| match maybe_walker {
                Some(mut walker) => walker.traverse_and_build_chunk(
                    &traversal_instructions,
                    chunk_height,
                    tree_type,
                    grove_version,
                ),
                None => Err(Error::ChunkingError(ChunkError::EmptyTree(
                    "cannot create chunk producer for empty Merk",
                )))
                .wrap_with_cost(Default::default()),
            })
            .unwrap()?;

        // now we need to return the next index
        // how do we know if we should return some or none
        if self.index > max_chunk_index {
            Ok((chunk, None))
        } else {
            Ok((chunk, Some(self.index)))
        }
    }

    /// Generate multichunk with chunk id
    /// Multichunks accumulate as many chunks as they can until they have all
    /// chunks or hit some optional limit
    pub fn multi_chunk_with_limit(
        &mut self,
        chunk_id: &[u8],
        limit: Option<usize>,
        grove_version: &GroveVersion,
    ) -> Result<MultiChunk, Error> {
        // we want to convert the chunk id to the index
        let chunk_index = vec_bytes_as_traversal_instruction(chunk_id).and_then(|instruction| {
            chunk_index_from_traversal_instruction(instruction.as_slice(), self.height)
        })?;
        self.multi_chunk_with_limit_and_index(chunk_index, limit, grove_version)
    }

    /// Generate multichunk with chunk index
    /// Multichunks accumulate as many chunks as they can until they have all
    /// chunks or hit some optional limit
    pub fn multi_chunk_with_limit_and_index(
        &mut self,
        index: usize,
        limit: Option<usize>,
        grove_version: &GroveVersion,
    ) -> Result<MultiChunk, Error> {
        // TODO: what happens if the vec is filled?
        //  we need to have some kind of hardhoc limit value if none is supplied.
        //  maybe we can just do something with the length to fix this?
        let mut chunk = vec![];

        let mut current_index = Some(index);
        let mut current_limit = limit;

        // generate as many subtree chunks as we can
        // until we have exhausted all or hit a limit restriction
        while current_index.is_some() {
            let current_index_traversal_instruction = generate_traversal_instruction(
                self.height,
                current_index.expect("confirmed is Some"),
            )?;
            let chunk_id_op = ChunkOp::ChunkId(current_index_traversal_instruction);

            // factor in the ChunkId encoding length in limit calculations
            let temp_limit = if let Some(limit) = current_limit {
                let chunk_id_op_encoding_len = chunk_id_op.encoding_length().map_err(|_e| {
                    Error::ChunkingError(ChunkError::InternalError("cannot get encoding length"))
                })?;
                if limit >= chunk_id_op_encoding_len {
                    Some(limit - chunk_id_op_encoding_len)
                } else {
                    Some(0)
                }
            } else {
                None
            };

            let subtree_multi_chunk_result = self.subtree_multi_chunk_with_limit(
                current_index.expect("confirmed is not None"),
                temp_limit,
                grove_version,
            );

            let limit_too_small_error = matches!(
                subtree_multi_chunk_result,
                Err(ChunkingError(ChunkError::LimitTooSmall(..)))
            );

            if limit_too_small_error {
                if chunk.is_empty() {
                    // no progress, return limit too small error
                    return Err(Error::ChunkingError(ChunkError::LimitTooSmall(
                        "limit too small for initial chunk",
                    )));
                } else {
                    // made progress, send accumulated chunk
                    break;
                }
            }

            let subtree_multi_chunk = subtree_multi_chunk_result?;

            chunk.push(chunk_id_op);
            chunk.push(ChunkOp::Chunk(subtree_multi_chunk.chunk));

            // update loop parameters
            current_index = subtree_multi_chunk.next_index;
            current_limit = subtree_multi_chunk.remaining_limit;
        }

        let index_bytes = current_index
            .map(|index| generate_traversal_instruction_as_vec_bytes(self.height, index))
            .transpose()?;

        Ok(MultiChunk::new(chunk, index_bytes, current_limit))
    }

    /// Packs as many chunks as it can from a starting chunk index, into a
    /// vector. Stops when we have exhausted all chunks or we have reached
    /// some limit.
    fn subtree_multi_chunk_with_limit(
        &mut self,
        index: usize,
        limit: Option<usize>,
        grove_version: &GroveVersion,
    ) -> Result<SubtreeChunk, Error> {
        let max_chunk_index = number_of_chunks(self.height);
        let mut chunk_index = index;

        // we first get the chunk at the given index
        // TODO: use the returned chunk index rather than tracking
        let (chunk_ops, _) = self.chunk_with_index(chunk_index, grove_version)?;
        let mut chunk_byte_length = chunk_ops.encoding_length().map_err(|_e| {
            Error::ChunkingError(ChunkError::InternalError("can't get encoding length"))
        })?;
        chunk_index += 1;

        let mut chunk = VecDeque::from(chunk_ops);

        // ensure the limit is not less than first chunk byte length
        // if it is we can't proceed and didn't make progress so we return an error
        if let Some(limit) = limit {
            if chunk_byte_length > limit {
                return Err(Error::ChunkingError(ChunkError::LimitTooSmall(
                    "limit too small for initial chunk",
                )));
            }
        }

        let mut iteration_index = 0;
        while iteration_index < chunk.len() {
            // we only perform replacements on Hash nodes
            if matches!(chunk[iteration_index], Op::Push(Node::Hash(..))) {
                // TODO: use the returned chunk index rather than tracking
                let (replacement_chunk, _) = self.chunk_with_index(chunk_index, grove_version)?;

                // calculate the new total
                let new_total = replacement_chunk.encoding_length().map_err(|_e| {
                    Error::ChunkingError(ChunkError::InternalError("can't get encoding length"))
                })? + chunk_byte_length
                    - chunk[iteration_index].encoding_length().map_err(|_e| {
                        Error::ChunkingError(ChunkError::InternalError("can't get encoding length"))
                    })?;

                // verify that this chunk doesn't make use exceed the limit
                if let Some(limit) = limit {
                    if new_total > limit {
                        let next_index = match chunk_index > max_chunk_index {
                            true => None,
                            _ => Some(chunk_index),
                        };

                        return Ok(SubtreeChunk::new(
                            chunk.into(),
                            next_index,
                            Some(limit - chunk_byte_length),
                        ));
                    }
                }

                chunk_byte_length = new_total;
                chunk_index += 1;

                chunk.remove(iteration_index);
                for op in replacement_chunk.into_iter().rev() {
                    chunk.insert(iteration_index, op);
                }
            } else {
                iteration_index += 1;
            }
        }

        let remaining_limit = limit.map(|l| l - chunk_byte_length);
        let next_index = match chunk_index > max_chunk_index {
            true => None,
            _ => Some(chunk_index),
        };

        Ok(SubtreeChunk::new(chunk.into(), next_index, remaining_limit))
    }

    /// Returns the total number of chunks for the underlying Merk tree.
    pub fn len(&self) -> usize {
        number_of_chunks(self.height)
    }

    pub fn is_empty(&self) -> bool {
        number_of_chunks(self.height) == 0
    }

    /// Gets the next chunk based on the `ChunkProducer`'s internal index state.
    /// This is mostly useful for letting `ChunkIter` yield the chunks in order,
    /// optimizing throughput compared to random access.
    // TODO: this is not better than random access, as we are not keeping state
    //  that will make this more efficient, decide if this should be fixed or not
    fn next_chunk(
        &mut self,
        grove_version: &GroveVersion,
    ) -> Option<Result<(Vec<Op>, Option<Vec<u8>>), Error>> {
        let max_index = number_of_chunks(self.height);
        if self.index > max_index {
            return None;
        }

        // get the chunk at the given index
        // return the next index as a string
        Some(
            self.chunk_with_index(self.index, grove_version)
                .and_then(|(chunk, chunk_index)| {
                    chunk_index
                        .map(|index| {
                            generate_traversal_instruction_as_vec_bytes(self.height, index)
                        })
                        .transpose()
                        .map(|v| (chunk, v))
                }),
        )
    }
}

/// Iterate over each chunk, returning `None` after last chunk
impl<'db, S> ChunkProducer<'db, S>
where
    S: StorageContext<'db>,
{
    pub fn next(
        &mut self,
        grove_version: &GroveVersion,
    ) -> Option<Result<(Vec<Op>, Option<Vec<u8>>), Error>> {
        self.next_chunk(grove_version)
    }
}

impl<'db, S> Merk<S>
where
    S: StorageContext<'db>,
{
    /// Creates a `ChunkProducer` which can return chunk proofs for replicating
    /// the entire Merk tree.
    pub fn chunks(&'db self) -> Result<ChunkProducer<'db, S>, Error> {
        ChunkProducer::new(self)
    }
}

#[cfg(test)]
mod test {
    use super::*;
    use crate::{
        proofs::{
            chunk::{
                chunk::{
                    tests::{traverse_get_kv_feature_type, traverse_get_node_hash},
                    LEFT, RIGHT,
                },
                util::traversal_instruction_as_vec_bytes,
            },
            tree::execute,
            Tree,
        },
        test_utils::{make_batch_seq, TempMerk},
        tree::RefWalker,
        PanicSource,
    };

    #[derive(Default)]
    struct NodeCounts {
        hash: usize,
        kv_hash: usize,
        kv: usize,
        kv_value_hash: usize,
        kv_digest: usize,
        kv_ref_value_hash: usize,
        kv_value_hash_feature_type: usize,
    }

    impl NodeCounts {
        fn sum(&self) -> usize {
            self.hash
                + self.kv_hash
                + self.kv
                + self.kv_value_hash
                + self.kv_digest
                + self.kv_ref_value_hash
                + self.kv_value_hash_feature_type
        }
    }

    fn count_node_types(tree: Tree) -> NodeCounts {
        let mut counts = NodeCounts::default();

        tree.visit_nodes(&mut |node| {
            match node {
                Node::Hash(_) => counts.hash += 1,
                Node::KVHash(_) => counts.kv_hash += 1,
                Node::KV(..) => counts.kv += 1,
                Node::KVValueHash(..) => counts.kv_value_hash += 1,
                Node::KVDigest(..) => counts.kv_digest += 1,
                Node::KVDigestCount(..) => counts.kv_digest += 1,
                Node::KVRefValueHash(..) => counts.kv_ref_value_hash += 1,
                Node::KVValueHashFeatureType(..) => counts.kv_value_hash_feature_type += 1,
                Node::KVCount(..) => counts.kv += 1,
                Node::KVHashCount(..) => counts.kv_hash += 1,
                Node::KVRefValueHashCount(..) => counts.kv_ref_value_hash += 1,
            };
        });

        counts
    }

    #[test]
    fn test_merk_chunk_len() {
        let grove_version = GroveVersion::latest();
        // Tree of height 5 - max of 31 elements, min of 16 elements
        // 5 will be broken into 2 layers = [3, 2]
        // exit nodes from first layer = 2^3 = 8
        // total_chunk = 1 + 8 = 9 chunks
        let mut merk = TempMerk::new(grove_version);
        let batch = make_batch_seq(0..20);
        merk.apply::<_, Vec<_>>(&batch, &[], None, grove_version)
            .unwrap()
            .expect("apply failed");
        assert_eq!(merk.height(), Some(5));
        let chunk_producer = ChunkProducer::new(&merk).unwrap();
        assert_eq!(chunk_producer.len(), 9);

        // Tree of height 10 - max of 1023 elements, min of 512 elements
        // 4 layers -> [3,3,2,2]
        // chunk_count_per_layer -> [1, 8, 64, 256]
        // total = 341 chunks
        let mut merk = TempMerk::new(grove_version);
        let batch = make_batch_seq(0..1000);
        merk.apply::<_, Vec<_>>(&batch, &[], None, grove_version)
            .unwrap()
            .expect("apply failed");
        assert_eq!(merk.height(), Some(10));
        let chunk_producer = ChunkProducer::new(&merk).unwrap();
        assert_eq!(chunk_producer.len(), 329);
    }

    #[test]
    fn test_chunk_producer_iter() {
        let grove_version = GroveVersion::latest();
        // tree with height 4
        // full tree
        //              7
        //           /      \
        //        3            11
        //      /   \        /    \
        //     1     5      9      13
        //   /  \   / \    / \    /   \
        //  0   2  4   6  8  10  12   14
        // going to be broken into [2, 2]
        // that's a total of 5 chunks

        let mut merk = TempMerk::new(grove_version);
        let batch = make_batch_seq(0..15);
        merk.apply::<_, Vec<_>>(&batch, &[], None, grove_version)
            .unwrap()
            .expect("apply failed");
        assert_eq!(merk.height(), Some(4));

        let mut chunk_producer = ChunkProducer::new(&merk).expect("should create chunk producer");

        // build iterator from first chunk producer
        let mut chunks = merk.chunks().expect("should return producer");

        // ensure that the chunks gotten from the iterator is the same
        // as that from the chunk producer
        for i in 1..=5 {
            assert_eq!(
                chunks.next(grove_version).unwrap().unwrap().0,
                chunk_producer.chunk_with_index(i, grove_version).unwrap().0
            );
        }

        // returns None after max
        assert!(chunks.next(grove_version).is_none());
    }

    #[test]
    fn test_random_chunk_access() {
        let grove_version = GroveVersion::latest();
        // tree with height 4
        // full tree
        //              7
        //           /      \
        //        3            11
        //      /   \        /    \
        //     1     5      9      13
        //   /  \   / \    / \    /   \
        //  0   2  4   6  8  10  12   14
        // going to be broken into [2, 2]
        // that's a total of 5 chunks

        let mut merk = TempMerk::new(grove_version);
        let batch = make_batch_seq(0..15);
        merk.apply::<_, Vec<_>>(&batch, &[], None, grove_version)
            .unwrap()
            .expect("apply failed");
        assert_eq!(merk.height(), Some(4));

        let mut inner_tree = merk.tree.take().expect("has inner tree");
        merk.tree.set(Some(inner_tree.clone()));

        // TODO: should I be using panic source?
        let mut tree_walker = RefWalker::new(&mut inner_tree, PanicSource {});

        let mut chunk_producer = ChunkProducer::new(&merk).expect("should create chunk producer");
        assert_eq!(chunk_producer.len(), 5);

        // assert bounds
        assert!(chunk_producer.chunk_with_index(0, grove_version).is_err());
        assert!(chunk_producer.chunk_with_index(6, grove_version).is_err());

        // first chunk
        // expected:
        //              7
        //           /      \
        //        3            11
        //      /   \        /    \
        //   H(1)   H(5)    H(9)   H(13)
        let (chunk, next_chunk) = chunk_producer
            .chunk_with_index(1, grove_version)
            .expect("should generate chunk");
        assert_eq!(chunk.len(), 13);
        assert_eq!(next_chunk, Some(2));
        assert_eq!(
            chunk,
            vec![
                Op::Push(traverse_get_node_hash(
                    &mut tree_walker,
                    &[LEFT, LEFT],
                    grove_version
                )),
                Op::Push(traverse_get_kv_feature_type(
                    &mut tree_walker,
                    &[LEFT],
                    grove_version
                )),
                Op::Parent,
                Op::Push(traverse_get_node_hash(
                    &mut tree_walker,
                    &[LEFT, RIGHT],
                    grove_version
                )),
                Op::Child,
                Op::Push(traverse_get_kv_feature_type(
                    &mut tree_walker,
                    &[],
                    grove_version
                )),
                Op::Parent,
                Op::Push(traverse_get_node_hash(
                    &mut tree_walker,
                    &[RIGHT, LEFT],
                    grove_version
                )),
                Op::Push(traverse_get_kv_feature_type(
                    &mut tree_walker,
                    &[RIGHT],
                    grove_version
                )),
                Op::Parent,
                Op::Push(traverse_get_node_hash(
                    &mut tree_walker,
                    &[RIGHT, RIGHT],
                    grove_version
                )),
                Op::Child,
                Op::Child
            ]
        );

        // second chunk
        // expected:
        //         1
        //        /  \
        //       0    2
        let (chunk, next_chunk) = chunk_producer
            .chunk_with_index(2, grove_version)
            .expect("should generate chunk");
        assert_eq!(chunk.len(), 5);
        assert_eq!(next_chunk, Some(3));
        assert_eq!(
            chunk,
            vec![
                Op::Push(traverse_get_kv_feature_type(
                    &mut tree_walker,
                    &[LEFT, LEFT, LEFT],
                    grove_version
                )),
                Op::Push(traverse_get_kv_feature_type(
                    &mut tree_walker,
                    &[LEFT, LEFT],
                    grove_version
                )),
                Op::Parent,
                Op::Push(traverse_get_kv_feature_type(
                    &mut tree_walker,
                    &[LEFT, LEFT, RIGHT],
                    grove_version
                )),
                Op::Child
            ]
        );

        // third chunk
        // expected:
        //         5
        //        /  \
        //       4    6
        let (chunk, next_chunk) = chunk_producer
            .chunk_with_index(3, grove_version)
            .expect("should generate chunk");
        assert_eq!(chunk.len(), 5);
        assert_eq!(next_chunk, Some(4));
        assert_eq!(
            chunk,
            vec![
                Op::Push(traverse_get_kv_feature_type(
                    &mut tree_walker,
                    &[LEFT, RIGHT, LEFT],
                    grove_version
                )),
                Op::Push(traverse_get_kv_feature_type(
                    &mut tree_walker,
                    &[LEFT, RIGHT],
                    grove_version
                )),
                Op::Parent,
                Op::Push(traverse_get_kv_feature_type(
                    &mut tree_walker,
                    &[LEFT, RIGHT, RIGHT],
                    grove_version
                )),
                Op::Child
            ]
        );

        // third chunk
        // expected:
        //         9
        //        /  \
        //       8    10
        let (chunk, next_chunk) = chunk_producer
            .chunk_with_index(4, grove_version)
            .expect("should generate chunk");
        assert_eq!(chunk.len(), 5);
        assert_eq!(next_chunk, Some(5));
        assert_eq!(
            chunk,
            vec![
                Op::Push(traverse_get_kv_feature_type(
                    &mut tree_walker,
                    &[RIGHT, LEFT, LEFT],
                    grove_version
                )),
                Op::Push(traverse_get_kv_feature_type(
                    &mut tree_walker,
                    &[RIGHT, LEFT],
                    grove_version
                )),
                Op::Parent,
                Op::Push(traverse_get_kv_feature_type(
                    &mut tree_walker,
                    &[RIGHT, LEFT, RIGHT],
                    grove_version
                )),
                Op::Child
            ]
        );

        // third chunk
        // expected:
        //         13
        //        /  \
        //       12    14
        let (chunk, next_chunk) = chunk_producer
            .chunk_with_index(5, grove_version)
            .expect("should generate chunk");
        assert_eq!(chunk.len(), 5);
        assert_eq!(next_chunk, None);
        assert_eq!(
            chunk,
            vec![
                Op::Push(traverse_get_kv_feature_type(
                    &mut tree_walker,
                    &[RIGHT, RIGHT, LEFT],
                    grove_version
                )),
                Op::Push(traverse_get_kv_feature_type(
                    &mut tree_walker,
                    &[RIGHT, RIGHT],
                    grove_version
                )),
                Op::Parent,
                Op::Push(traverse_get_kv_feature_type(
                    &mut tree_walker,
                    &[RIGHT, RIGHT, RIGHT],
                    grove_version
                )),
                Op::Child
            ]
        );
    }

    #[test]
    fn test_subtree_chunk_no_limit() {
        let grove_version = GroveVersion::latest();
        // tree of height 4
        // 5 chunks
        let mut merk = TempMerk::new(grove_version);
        let batch = make_batch_seq(0..15);
        merk.apply::<_, Vec<_>>(&batch, &[], None, grove_version)
            .unwrap()
            .expect("apply failed");
        assert_eq!(merk.height(), Some(4));

        // generate multi chunk with no limit
        let mut chunk_producer = ChunkProducer::new(&merk).expect("should create chunk producer");
        let chunk_result = chunk_producer
            .subtree_multi_chunk_with_limit(1, None, grove_version)
            .expect("should generate chunk with limit");

        assert_eq!(chunk_result.remaining_limit, None);
        assert_eq!(chunk_result.next_index, None);

        let tree = execute(chunk_result.chunk.into_iter().map(Ok), false, |_| Ok(()))
            .unwrap()
            .expect("should reconstruct tree");
        assert_eq!(tree.hash().unwrap(), merk.root_hash().unwrap());

        // assert that all nodes are of type kv_value_hash_feature_type
        let node_counts = count_node_types(tree);
        assert_eq!(node_counts.hash, 0);
        assert_eq!(node_counts.kv_hash, 0);
        assert_eq!(node_counts.kv, 0);
        assert_eq!(node_counts.kv_value_hash, 0);
        assert_eq!(node_counts.kv_digest, 0);
        assert_eq!(node_counts.kv_ref_value_hash, 0);
        assert_eq!(node_counts.kv_value_hash_feature_type, 15);
    }

    #[test]
    fn test_subtree_chunk_with_limit() {
        let grove_version = GroveVersion::latest();
        // tree of height 4
        // 5 chunks
        let mut merk = TempMerk::new(grove_version);
        let batch = make_batch_seq(0..15);
        merk.apply::<_, Vec<_>>(&batch, &[], None, grove_version)
            .unwrap()
            .expect("apply failed");
        assert_eq!(merk.height(), Some(4));

        let mut chunk_producer = ChunkProducer::new(&merk).expect("should create chunk producer");

        // initial chunk is of size 453, so limit of 10 is too small
        // should return an error
        let chunk = chunk_producer.subtree_multi_chunk_with_limit(1, Some(10), grove_version);
        assert!(chunk.is_err());

        // get just the fist chunk
        let chunk_result = chunk_producer
            .subtree_multi_chunk_with_limit(1, Some(453), grove_version)
            .expect("should generate chunk with limit");
        assert_eq!(chunk_result.remaining_limit, Some(0));
        assert_eq!(chunk_result.next_index, Some(2));

        let chunk = chunk_result.chunk;
        assert_eq!(chunk.encoding_length().unwrap(), 453);
        assert_eq!(chunk.len(), 13); // op count
        let tree = execute(chunk.into_iter().map(Ok), false, |_| Ok(()))
            .unwrap()
            .expect("should reconstruct tree");
        assert_eq!(tree.hash().unwrap(), merk.root_hash().unwrap());

        let node_counts = count_node_types(tree);
        assert_eq!(node_counts.kv_value_hash_feature_type, 3);
        assert_eq!(node_counts.hash, 4);
        assert_eq!(node_counts.sum(), 4 + 3);

        // get up to second chunk
        let chunk_result = chunk_producer
            .subtree_multi_chunk_with_limit(1, Some(737), grove_version)
            .expect("should generate chunk with limit");
        assert_eq!(chunk_result.remaining_limit, Some(0));
        assert_eq!(chunk_result.next_index, Some(3));

        let chunk = chunk_result.chunk;
        assert_eq!(chunk.encoding_length().unwrap(), 737);
        assert_eq!(chunk.len(), 17); // op count
        let tree = execute(chunk.into_iter().map(Ok), false, |_| Ok(()))
            .unwrap()
            .expect("should reconstruct tree");
        assert_eq!(tree.hash().unwrap(), merk.root_hash().unwrap());

        let node_counts = count_node_types(tree);
        assert_eq!(node_counts.kv_value_hash_feature_type, 6);
        assert_eq!(node_counts.hash, 3);
        assert_eq!(node_counts.sum(), 6 + 3);

        // get up to third chunk
        let chunk_result = chunk_producer
            .subtree_multi_chunk_with_limit(1, Some(1021), grove_version)
            .expect("should generate chunk with limit");
        assert_eq!(chunk_result.remaining_limit, Some(0));
        assert_eq!(chunk_result.next_index, Some(4));

        let chunk = chunk_result.chunk;
        assert_eq!(chunk.encoding_length().unwrap(), 1021);
        assert_eq!(chunk.len(), 21); // op count
        let tree = execute(chunk.into_iter().map(Ok), false, |_| Ok(()))
            .unwrap()
            .expect("should reconstruct tree");
        assert_eq!(tree.hash().unwrap(), merk.root_hash().unwrap());

        let node_counts = count_node_types(tree);
        assert_eq!(node_counts.kv_value_hash_feature_type, 9);
        assert_eq!(node_counts.hash, 2);
        assert_eq!(node_counts.sum(), 9 + 2);

        // get up to fourth chunk
        let chunk_result = chunk_producer
            .subtree_multi_chunk_with_limit(1, Some(1305), grove_version)
            .expect("should generate chunk with limit");
        assert_eq!(chunk_result.remaining_limit, Some(0));
        assert_eq!(chunk_result.next_index, Some(5));

        let chunk = chunk_result.chunk;
        assert_eq!(chunk.encoding_length().unwrap(), 1305);
        assert_eq!(chunk.len(), 25); // op count
        let tree = execute(chunk.into_iter().map(Ok), false, |_| Ok(()))
            .unwrap()
            .expect("should reconstruct tree");
        assert_eq!(tree.hash().unwrap(), merk.root_hash().unwrap());

        let node_counts = count_node_types(tree);
        assert_eq!(node_counts.kv_value_hash_feature_type, 12);
        assert_eq!(node_counts.hash, 1);
        assert_eq!(node_counts.sum(), 12 + 1);

        // get up to fifth chunk
        let chunk_result = chunk_producer
            .subtree_multi_chunk_with_limit(1, Some(1589), grove_version)
            .expect("should generate chunk with limit");
        assert_eq!(chunk_result.remaining_limit, Some(0));
        assert_eq!(chunk_result.next_index, None);

        let chunk = chunk_result.chunk;
        assert_eq!(chunk.encoding_length().unwrap(), 1589);
        assert_eq!(chunk.len(), 29); // op count
        let tree = execute(chunk.into_iter().map(Ok), false, |_| Ok(()))
            .unwrap()
            .expect("should reconstruct tree");
        assert_eq!(tree.hash().unwrap(), merk.root_hash().unwrap());

        let node_counts = count_node_types(tree);
        assert_eq!(node_counts.kv_value_hash_feature_type, 15);
        assert_eq!(node_counts.hash, 0);
        assert_eq!(node_counts.sum(), 15);

        // limit larger than total chunk
        let chunk_result = chunk_producer
            .subtree_multi_chunk_with_limit(1, Some(usize::MAX), grove_version)
            .expect("should generate chunk with limit");
        assert_eq!(chunk_result.remaining_limit, Some(18446744073709550026));
        assert_eq!(chunk_result.next_index, None);

        let chunk = chunk_result.chunk;
        assert_eq!(chunk.encoding_length().unwrap(), 1589);
        assert_eq!(chunk.len(), 29); // op count
        let tree = execute(chunk.into_iter().map(Ok), false, |_| Ok(()))
            .unwrap()
            .expect("should reconstruct tree");
        assert_eq!(tree.hash().unwrap(), merk.root_hash().unwrap());

        let node_counts = count_node_types(tree);
        assert_eq!(node_counts.kv_value_hash_feature_type, 15);
        assert_eq!(node_counts.hash, 0);
        assert_eq!(node_counts.sum(), 15);
    }

    #[test]
    fn test_multi_chunk_with_no_limit_trunk() {
        let grove_version = GroveVersion::latest();
        // tree of height 4
        // 5 chunks
        let mut merk = TempMerk::new(grove_version);
        let batch = make_batch_seq(0..15);
        merk.apply::<_, Vec<_>>(&batch, &[], None, grove_version)
            .unwrap()
            .expect("apply failed");
        assert_eq!(merk.height(), Some(4));

        let mut chunk_producer = ChunkProducer::new(&merk).expect("should create chunk producer");

        // we generate the chunk starting from index 1, this has no hash nodes
        // so no multi chunk will be generated
        let chunk_result = chunk_producer
            .multi_chunk_with_limit_and_index(1, None, grove_version)
            .expect("should generate chunk with limit");

        assert_eq!(chunk_result.remaining_limit, None);
        assert_eq!(chunk_result.next_index, None);

        // should only contain 2 items, the starting chunk id and the entire tree
        assert_eq!(chunk_result.chunk.len(), 2);

        // assert items
        assert_eq!(chunk_result.chunk[0], ChunkOp::ChunkId(vec![]));
        if let ChunkOp::Chunk(chunk) = &chunk_result.chunk[1] {
            let tree = execute(chunk.clone().into_iter().map(Ok), false, |_| Ok(()))
                .unwrap()
                .expect("should reconstruct tree");
            assert_eq!(tree.hash().unwrap(), merk.root_hash().unwrap());
        } else {
            panic!("expected ChunkOp::Chunk");
        }
    }

    #[test]
    fn test_multi_chunk_with_no_limit_not_trunk() {
        let grove_version = GroveVersion::latest();
        // tree of height 4
        // 5 chunks
        let mut merk = TempMerk::new(grove_version);
        let batch = make_batch_seq(0..15);
        merk.apply::<_, Vec<_>>(&batch, &[], None, grove_version)
            .unwrap()
            .expect("apply failed");
        assert_eq!(merk.height(), Some(4));

        let mut chunk_producer = ChunkProducer::new(&merk).expect("should create chunk producer");

        // we generate the chunk starting from index 2, this has no hash nodes
        // so no multi chunk will be generated
        let chunk_result = chunk_producer
            .multi_chunk_with_limit_and_index(2, None, grove_version)
            .expect("should generate chunk with limit");

        assert_eq!(chunk_result.remaining_limit, None);
        assert_eq!(chunk_result.next_index, None);

        // chunk 2 - 5 will be considered separate subtrees
        // each will have an accompanying chunk id, so 8 elements total
        assert_eq!(chunk_result.chunk.len(), 8);

        // assert the chunk id's
        assert_eq!(chunk_result.chunk[0], ChunkOp::ChunkId(vec![LEFT, LEFT]));
        assert_eq!(chunk_result.chunk[2], ChunkOp::ChunkId(vec![LEFT, RIGHT]));
        assert_eq!(chunk_result.chunk[4], ChunkOp::ChunkId(vec![RIGHT, LEFT]));
        assert_eq!(chunk_result.chunk[6], ChunkOp::ChunkId(vec![RIGHT, RIGHT]));

        // assert the chunks
        assert_eq!(
            chunk_result.chunk[1],
            ChunkOp::Chunk(
                chunk_producer
                    .chunk_with_index(2, grove_version)
                    .expect("should generate chunk")
                    .0
            )
        );
        assert_eq!(
            chunk_result.chunk[3],
            ChunkOp::Chunk(
                chunk_producer
                    .chunk_with_index(3, grove_version)
                    .expect("should generate chunk")
                    .0
            )
        );
        assert_eq!(
            chunk_result.chunk[5],
            ChunkOp::Chunk(
                chunk_producer
                    .chunk_with_index(4, grove_version)
                    .expect("should generate chunk")
                    .0
            )
        );
        assert_eq!(
            chunk_result.chunk[7],
            ChunkOp::Chunk(
                chunk_producer
                    .chunk_with_index(5, grove_version)
                    .expect("should generate chunk")
                    .0
            )
        );
    }

    #[test]
    fn test_multi_chunk_with_limit() {
        let grove_version = GroveVersion::latest();
        // tree of height 4
        // 5 chunks
        let mut merk = TempMerk::new(grove_version);
        let batch = make_batch_seq(0..15);
        merk.apply::<_, Vec<_>>(&batch, &[], None, grove_version)
            .unwrap()
            .expect("apply failed");
        assert_eq!(merk.height(), Some(4));

        let mut chunk_producer = ChunkProducer::new(&merk).expect("should create chunk producer");

        // ensure that the remaining limit, next index and values given are correct
        // if limit is smaller than first chunk, we should get an error
        let chunk_result =
            chunk_producer.multi_chunk_with_limit(vec![].as_slice(), Some(5), grove_version);
        assert!(matches!(
            chunk_result,
            Err(Error::ChunkingError(ChunkError::LimitTooSmall(..)))
        ));

        // get chunk 2
        // data size of chunk 2 is exactly 317
        // chunk op encoding for chunk 2 = 321
        // hence limit of 317 will be insufficient
        let chunk_result =
            chunk_producer.multi_chunk_with_limit_and_index(2, Some(317), grove_version);
        assert!(matches!(
            chunk_result,
            Err(Error::ChunkingError(ChunkError::LimitTooSmall(..)))
        ));

        // get chunk 2 and 3
        // chunk 2 chunk op = 331
        // chunk 3 chunk op = 321
        // padding = 5
        let chunk_result = chunk_producer
            .multi_chunk_with_limit_and_index(2, Some(321 + 321 + 5), grove_version)
            .expect("should generate chunk");
        assert_eq!(
            chunk_result.next_index,
            Some(traversal_instruction_as_vec_bytes(
                &generate_traversal_instruction(4, 4).unwrap()
            ))
        );
        assert_eq!(chunk_result.remaining_limit, Some(5));
        assert_eq!(chunk_result.chunk.len(), 4);
        assert_eq!(chunk_result.chunk[0], ChunkOp::ChunkId(vec![LEFT, LEFT]));
        assert_eq!(chunk_result.chunk[2], ChunkOp::ChunkId(vec![LEFT, RIGHT]));
    }
}
