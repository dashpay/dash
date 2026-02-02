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

//! Visualize

use std::io::{Result, Write};

use grovedb_storage::StorageContext;
use grovedb_visualize::{Drawer, Visualize};

use crate::{tree::TreeNode, Merk};

/// Visualizeable Merk
pub struct VisualizeableMerk<'a, S, F> {
    merk: &'a Merk<S>,
    deserialize_fn: F,
}

impl<'a, S, F> VisualizeableMerk<'a, S, F> {
    /// New
    pub fn new(merk: &'a Merk<S>, deserialize_fn: F) -> Self {
        Self {
            merk,
            deserialize_fn,
        }
    }
}

struct VisualizableTree<'a, F> {
    tree: &'a TreeNode,
    deserialize_fn: F,
}

impl<'a, F> VisualizableTree<'a, F> {
    fn new(tree: &'a TreeNode, deserialize_fn: F) -> Self {
        Self {
            tree,
            deserialize_fn,
        }
    }
}

impl<'db, S: StorageContext<'db>, T: Visualize, F: Fn(&[u8]) -> T + Copy> Visualize
    for VisualizeableMerk<'_, S, F>
{
    fn visualize<W: Write>(&self, mut drawer: Drawer<W>) -> Result<Drawer<W>> {
        drawer.write(b"Merk root: ")?;
        drawer = self.merk.use_tree(|tree| {
            if let Some(t) = tree {
                VisualizableTree::new(t, self.deserialize_fn).visualize(drawer)
            } else {
                drawer.write(b"empty")?;
                Ok(drawer)
            }
        })?;
        drawer.flush()?;

        Ok(drawer)
    }
}

impl<T: Visualize, F: Fn(&[u8]) -> T + Copy> Visualize for VisualizableTree<'_, F> {
    fn visualize<W: Write>(&self, mut drawer: Drawer<W>) -> Result<Drawer<W>> {
        drawer.write(b"[key: ")?;
        drawer = self.tree.inner.kv.key_as_ref().visualize(drawer)?;
        drawer.write(b", value: ")?;
        drawer = (self.deserialize_fn)(self.tree.inner.kv.value_as_slice()).visualize(drawer)?;

        drawer.down();
        drawer.write(b"\n")?;

        drawer.write(b"left: ")?;
        drawer = self
            .tree
            .child(true)
            .map(|tree| Self::new(tree, self.deserialize_fn))
            .visualize(drawer)?;
        drawer.write(b"\n")?;

        drawer.write(b"right: ")?;
        drawer = self
            .tree
            .child(false)
            .map(|tree| Self::new(tree, self.deserialize_fn))
            .visualize(drawer)?;

        drawer.up();
        Ok(drawer)
    }
}
