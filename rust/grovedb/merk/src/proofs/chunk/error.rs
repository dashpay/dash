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

#[derive(Debug, thiserror::Error)]
/// Chunk related errors
pub enum ChunkError {
    /// Limit too small for first chunk, cannot make progress
    #[error("overflow error {0}")]
    LimitTooSmall(&'static str),

    /// Chunk index out of bounds
    #[error("chunk index out of bounds: {0}")]
    OutOfBounds(&'static str),

    /// Empty tree contains no chunks
    #[error("chunk from empty tree: {0}")]
    EmptyTree(&'static str),

    /// Invalid traversal instruction (points to no element)
    #[error("traversal instruction invalid {0}")]
    BadTraversalInstruction(&'static str),

    /// Expected ChunkId when parsing chunk ops
    #[error("expected chunk id when parsing chunk op")]
    ExpectedChunkId,

    /// Expected Chunk when parsing chunk ops
    #[error("expected chunk when parsing chunk op")]
    ExpectedChunk,

    // Restoration Errors
    /// Chunk restoration starts from the root chunk, this lead to a set of
    /// root hash values to verify other chunks ....
    /// Hence before you can verify a child you need to have verified it's
    /// parent.
    #[error("unexpected chunk: cannot verify chunk because verification hash is not in memory")]
    UnexpectedChunk,

    /// Invalid chunk proof when verifying chunk
    #[error("invalid chunk proof: {0}")]
    InvalidChunkProof(&'static str),

    /// Invalid multi chunk
    #[error("invalid multi chunk: {0}")]
    InvalidMultiChunk(&'static str),

    #[error("called finalize too early still expecting chunks")]
    RestorationNotComplete,

    /// Internal error, this should never surface
    /// if it does, it means wrong assumption in code
    #[error("internal error {0}")]
    InternalError(&'static str),
}
