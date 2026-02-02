// MIT LICENSE
//
// Copyright (c) 2021 Dash Core Group
//
// Permission is hereby granted, free of charge, to any
// person obtaining a copy of this software and associated
// documentation files (the "Software"), to deal in the
// Software without restriction, including without
// limitation the rights to use, copy, modify, merge,
// publish, distribute, sublicense, and/or sell copies of
// the Software, and to permit persons to whom the Software
// is furnished to do so, subject to the following
// conditions:
//
// The above copyright notice and this permission notice
// shall be included in all copies or substantial portions
// of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF
// ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED
// TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A
// PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT
// SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
// CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
// OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR
// IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
// DEALINGS IN THE SOFTWARE.

use grovedb_costs::{cost_return_on_error, CostResult, CostsExt, OperationCost};
use grovedb_element::{ElementType, ProofNodeType};
use grovedb_version::version::GroveVersion;

// TODO: add copyright comment
use crate::proofs::{Node, Op, Tree};
use crate::{
    proofs::{chunk::error::ChunkError, tree::execute},
    tree::{kv::ValueDefinedCostType, Fetch, RefWalker},
    tree_type::TreeType,
    CryptoHash, Error,
};

pub const LEFT: bool = true;
pub const RIGHT: bool = false;

impl<S> RefWalker<'_, S>
where
    S: Fetch + Sized + Clone,
{
    /// Returns a chunk of a given depth from a RefWalker
    ///
    /// # Arguments
    /// * `depth` - The depth of the chunk to create
    /// * `tree_type` - The type of tree this chunk is from (determines node
    ///   types)
    /// * `grove_version` - The grove version for compatibility
    pub fn create_chunk(
        &mut self,
        depth: usize,
        tree_type: TreeType,
        grove_version: &GroveVersion,
    ) -> CostResult<Vec<Op>, Error> {
        let mut cost = OperationCost::default();

        // build the proof vector
        let mut proof = vec![];

        cost_return_on_error!(
            &mut cost,
            self.create_chunk_internal(&mut proof, depth, tree_type, grove_version)
        );

        Ok(proof).wrap_with_cost(cost)
    }

    fn create_chunk_internal(
        &mut self,
        proof: &mut Vec<Op>,
        remaining_depth: usize,
        tree_type: TreeType,
        grove_version: &GroveVersion,
    ) -> CostResult<(), Error> {
        let mut cost = OperationCost::default();

        // at some point we will reach the depth
        // here we need to put the node hash
        if remaining_depth == 0 {
            // Use tree_type-aware hashing so ProvableCountTree/ProvableCountSumTree
            // include the count in their hash computation
            let hash_node = self.to_hash_node_for_tree_type(tree_type).unwrap();
            proof.push(Op::Push(hash_node));
            return Ok(()).wrap_with_cost(cost);
        }

        // traverse left
        let has_left_child = self.tree().link(true).is_some();
        if has_left_child {
            let mut left = cost_return_on_error!(
                &mut cost,
                self.walk(
                    true,
                    None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                    grove_version,
                )
            )
            .expect("confirmed is some");
            cost_return_on_error!(
                &mut cost,
                left.create_chunk_internal(proof, remaining_depth - 1, tree_type, grove_version)
            );
        }

        // Determine the correct node type based on element type and tree type
        let node = self.create_proof_node_for_chunk(tree_type);
        proof.push(Op::Push(node));

        if has_left_child {
            proof.push(Op::Parent);
        }

        // traverse right
        let maybe_right = cost_return_on_error!(
            &mut cost,
            self.walk(
                false,
                None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                grove_version,
            )
        );
        if let Some(mut right) = maybe_right {
            cost_return_on_error!(
                &mut cost,
                right.create_chunk_internal(proof, remaining_depth - 1, tree_type, grove_version)
            );

            proof.push(Op::Child);
        }

        Ok(()).wrap_with_cost(cost)
    }

    /// Creates the appropriate proof node based on element type and tree type.
    ///
    /// This uses the same logic as query proofs to determine the correct node
    /// type:
    /// - Items in regular trees: KV (verifier computes hash)
    /// - Items in ProvableCountTree: KVCount (includes count in hash)
    /// - Subtrees in regular trees: KVValueHash (combined hash)
    /// - Subtrees in ProvableCountTree: KVValueHashFeatureType (includes count)
    ///
    /// For raw merk values (not GroveDB Elements), defaults to
    /// KVValueHashFeatureType for backward compatibility with
    /// restore/chunking operations.
    fn create_proof_node_for_chunk(&self, tree_type: TreeType) -> Node {
        let parent_tree_type = tree_type.to_element_type();

        // Determine the proof node type based on element type
        // Default to KVValueHashFeatureType for raw merk (non-Element) values
        // to maintain backward compatibility with restore/chunking
        let proof_node_type = ElementType::from_serialized_value(self.tree().value_as_slice())
            .map(|et| et.proof_node_type(parent_tree_type))
            .unwrap_or(ProofNodeType::KvValueHashFeatureType);

        // Convert ProofNodeType to actual Node
        match proof_node_type {
            ProofNodeType::Kv => self.to_kv_node(),
            ProofNodeType::KvCount => self.to_kv_count_node(),
            ProofNodeType::KvValueHash => self.to_kv_value_hash_node(),
            ProofNodeType::KvValueHashFeatureType => self.to_kv_value_hash_feature_type_node(),
            // References: at merk level, generate same node type as non-ref counterpart
            // GroveDB will post-process if needed
            ProofNodeType::KvRefValueHash => self.to_kv_value_hash_node(),
            ProofNodeType::KvRefValueHashCount => self.to_kv_value_hash_feature_type_node(),
        }
    }

    /// Returns a chunk of a given depth after applying some traversal
    /// instruction to the RefWalker
    pub fn traverse_and_build_chunk(
        &mut self,
        instructions: &[bool],
        depth: usize,
        tree_type: TreeType,
        grove_version: &GroveVersion,
    ) -> CostResult<Vec<Op>, Error> {
        let mut cost = OperationCost::default();

        // base case
        if instructions.is_empty() {
            // we are at the desired node
            return self.create_chunk(depth, tree_type, grove_version);
        }

        // link must exist
        let has_link = self.tree().link(instructions[0]).is_some();
        if !has_link {
            return Err(Error::ChunkingError(ChunkError::BadTraversalInstruction(
                "no node found at given traversal instruction",
            )))
            .wrap_with_cost(cost);
        }

        // grab child
        let mut child = cost_return_on_error!(
            &mut cost,
            self.walk(
                instructions[0],
                None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                grove_version,
            )
        )
        .expect("confirmed link exists so cannot be none");

        // recurse on child
        child
            .traverse_and_build_chunk(&instructions[1..], depth, tree_type, grove_version)
            .add_cost(cost)
    }

    /// Returns the smallest amount of tree ops, that can convince
    /// a verifier of the tree height
    /// the generated subtree is of this form
    ///         kv_hash
    ///       /         \
    ///    kv_hash      node_hash
    ///    /      \
    ///  kv_hash   node_hash
    ///     .
    ///     .
    ///     .
    pub fn generate_height_proof(
        &mut self,
        proof: &mut Vec<Op>,
        grove_version: &GroveVersion,
    ) -> CostResult<(), Error> {
        // TODO: look into making height proofs more efficient
        //  they will always be used in the context of some
        //  existing chunk, we don't want to repeat nodes unnecessarily
        let mut cost = OperationCost::default();

        let maybe_left = cost_return_on_error!(
            &mut cost,
            self.walk(
                LEFT,
                None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                grove_version
            )
        );
        let has_left_child = maybe_left.is_some();

        // recurse to leftmost element
        if let Some(mut left) = maybe_left {
            cost_return_on_error!(&mut cost, left.generate_height_proof(proof, grove_version))
        }

        proof.push(Op::Push(self.to_kvhash_node()));

        if has_left_child {
            proof.push(Op::Parent);
        }

        if let Some(right) = self.tree().link(RIGHT) {
            proof.push(Op::Push(Node::Hash(*right.hash())));
            proof.push(Op::Child);
        }

        Ok(()).wrap_with_cost(cost)
    }
}

// TODO: add documentation
pub fn verify_height_proof(proof: Vec<Op>, expected_root_hash: CryptoHash) -> Result<usize, Error> {
    // todo: remove unwrap
    let height_proof_tree = execute(proof.into_iter().map(Ok), false, |_| Ok(())).unwrap()?;

    // todo: deal with cost
    // todo: deal with old chunk restoring error
    if height_proof_tree.hash().unwrap() != expected_root_hash {
        return Err(Error::OldChunkRestoringError(
            "invalid height proof: root hash mismatch".to_string(),
        ));
    }

    verify_height_tree(&height_proof_tree)
}

// TODO: add documentation
pub fn verify_height_tree(height_proof_tree: &Tree) -> Result<usize, Error> {
    Ok(match height_proof_tree.child(LEFT) {
        Some(child) => {
            if !matches!(child.tree.node, Node::KVHash(..)) {
                // todo deal with old chunk restoring error
                return Err(Error::OldChunkRestoringError(
                    "Expected left nodes in height proofs to be kvhash nodes".to_string(),
                ));
            }
            verify_height_tree(&child.tree)? + 1
        }
        None => 1,
    })
}

#[cfg(test)]
pub mod tests {
    use ed::Encode;
    use grovedb_version::version::GroveVersion;

    use crate::{
        proofs::{
            chunk::chunk::{verify_height_proof, LEFT, RIGHT},
            tree::execute,
            Node, Op,
        },
        test_utils::make_tree_seq_with_start_key,
        tree::{kv::ValueDefinedCostType, RefWalker, TreeNode},
        tree_type::TreeType,
        PanicSource, TreeFeatureType,
    };

    fn build_tree_10_nodes() -> TreeNode {
        let grove_version = GroveVersion::latest();
        //              3
        //           /      \
        //          1         7
        //        /    \    /   \
        //       0       2 5      8
        //                / \      \
        //               4   6      9
        make_tree_seq_with_start_key(10, [0; 8].to_vec(), grove_version)
    }

    /// Traverses a tree to a certain node and returns the node hash of that
    /// node
    pub fn traverse_get_node_hash(
        walker: &mut RefWalker<PanicSource>,
        traverse_instructions: &[bool],
        grove_version: &GroveVersion,
    ) -> Node {
        traverse_and_apply(
            walker,
            traverse_instructions,
            |walker| walker.to_hash_node().unwrap(),
            grove_version,
        )
    }

    /// Traverses a tree to a certain node and returns the kv_feature_type of
    /// that node
    pub fn traverse_get_kv_feature_type(
        walker: &mut RefWalker<PanicSource>,
        traverse_instructions: &[bool],
        grove_version: &GroveVersion,
    ) -> Node {
        traverse_and_apply(
            walker,
            traverse_instructions,
            |walker| walker.to_kv_value_hash_feature_type_node(),
            grove_version,
        )
    }
    /// Traverses a tree to a certain node and returns the kv_hash of
    /// that node
    pub fn traverse_get_kv_hash(
        walker: &mut RefWalker<PanicSource>,
        traverse_instructions: &[bool],
        grove_version: &GroveVersion,
    ) -> Node {
        traverse_and_apply(
            walker,
            traverse_instructions,
            |walker| walker.to_kvhash_node(),
            grove_version,
        )
    }

    /// Traverses a tree to a certain node and returns the result of applying
    /// some arbitrary function
    pub fn traverse_and_apply<T>(
        walker: &mut RefWalker<PanicSource>,
        traverse_instructions: &[bool],
        apply_fn: T,
        grove_version: &GroveVersion,
    ) -> Node
    where
        T: Fn(&mut RefWalker<PanicSource>) -> Node,
    {
        if traverse_instructions.is_empty() {
            return apply_fn(walker);
        }

        let mut child = walker
            .walk(
                traverse_instructions[0],
                None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                grove_version,
            )
            .unwrap()
            .unwrap()
            .unwrap();
        traverse_and_apply(
            &mut child,
            &traverse_instructions[1..],
            apply_fn,
            grove_version,
        )
    }

    #[test]
    fn build_chunk_from_root_depth_0() {
        let grove_version = GroveVersion::latest();
        let mut tree = build_tree_10_nodes();
        let mut tree_walker = RefWalker::new(&mut tree, PanicSource {});

        // should return the node hash of the root node
        let chunk = tree_walker
            .create_chunk(0, TreeType::NormalTree, grove_version)
            .unwrap()
            .expect("should build chunk");
        assert_eq!(chunk.len(), 1);
        assert_eq!(
            chunk[0],
            Op::Push(traverse_get_node_hash(&mut tree_walker, &[], grove_version))
        );

        let computed_tree = execute(chunk.into_iter().map(Ok), true, |_| Ok(()))
            .unwrap()
            .expect("should reconstruct tree");
        assert_eq!(computed_tree.hash().unwrap(), tree.hash().unwrap());
    }

    #[test]
    fn build_chunk_from_root_depth_1() {
        let grove_version = GroveVersion::latest();
        let mut tree = build_tree_10_nodes();
        let mut tree_walker = RefWalker::new(&mut tree, PanicSource {});

        // build chunk for depth 1
        // expected:
        //              3
        //           /      \
        //        Hash(1)   Hash(7)
        let chunk = tree_walker
            .create_chunk(1, TreeType::NormalTree, grove_version)
            .unwrap()
            .expect("should build chunk");
        assert_eq!(chunk.len(), 5);
        assert_eq!(
            chunk,
            vec![
                Op::Push(traverse_get_node_hash(
                    &mut tree_walker,
                    &[LEFT],
                    grove_version
                )),
                Op::Push(traverse_get_kv_feature_type(
                    &mut tree_walker,
                    &[],
                    grove_version
                )),
                Op::Parent,
                Op::Push(traverse_get_node_hash(
                    &mut tree_walker,
                    &[RIGHT],
                    grove_version
                )),
                Op::Child
            ]
        );

        let computed_tree = execute(chunk.into_iter().map(Ok), true, |_| Ok(()))
            .unwrap()
            .expect("should reconstruct tree");
        assert_eq!(computed_tree.hash().unwrap(), tree.hash().unwrap());
    }

    #[test]
    fn build_chunk_from_root_depth_3() {
        let grove_version = GroveVersion::latest();
        let mut tree = build_tree_10_nodes();
        let mut tree_walker = RefWalker::new(&mut tree, PanicSource {});

        // build chunk for depth 3
        // expected:
        //              3
        //           /      \
        //          1         7
        //        /    \    /   \
        //       0       2 5      8
        //                / \      \
        //             H(4) H(6)   H(9)
        let chunk = tree_walker
            .create_chunk(3, TreeType::NormalTree, grove_version)
            .unwrap()
            .expect("should build chunk");
        assert_eq!(chunk.len(), 19);
        assert_eq!(
            chunk,
            vec![
                Op::Push(traverse_get_kv_feature_type(
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
                Op::Push(traverse_get_kv_feature_type(
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
                    &[RIGHT, LEFT, LEFT],
                    grove_version
                )),
                Op::Push(traverse_get_kv_feature_type(
                    &mut tree_walker,
                    &[RIGHT, LEFT],
                    grove_version
                )),
                Op::Parent,
                Op::Push(traverse_get_node_hash(
                    &mut tree_walker,
                    &[RIGHT, LEFT, RIGHT],
                    grove_version
                )),
                Op::Child,
                Op::Push(traverse_get_kv_feature_type(
                    &mut tree_walker,
                    &[RIGHT],
                    grove_version
                )),
                Op::Parent,
                Op::Push(traverse_get_kv_feature_type(
                    &mut tree_walker,
                    &[RIGHT, RIGHT],
                    grove_version
                )),
                Op::Push(traverse_get_node_hash(
                    &mut tree_walker,
                    &[RIGHT, RIGHT, RIGHT],
                    grove_version
                )),
                Op::Child,
                Op::Child,
                Op::Child
            ]
        );

        let computed_tree = execute(chunk.into_iter().map(Ok), true, |_| Ok(()))
            .unwrap()
            .expect("should reconstruct tree");
        assert_eq!(computed_tree.hash().unwrap(), tree.hash().unwrap());
    }

    #[test]
    fn build_chunk_from_root_depth_max_depth() {
        let grove_version = GroveVersion::latest();
        let mut tree = build_tree_10_nodes();
        let mut tree_walker = RefWalker::new(&mut tree, PanicSource {});

        // build chunk for entire tree (depth 4)
        //              3
        //           /      \
        //          1         7
        //        /    \    /   \
        //       0       2 5      8
        //                / \      \
        //               4   6      9
        let chunk = tree_walker
            .create_chunk(4, TreeType::NormalTree, grove_version)
            .unwrap()
            .expect("should build chunk");
        assert_eq!(chunk.len(), 19);
        assert_eq!(
            chunk,
            vec![
                Op::Push(traverse_get_kv_feature_type(
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
                Op::Push(traverse_get_kv_feature_type(
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
                Op::Child,
                Op::Push(traverse_get_kv_feature_type(
                    &mut tree_walker,
                    &[RIGHT],
                    grove_version
                )),
                Op::Parent,
                Op::Push(traverse_get_kv_feature_type(
                    &mut tree_walker,
                    &[RIGHT, RIGHT],
                    grove_version
                )),
                Op::Push(traverse_get_kv_feature_type(
                    &mut tree_walker,
                    &[RIGHT, RIGHT, RIGHT],
                    grove_version
                )),
                Op::Child,
                Op::Child,
                Op::Child
            ]
        );

        let computed_tree = execute(chunk.into_iter().map(Ok), true, |_| Ok(()))
            .unwrap()
            .expect("should reconstruct tree");
        assert_eq!(computed_tree.hash().unwrap(), tree.hash().unwrap());
    }

    #[test]
    fn chunk_greater_than_max_should_equal_max_depth() {
        let grove_version = GroveVersion::latest();
        let mut tree = build_tree_10_nodes();
        let mut tree_walker = RefWalker::new(&mut tree, PanicSource {});

        // build chunk with depth greater than tree
        // we should get the same result as building with the exact depth
        let large_depth_chunk = tree_walker
            .create_chunk(100, TreeType::NormalTree, grove_version)
            .unwrap()
            .expect("should build chunk");
        let exact_depth_chunk = tree_walker
            .create_chunk(4, TreeType::NormalTree, grove_version)
            .unwrap()
            .expect("should build chunk");
        assert_eq!(large_depth_chunk, exact_depth_chunk);

        let tree_a = execute(large_depth_chunk.into_iter().map(Ok), true, |_| Ok(()))
            .unwrap()
            .expect("should reconstruct tree");
        let tree_b = execute(exact_depth_chunk.into_iter().map(Ok), true, |_| Ok(()))
            .unwrap()
            .expect("should reconstruct tree");
        assert_eq!(tree_a.hash().unwrap(), tree_b.hash().unwrap());
    }

    #[test]
    fn build_chunk_after_traversal_depth_2() {
        let grove_version = GroveVersion::latest();
        let mut tree = build_tree_10_nodes();
        let mut tree_walker = RefWalker::new(&mut tree, PanicSource {});

        // traverse to the right first then build chunk
        // expected
        //       7
        //     /   \
        //    5     8
        //   / \     \
        // H(4) H(6)  H(9)

        // right traversal
        let chunk = tree_walker
            .traverse_and_build_chunk(&[RIGHT], 2, TreeType::NormalTree, grove_version)
            .unwrap()
            .expect("should build chunk");
        assert_eq!(
            chunk,
            vec![
                Op::Push(traverse_get_node_hash(
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
                Op::Push(traverse_get_node_hash(
                    &mut tree_walker,
                    &[RIGHT, LEFT, RIGHT],
                    grove_version
                )),
                Op::Child,
                Op::Push(traverse_get_kv_feature_type(
                    &mut tree_walker,
                    &[RIGHT],
                    grove_version
                )),
                Op::Parent,
                Op::Push(traverse_get_kv_feature_type(
                    &mut tree_walker,
                    &[RIGHT, RIGHT],
                    grove_version
                )),
                Op::Push(traverse_get_node_hash(
                    &mut tree_walker,
                    &[RIGHT, RIGHT, RIGHT],
                    grove_version
                )),
                Op::Child,
                Op::Child,
            ]
        );

        // the hash of the tree computed from the chunk
        // should be the same as the node_hash of the element
        // on the right
        let computed_tree = execute(chunk.into_iter().map(Ok), true, |_| Ok(()))
            .unwrap()
            .expect("should reconstruct tree");
        assert_eq!(
            Node::Hash(computed_tree.hash().unwrap()),
            traverse_get_node_hash(&mut tree_walker, &[RIGHT], grove_version)
        );
    }

    #[test]
    fn build_chunk_after_traversal_depth_1() {
        let grove_version = GroveVersion::latest();
        let mut tree = build_tree_10_nodes();
        let mut tree_walker = RefWalker::new(&mut tree, PanicSource {});

        // traverse with [right, left] and then build chunk of depth 1
        // expected
        //     5
        //   /   \
        // H(4)  H(6)

        // instruction traversal
        let chunk = tree_walker
            .traverse_and_build_chunk(&[RIGHT, LEFT], 1, TreeType::NormalTree, grove_version)
            .unwrap()
            .expect("should build chunk");
        assert_eq!(
            chunk,
            vec![
                Op::Push(traverse_get_node_hash(
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
                Op::Push(traverse_get_node_hash(
                    &mut tree_walker,
                    &[RIGHT, LEFT, RIGHT],
                    grove_version
                )),
                Op::Child,
            ]
        );

        let computed_tree = execute(chunk.into_iter().map(Ok), true, |_| Ok(()))
            .unwrap()
            .expect("should reconstruct tree");
        assert_eq!(
            Node::Hash(computed_tree.hash().unwrap()),
            traverse_get_node_hash(&mut tree_walker, &[RIGHT, LEFT], grove_version)
        );
    }

    #[test]
    fn test_chunk_encoding() {
        let chunk = vec![
            Op::Push(Node::Hash([0; 32])),
            Op::Push(Node::KVValueHashFeatureType(
                vec![1],
                vec![2],
                [0; 32],
                TreeFeatureType::BasicMerkNode,
            )),
        ];
        let encoded_chunk = chunk.encode().expect("should encode");
        assert_eq!(encoded_chunk.len(), 33 + 39);
        assert_eq!(
            encoded_chunk.len(),
            chunk.encoding_length().expect("should get encoding length")
        );
    }

    #[test]
    fn test_height_proof_generation() {
        let grove_version = GroveVersion::latest();
        let mut tree = build_tree_10_nodes();
        let mut tree_walker = RefWalker::new(&mut tree, PanicSource {});

        let mut height_proof = vec![];
        tree_walker
            .generate_height_proof(&mut height_proof, grove_version)
            .unwrap()
            .expect("should generate height proof");

        assert_eq!(height_proof.len(), 9);
        assert_eq!(
            height_proof,
            vec![
                Op::Push(traverse_get_kv_hash(
                    &mut tree_walker,
                    &[LEFT, LEFT],
                    grove_version
                )),
                Op::Push(traverse_get_kv_hash(
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
                Op::Push(traverse_get_kv_hash(&mut tree_walker, &[], grove_version)),
                Op::Parent,
                Op::Push(traverse_get_node_hash(
                    &mut tree_walker,
                    &[RIGHT],
                    grove_version
                )),
                Op::Child,
            ]
        );
    }

    #[test]
    fn test_height_proof_verification() {
        let grove_version = GroveVersion::latest();
        let mut tree = build_tree_10_nodes();
        let mut tree_walker = RefWalker::new(&mut tree, PanicSource {});

        let mut height_proof = vec![];
        tree_walker
            .generate_height_proof(&mut height_proof, grove_version)
            .unwrap()
            .expect("should generate height proof");

        let verified_height = verify_height_proof(height_proof, tree.hash().unwrap())
            .expect("should verify height proof");

        // doesn't represent the max height of the tree
        assert_eq!(verified_height, 3);
    }
}
