//! Collection of state independent algorithms needed for facilitate chunk
//! production and restoration

use std::io::Write;

// TODO: figure out better nomenclature
use crate::{proofs::chunk::binary_range::BinaryRange, Error};
use crate::{
    proofs::chunk::{
        chunk::{LEFT, RIGHT},
        error::{ChunkError, ChunkError::BadTraversalInstruction},
    },
    Error::InternalError,
};

/// Represents the height as a linear combination of 3 amd 2
/// of the form 3x + 2y
/// this breaks the tree into layers of height 3 or 2
/// the minimum chunk height is 2, so if tree height is less than 2
/// we just return a single layer of height 2
fn chunk_height_per_layer(height: usize) -> Vec<usize> {
    let mut two_count = 0;
    let mut three_count = height / 3;

    if height == 0 {
        return vec![];
    }

    // minimum chunk height is 2, if tree height is less than 2
    // return a single layer with chunk height 2
    if height < 2 {
        two_count = 1;
    } else {
        match height % 3 {
            0 => { /* do nothing */ }
            1 => {
                // reduce the three_count by 1
                // so the remainder becomes 3 + 1
                // which is equivalent to 2 + 2
                three_count -= 1;
                two_count += 2;
            }
            2 => {
                // remainder is a factor of 2
                // just increase the two_count
                two_count += 1;
            }
            // this is unreachable because height is a positive number
            // remainder set after diving by 3 is fixed to [0,1,2]
            _ => unreachable!(""),
        }
    }

    let mut layer_heights = vec![3; three_count];
    layer_heights.extend(vec![2; two_count]);

    layer_heights
}

/// Return the layer a chunk subtree belongs to
pub fn chunk_layer(height: usize, chunk_id: usize) -> Result<usize, Error> {
    // remaining depth tells us how deep in the tree the specified chunk is
    let mut remaining_depth = generate_traversal_instruction(height, chunk_id)?.len() + 1;
    let layer_heights = chunk_height_per_layer(height);

    let mut layer = 1;

    while remaining_depth > 1 {
        // remaining depth will always larger than the next layer height
        // if it is not already 1
        // this is because a every chunk always starts at a layer boundary
        // and remaining depth points to a chunk
        debug_assert!(remaining_depth > layer_heights[layer - 1]);

        remaining_depth -= layer_heights[layer - 1];
        layer += 1;
    }

    Ok(layer - 1)
}

/// Return the depth of a chunk given the height
/// and chunk id
pub fn chunk_height(height: usize, chunk_id: usize) -> Result<usize, Error> {
    let chunk_layer = chunk_layer(height, chunk_id)?;
    let layer_heights = chunk_height_per_layer(height);

    Ok(layer_heights[chunk_layer])
}

/// Given a tree of height h, return the number of chunks needed
/// to completely represent the tree
pub fn number_of_chunks(height: usize) -> usize {
    let layer_heights = chunk_height_per_layer(height);
    number_of_chunks_internal(layer_heights)
}

/// Locates the subtree represented by a chunk id and returns
/// the number of chunks under that subtree
pub fn number_of_chunks_under_chunk_id(height: usize, chunk_id: usize) -> Result<usize, Error> {
    let chunk_layer = chunk_layer(height, chunk_id)?;
    let layer_heights = chunk_height_per_layer(height);

    // we only care about the layer heights after the chunk layer
    // as we are getting the number of chunks under a subtree and not
    // the entire tree of height h
    Ok(number_of_chunks_internal(
        layer_heights[chunk_layer..].to_vec(),
    ))
}

/// Given the heights of a tree per layer, return the total number of chunks in
/// that tree
fn number_of_chunks_internal(layer_heights: Vec<usize>) -> usize {
    // a layer consists of 1 or more subtrees of a given height
    // here we figure out number of exit nodes from a single subtree for each layer
    let mut single_subtree_exits_per_layer = layer_heights
        .into_iter()
        .map(exit_node_count)
        .collect::<Vec<usize>>();

    // we don't care about exit nodes from the last layer
    // as that points to non-existent subtrees
    single_subtree_exits_per_layer.pop();

    // now we get the total exit nodes per layer
    // by multiplying the exits per subtree with the number of subtrees on that
    // layer
    let mut chunk_counts_per_layer = vec![1];
    for i in 0..single_subtree_exits_per_layer.len() {
        let previous_layer_chunk_count = chunk_counts_per_layer[i];
        let current_layer_chunk_count =
            previous_layer_chunk_count * single_subtree_exits_per_layer[i];
        chunk_counts_per_layer.push(current_layer_chunk_count);
    }

    chunk_counts_per_layer.into_iter().sum()
}

/// Calculates the maximum number of exit nodes for a tree of height h.
fn exit_node_count(height: usize) -> usize {
    2_usize.pow(height as u32)
}

/// Generate instruction for traversing to a given chunk index in a binary tree
pub fn generate_traversal_instruction(
    height: usize,
    chunk_index: usize,
) -> Result<Vec<bool>, Error> {
    let mut instructions = vec![];

    let total_chunk_count = number_of_chunks(height);

    // out of bounds
    if chunk_index < 1 || chunk_index > total_chunk_count {
        return Err(Error::ChunkingError(ChunkError::OutOfBounds(
            "chunk id out of bounds",
        )));
    }

    let mut chunk_range = BinaryRange::new(1, total_chunk_count).map_err(|_| {
        Error::ChunkingError(ChunkError::InternalError(
            "failed to initialize chunk range",
        ))
    })?;

    // total chunk count will always be odd because
    // from the initial chunk (1) we have an even number of
    // exit nodes, and they have even numbers of exit nodes ...
    // so total_chunk_count = 1 + some_even_number = odd
    debug_assert!(chunk_range.odd());

    // bisect and reduce the chunk range until we get to the desired chunk
    // we keep track of every left right decision we make
    while chunk_range.len() > 1 {
        if chunk_range.odd() {
            // checks if we last decision we made got us to the desired chunk id
            let advance_result = chunk_range.advance_range_start().unwrap();
            chunk_range = advance_result.0;
            if advance_result.1 == chunk_index {
                return Ok(instructions);
            }
        } else {
            // for even chunk range, we are at the decision point
            // we can either go left or right
            // we first check which half the desired chunk is
            // then follow that path
            let chunk_id_half = chunk_range
                .which_half(chunk_index)
                .expect("chunk id must exist in range");
            instructions.push(chunk_id_half);
            chunk_range = chunk_range
                .get_half(chunk_id_half)
                .expect("confirmed range is not odd");
        }
    }

    // chunk range len is exactly 1
    // this must be the desired chunk id
    // return instructions that got us here
    Ok(instructions)
}

/// Determine the chunk index given the traversal instruction and the max height
/// of the tree
pub fn chunk_index_from_traversal_instruction(
    traversal_instruction: &[bool],
    height: usize,
) -> Result<usize, Error> {
    // empty traversal instruction points to the first chunk
    if traversal_instruction.is_empty() {
        return Ok(1);
    }

    let mut chunk_count = number_of_chunks(height);
    let mut current_chunk_index = 1;

    let mut layer_heights = chunk_height_per_layer(height);
    let last_layer_height = layer_heights.pop().expect("confirmed not empty");

    // traversal instructions should only point to the root node of chunks (chunk
    // boundaries) the layer heights represent the height of each chunk layer
    // the last chunk layer is at height = total_height - last_chunk_height + 1
    // traversal instructions require 1 less than height to address it
    // e.g. height 1 is represented by [] - len of 0
    //      height 2 is represented by [left] or [right] len of 1
    // therefore last chunk root node is address with total_height -
    // last_chunk_height
    if traversal_instruction.len() > height - last_layer_height {
        return Err(Error::ChunkingError(BadTraversalInstruction(
            "traversal instruction should not address nodes past the root of the last layer chunks",
        )));
    }

    // verify that the traversal instruction points to a chunk boundary
    let mut traversal_length = traversal_instruction.len();
    let mut relevant_layer_heights = vec![];
    for layer_height in layer_heights {
        // the traversal_length should be a perfect sum of a subset of the layer_height
        // if the traversal_length is not 0, it should be larger than or equal to the
        // next layer height.
        if traversal_length < layer_height {
            return Err(Error::ChunkingError(BadTraversalInstruction(
                "traversal instruction should point to a chunk boundary",
            )));
        }

        traversal_length -= layer_height;
        relevant_layer_heights.push(layer_height);

        if traversal_length == 0 {
            break;
        }
    }

    // take layer_height instructions and determine the updated chunk id
    let mut start_index = 0;
    for layer_height in relevant_layer_heights {
        let end_index = start_index + layer_height;
        let subset_instructions = &traversal_instruction[start_index..end_index];

        // offset multiplier determines what subchunk we are on based on the given
        // instruction offset multiplier just converts the binary instruction to
        // decimal, taking left as 0 and right as 0 i.e [left, left, left] = 0
        // means we are at subchunk 0
        let mut offset_multiplier = 0;
        for (i, instruction) in subset_instructions.iter().enumerate() {
            offset_multiplier += 2_usize.pow((subset_instructions.len() - i - 1) as u32)
                * (1 - *instruction as usize);
        }

        if chunk_count % 2 != 0 {
            // remove the current chunk from the chunk count
            chunk_count -= 1;
        }

        chunk_count /= exit_node_count(layer_height);

        current_chunk_index = current_chunk_index + offset_multiplier * chunk_count + 1;

        start_index = end_index;
    }

    Ok(current_chunk_index)
}

/// Determine the chunk index given the traversal instruction and the max height
/// of the tree. This can recover from traversal instructions not pointing to a
/// chunk boundary, in such a case, it backtracks until it hits a chunk
/// boundary.
pub fn chunk_index_from_traversal_instruction_with_recovery(
    traversal_instruction: &[bool],
    height: usize,
) -> Result<usize, Error> {
    let chunk_index_result = chunk_index_from_traversal_instruction(traversal_instruction, height);
    if chunk_index_result.is_err() {
        return chunk_index_from_traversal_instruction_with_recovery(
            &traversal_instruction[0..traversal_instruction.len() - 1],
            height,
        );
    }
    chunk_index_result
}

/// Generate instruction for traversing to a given chunk index in a binary tree,
/// returns vec bytes representation
pub fn generate_traversal_instruction_as_vec_bytes(
    height: usize,
    chunk_index: usize,
) -> Result<Vec<u8>, Error> {
    let instruction = generate_traversal_instruction(height, chunk_index)?;
    Ok(traversal_instruction_as_vec_bytes(&instruction))
}

/// Convert traversal instruction to bytes vec
/// 1 represents left (true)
/// 0 represents right (false)
pub fn traversal_instruction_as_vec_bytes(instruction: &[bool]) -> Vec<u8> {
    instruction
        .iter()
        .map(|v| if *v { 1u8 } else { 0u8 })
        .collect()
}

/// Converts a vec bytes that represents a traversal instruction
/// to a vec of bool, true = left and false = right
pub fn vec_bytes_as_traversal_instruction(
    instruction_vec_bytes: &[u8],
) -> Result<Vec<bool>, Error> {
    instruction_vec_bytes
        .iter()
        .map(|byte| match byte {
            1u8 => Ok(LEFT),
            0u8 => Ok(RIGHT),
            _ => Err(Error::ChunkingError(ChunkError::BadTraversalInstruction(
                "failed to parse instruction vec bytes",
            ))),
        })
        .collect()
}

pub fn write_to_vec<W: Write>(dest: &mut W, value: &[u8]) -> Result<(), Error> {
    dest.write_all(value)
        .map_err(|_e| InternalError("failed to write to vector"))
}

#[cfg(test)]
mod test {

    use super::*;
    use crate::proofs::chunk::chunk::{LEFT, RIGHT};

    #[test]
    fn test_chunk_height_per_layer() {
        let layer_heights = chunk_height_per_layer(10);
        assert_eq!(layer_heights.iter().sum::<usize>(), 10);
        assert_eq!(layer_heights, [3, 3, 2, 2]);

        let layer_heights = chunk_height_per_layer(45);
        assert_eq!(layer_heights.iter().sum::<usize>(), 45);
        assert_eq!(layer_heights, [3; 15]);

        let layer_heights = chunk_height_per_layer(2);
        assert_eq!(layer_heights.iter().sum::<usize>(), 2);
        assert_eq!(layer_heights, [2]);

        // height less than 2
        let layer_heights = chunk_height_per_layer(1);
        assert_eq!(layer_heights.iter().sum::<usize>(), 2);
        assert_eq!(layer_heights, [2]);

        let layer_heights = chunk_height_per_layer(0);
        assert_eq!(layer_heights.iter().sum::<usize>(), 0);
        assert_eq!(layer_heights, Vec::<usize>::new());
    }

    #[test]
    fn test_exit_node_count() {
        // tree with just one node has 2 exit nodes
        assert_eq!(exit_node_count(1), 2);

        // tree with height 2 has 4 exit nodes
        assert_eq!(exit_node_count(2), 4);

        // tree with height 6 has 64 exit nodes
        assert_eq!(exit_node_count(6), 64);
    }

    #[test]
    fn test_number_of_chunks() {
        // given a chunk of height less than 3 chunk count should be 1
        assert_eq!(number_of_chunks(1), 1);
        assert_eq!(number_of_chunks(2), 1);

        // tree with height 4 should have 5 chunks
        // we split the tree into 2 layers of chunk height 2 each
        // first layer contains just one chunk (1), but has 4 exit nodes
        // hence total chunk count = 1 + 4 = 5
        assert_eq!(number_of_chunks(4), 5);

        // tree with height 6 should have 9 chunks
        // will be split into two layers of chunk height 3 = [3,3]
        // first chunk takes 1, has 2^3 = 8 exit nodes
        // total chunks = 1 + 8 = 9
        assert_eq!(number_of_chunks(6), 9);

        // tree with height 10 should have 341 chunks
        // will be split into 5 layers = [3, 3, 2, 2]
        // first layer has just 1 chunk, exit nodes = 2^3 = 8
        // second layer has 4 chunks, exit nodes = 2^3 * 8 = 64
        // third layer has 16 chunks, exit nodes = 2^2 * 64 = 256
        // fourth layer has 256 chunks
        // total chunks = 1 + 8 + 64 + 256 = 329 chunks
        assert_eq!(number_of_chunks(10), 329);
    }

    #[test]
    fn test_number_of_chunks_under_chunk_id() {
        // tree with height less than 3 should have just 1 chunk
        assert_eq!(number_of_chunks_under_chunk_id(1, 1).unwrap(), 1);
        assert_eq!(number_of_chunks_under_chunk_id(2, 1).unwrap(), 1);

        // asking for chunk out of bounds should return error
        assert!(number_of_chunks_under_chunk_id(1, 3).is_err());

        // tree with height 4 should have 5 chunks at chunk id 1
        // but 1 chunk at id 2 - 5
        assert_eq!(number_of_chunks_under_chunk_id(4, 1).unwrap(), 5);
        assert_eq!(number_of_chunks_under_chunk_id(4, 2).unwrap(), 1);
        assert_eq!(number_of_chunks_under_chunk_id(4, 3).unwrap(), 1);
        assert_eq!(number_of_chunks_under_chunk_id(4, 4).unwrap(), 1);
        assert_eq!(number_of_chunks_under_chunk_id(4, 5).unwrap(), 1);

        // tree with height 10 should have 329 chunks
        // layer_heights = [3, 3, 2, 2]
        // chunk_id 1 = 329
        // chunk_id 2 = 41 i.e (329 - 1) / 2^3
        // chunk_id 3 = 5 i.e (41 - 1) / 2^3
        // chunk_id 4 = 1 i.e (5 - 1) / 2^2
        // chunk_id 5 = 1 on the same layer as 4
        // chunk_id 43 = 41 as chunk 43 should wrap back to the same layer as chunk_id 2
        // chunk_id 44 = mirrors chunk_id 3
        // chunk_id 45 = mirrors chunk_id 4
        // chunk_id 46 = mirrors chunk_id 5
        assert_eq!(number_of_chunks_under_chunk_id(10, 1).unwrap(), 329);
        assert_eq!(number_of_chunks_under_chunk_id(10, 2).unwrap(), 41);
        assert_eq!(number_of_chunks_under_chunk_id(10, 3).unwrap(), 5);
        assert_eq!(number_of_chunks_under_chunk_id(10, 4).unwrap(), 1);
        assert_eq!(number_of_chunks_under_chunk_id(10, 5).unwrap(), 1);
        assert_eq!(number_of_chunks_under_chunk_id(10, 43).unwrap(), 41);
        assert_eq!(number_of_chunks_under_chunk_id(10, 44).unwrap(), 5);
        assert_eq!(number_of_chunks_under_chunk_id(10, 45).unwrap(), 1);
        assert_eq!(number_of_chunks_under_chunk_id(10, 46).unwrap(), 1);
    }

    #[test]
    fn test_traversal_instruction_generation() {
        //              3
        //           /      \
        //          1         7
        //        /    \    /   \
        //       0       2 5      8
        //                / \      \
        //               4   6      9
        // height: 4
        // layer_height: 3, 3
        //              3
        //           /      \
        //          1         7
        //        /    \    /   \
        //       0       2 5      8
        // ............................
        //                / \      \
        //               4   6      9
        // 5 chunks
        // chunk 1 entry - 3
        // chunk 2 entry - 0
        // chunk 3 entry - 2
        // chunk 4 entry - 5
        // chunk 5 entry - 8

        // chunk 1 entry - 3 is at the top of the tree so empty instruction set
        let instruction =
            generate_traversal_instruction(4, 1).expect("should generate traversal instruction");
        let empty_instruction: &[bool] = &[];
        assert_eq!(instruction, empty_instruction);

        // chunk 2 entry - 0
        // go left twice from root i.e 3 left -> 1 left -> 0
        let instruction =
            generate_traversal_instruction(4, 2).expect("should generate traversal instruction");
        assert_eq!(instruction, &[LEFT, LEFT]);

        // chunk 3 entry - 2
        // go left then right from root i.e 3 left -> 1 right -> 2
        let instruction =
            generate_traversal_instruction(4, 3).expect("should generate traversal instruction");
        assert_eq!(instruction, &[LEFT, RIGHT]);

        // chunk 4 entry - 5
        // go right then left i.e 3 right -> 7 left -> 5
        let instruction =
            generate_traversal_instruction(4, 4).expect("should generate traversal instruction");
        assert_eq!(instruction, &[RIGHT, LEFT]);

        // chunk 5 entry - 8
        // go right twice i.e 3 right -> 7 right -> 8
        let instruction =
            generate_traversal_instruction(4, 5).expect("should generate traversal instruction");
        assert_eq!(instruction, &[RIGHT, RIGHT]);

        // out of bound tests
        assert!(generate_traversal_instruction(4, 6).is_err());
        assert!(generate_traversal_instruction(4, 0).is_err());
    }

    #[test]
    fn test_chunk_height() {
        // tree of height 6
        // all chunks have the same height
        // since layer height = [3,3]
        // we have 9 chunks in a tree of this height
        for i in 1..=9 {
            assert_eq!(chunk_height(6, i).unwrap(), 3);
        }

        // tree of height 5
        // layer_height = [3, 2]
        // we have 9 chunks, just the first chunk is of height 3
        // the rest are of height 2
        assert_eq!(chunk_height(5, 1).unwrap(), 3);
        for i in 2..=9 {
            assert_eq!(chunk_height(5, i).unwrap(), 2);
        }

        // tree of height 10
        // layer_height = [3, 3, 2, 2]
        // just going to check chunk 1 - 5
        assert_eq!(chunk_height(10, 1).unwrap(), 3);
        assert_eq!(chunk_height(10, 2).unwrap(), 3);
        assert_eq!(chunk_height(10, 3).unwrap(), 2);
        assert_eq!(chunk_height(10, 4).unwrap(), 2);
        assert_eq!(chunk_height(10, 5).unwrap(), 2);
    }

    #[test]
    fn test_traversal_instruction_as_string() {
        assert_eq!(traversal_instruction_as_vec_bytes(&[]), Vec::<u8>::new());
        assert_eq!(traversal_instruction_as_vec_bytes(&[LEFT]), vec![1u8]);
        assert_eq!(traversal_instruction_as_vec_bytes(&[RIGHT]), vec![0u8]);
        assert_eq!(
            traversal_instruction_as_vec_bytes(&[RIGHT, LEFT, LEFT, RIGHT]),
            vec![0u8, 1u8, 1u8, 0u8]
        );
    }

    #[test]
    fn test_instruction_string_to_traversal_instruction() {
        assert_eq!(
            vec_bytes_as_traversal_instruction(&[1u8]).unwrap(),
            vec![LEFT]
        );
        assert_eq!(
            vec_bytes_as_traversal_instruction(&[0u8]).unwrap(),
            vec![RIGHT]
        );
        assert_eq!(
            vec_bytes_as_traversal_instruction(&[0u8, 0u8, 1u8]).unwrap(),
            vec![RIGHT, RIGHT, LEFT]
        );
        assert!(vec_bytes_as_traversal_instruction(&[0u8, 0u8, 2u8]).is_err());
        assert_eq!(
            vec_bytes_as_traversal_instruction(&[]).unwrap(),
            Vec::<bool>::new()
        );
    }

    #[test]
    fn test_chunk_id_from_traversal_instruction() {
        // tree of height 4
        let traversal_instruction = generate_traversal_instruction(4, 1).unwrap();
        assert_eq!(
            chunk_index_from_traversal_instruction(traversal_instruction.as_slice(), 4).unwrap(),
            1
        );
        let traversal_instruction = generate_traversal_instruction(4, 2).unwrap();
        assert_eq!(
            chunk_index_from_traversal_instruction(traversal_instruction.as_slice(), 4).unwrap(),
            2
        );
        let traversal_instruction = generate_traversal_instruction(4, 3).unwrap();
        assert_eq!(
            chunk_index_from_traversal_instruction(traversal_instruction.as_slice(), 4).unwrap(),
            3
        );
        let traversal_instruction = generate_traversal_instruction(4, 4).unwrap();
        assert_eq!(
            chunk_index_from_traversal_instruction(traversal_instruction.as_slice(), 4).unwrap(),
            4
        );

        // tree of height 6
        let traversal_instruction = generate_traversal_instruction(6, 1).unwrap();
        assert_eq!(
            chunk_index_from_traversal_instruction(traversal_instruction.as_slice(), 6).unwrap(),
            1
        );
        let traversal_instruction = generate_traversal_instruction(6, 2).unwrap();
        assert_eq!(
            chunk_index_from_traversal_instruction(traversal_instruction.as_slice(), 6).unwrap(),
            2
        );
        let traversal_instruction = generate_traversal_instruction(6, 3).unwrap();
        assert_eq!(
            chunk_index_from_traversal_instruction(traversal_instruction.as_slice(), 6).unwrap(),
            3
        );
        let traversal_instruction = generate_traversal_instruction(6, 4).unwrap();
        assert_eq!(
            chunk_index_from_traversal_instruction(traversal_instruction.as_slice(), 6).unwrap(),
            4
        );
        let traversal_instruction = generate_traversal_instruction(6, 5).unwrap();
        assert_eq!(
            chunk_index_from_traversal_instruction(traversal_instruction.as_slice(), 6).unwrap(),
            5
        );
        let traversal_instruction = generate_traversal_instruction(6, 6).unwrap();
        assert_eq!(
            chunk_index_from_traversal_instruction(traversal_instruction.as_slice(), 6).unwrap(),
            6
        );
        let traversal_instruction = generate_traversal_instruction(6, 7).unwrap();
        assert_eq!(
            chunk_index_from_traversal_instruction(traversal_instruction.as_slice(), 6).unwrap(),
            7
        );
        let traversal_instruction = generate_traversal_instruction(6, 8).unwrap();
        assert_eq!(
            chunk_index_from_traversal_instruction(traversal_instruction.as_slice(), 6).unwrap(),
            8
        );
        let traversal_instruction = generate_traversal_instruction(6, 9).unwrap();
        assert_eq!(
            chunk_index_from_traversal_instruction(traversal_instruction.as_slice(), 6).unwrap(),
            9
        );
    }

    #[test]
    fn test_chunk_id_from_traversal_instruction_with_recovery() {
        // tree of height 5
        // layer heights = [3, 2]
        // first chunk boundary is at instruction len 0 e.g. []
        // second chunk boundary is at instruction len 3 e.g. [left, left, left]
        // anything outside of this should return an error with regular chunk_id
        // function with recovery we expect this to backtrack to the last chunk
        // boundary e.g. [left] should backtrack to []
        //      [left, left, right, left] should backtrack to [left, left, right]
        assert!(chunk_index_from_traversal_instruction(&[LEFT], 5).is_err());
        assert_eq!(
            chunk_index_from_traversal_instruction_with_recovery(&[LEFT], 5).unwrap(),
            1
        );
        assert_eq!(
            chunk_index_from_traversal_instruction_with_recovery(&[LEFT, LEFT], 5).unwrap(),
            1
        );
        assert_eq!(
            chunk_index_from_traversal_instruction_with_recovery(&[LEFT, LEFT, RIGHT], 5).unwrap(),
            3
        );
        assert_eq!(
            chunk_index_from_traversal_instruction_with_recovery(&[LEFT, LEFT, RIGHT, LEFT], 5)
                .unwrap(),
            3
        );
        assert_eq!(
            chunk_index_from_traversal_instruction_with_recovery(&[LEFT; 50], 5).unwrap(),
            2
        );
    }
}
