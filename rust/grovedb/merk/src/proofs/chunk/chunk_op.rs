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

use std::io::{Read, Write};

use ed::{Decode, Encode};
use integer_encoding::{VarInt, VarIntReader};

use crate::proofs::Op;

/// Represents the chunk generated from a given starting chunk id
#[derive(PartialEq, Debug)]
pub enum ChunkOp {
    ChunkId(Vec<bool>),
    Chunk(Vec<Op>),
}

impl Encode for ChunkOp {
    fn encode_into<W: Write>(&self, dest: &mut W) -> ed::Result<()> {
        match self {
            Self::ChunkId(instruction) => {
                // write the marker then the len
                let _ = dest.write_all(&[0_u8]);
                dest.write_all(instruction.len().encode_var_vec().as_slice())?;
                let instruction_as_binary: Vec<u8> = instruction
                    .iter()
                    .map(|v| if *v { 1_u8 } else { 0_u8 })
                    .collect();
                dest.write_all(&instruction_as_binary)?;
            }
            Self::Chunk(chunk) => {
                let _ = dest.write_all(&[1_u8]);
                // chunk len represents the number of ops not the total encoding len of ops
                dest.write_all(chunk.len().encode_var_vec().as_slice())?;
                for op in chunk {
                    dest.write_all(&op.encode()?)?;
                }
            }
        }

        Ok(())
    }

    fn encoding_length(&self) -> ed::Result<usize> {
        Ok(match self {
            Self::ChunkId(instruction) => {
                1 + instruction.len().encode_var_vec().len() + instruction.len()
            }
            Self::Chunk(chunk) => {
                1 + chunk.len().encode_var_vec().len() + chunk.encoding_length()?
            }
        })
    }
}

impl Decode for ChunkOp {
    fn decode<R: Read>(input: R) -> ed::Result<Self> {
        let mut chunk_op = ChunkOp::ChunkId(vec![]);
        Self::decode_into(&mut chunk_op, input)?;
        Ok(chunk_op)
    }

    fn decode_into<R: Read>(&mut self, mut input: R) -> ed::Result<()> {
        let mut marker = [0_u8; 1];
        input.read_exact(&mut marker)?;

        match marker[0] {
            0 => {
                let length = input.read_varint()?;
                let mut instruction_as_binary = vec![0_u8; length];
                input.read_exact(&mut instruction_as_binary)?;

                let instruction: Vec<bool> = instruction_as_binary
                    .into_iter()
                    .map(|v| v == 1_u8)
                    .collect();

                *self = ChunkOp::ChunkId(instruction);
            }
            1 => {
                let ops_length = input.read_varint()?;
                let mut chunk = Vec::with_capacity(ops_length);

                for _ in 0..ops_length {
                    let op = Decode::decode(&mut input)?;
                    chunk.push(op);
                }

                *self = ChunkOp::Chunk(chunk);
            }
            _ => return Err(ed::Error::UnexpectedByte(marker[0])),
        }

        Ok(())
    }
}

#[cfg(test)]
mod test {
    use ed::{Decode, Encode};

    use crate::proofs::{
        chunk::{
            chunk::{LEFT, RIGHT},
            chunk_op::ChunkOp,
        },
        Node, Op,
    };

    #[test]
    fn test_chunk_op_encoding() {
        let chunk_op = ChunkOp::ChunkId(vec![LEFT, RIGHT]);
        let encoded_chunk_op = chunk_op.encode().unwrap();
        assert_eq!(encoded_chunk_op, vec![0, 2, 1, 0]);
        assert_eq!(encoded_chunk_op.len(), chunk_op.encoding_length().unwrap());

        let chunk_op = ChunkOp::Chunk(vec![Op::Push(Node::Hash([0; 32])), Op::Child]);
        let encoded_chunk_op = chunk_op.encode().unwrap();
        let mut expected_encoding = vec![1, 2];
        expected_encoding.extend(Op::Push(Node::Hash([0; 32])).encode().unwrap());
        expected_encoding.extend(Op::Child.encode().unwrap());
        assert_eq!(encoded_chunk_op, expected_encoding);
        assert_eq!(encoded_chunk_op.len(), chunk_op.encoding_length().unwrap());
    }

    #[test]
    fn test_chunk_op_decoding() {
        let encoded_chunk_op = vec![0, 3, 1, 0, 1];
        let decoded_chunk_op = ChunkOp::decode(encoded_chunk_op.as_slice()).unwrap();
        assert_eq!(decoded_chunk_op, ChunkOp::ChunkId(vec![LEFT, RIGHT, LEFT]));

        let mut encoded_chunk_op = vec![1, 2];
        encoded_chunk_op.extend(Op::Push(Node::Hash([1; 32])).encode().unwrap());
        encoded_chunk_op.extend(Op::Push(Node::KV(vec![1], vec![2])).encode().unwrap());
        let decoded_chunk_op = ChunkOp::decode(encoded_chunk_op.as_slice()).unwrap();
        assert_eq!(
            decoded_chunk_op,
            ChunkOp::Chunk(vec![
                Op::Push(Node::Hash([1; 32])),
                Op::Push(Node::KV(vec![1], vec![2]))
            ])
        );
    }
}
