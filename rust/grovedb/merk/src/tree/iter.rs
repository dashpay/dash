//! Merk tree iterator

#[cfg(feature = "minimal")]
use super::TreeNode;

#[cfg(feature = "minimal")]
/// An entry stored on an `Iter`'s stack, containing a reference to a `Tree`,
/// and its traversal state.
///
/// The `traversed` field represents whether or not the left child, self, and
/// right child have been visited, respectively (`(left, self, right)`).
struct StackItem<'a> {
    tree: &'a TreeNode,
    traversed: (bool, bool, bool),
}

#[cfg(feature = "minimal")]
impl<'a> StackItem<'a> {
    /// Creates a new `StackItem` for the given tree. The `traversed` state will
    /// be `false` since the children and self have not been visited yet, but
    /// will default to `true` for sides that do not have a child.
    const fn new(tree: &'a TreeNode) -> Self {
        StackItem {
            tree,
            traversed: (
                tree.child(true).is_none(),
                false,
                tree.child(false).is_none(),
            ),
        }
    }

    /// Gets a tuple to yield from an `Iter`, `(key, value)`.
    fn to_entry(&self) -> (Vec<u8>, Vec<u8>) {
        (
            self.tree.key().to_vec(),
            self.tree.value_as_slice().to_vec(),
        )
    }
}

#[cfg(feature = "minimal")]
/// An iterator which yields the key/value pairs of the tree, in order, skipping
/// any parts of the tree which are pruned (not currently retained in memory).
pub struct Iter<'a> {
    stack: Vec<StackItem<'a>>,
}

#[cfg(feature = "minimal")]
impl<'a> Iter<'a> {
    /// Creates a new iterator for the given tree.
    pub fn new(tree: &'a TreeNode) -> Self {
        let stack = vec![StackItem::new(tree)];
        Iter { stack }
    }
}

#[cfg(feature = "minimal")]
impl<'a> TreeNode {
    /// Creates an iterator which yields `(key, value)` tuples for all of the
    /// tree's nodes which are retained in memory (skipping pruned subtrees).
    pub fn iter(&'a self) -> Iter<'a> {
        Iter::new(self)
    }
}

#[cfg(feature = "minimal")]
impl Iterator for Iter<'_> {
    type Item = (Vec<u8>, Vec<u8>);

    /// Traverses to and yields the next key/value pair, in key order.
    fn next(&mut self) -> Option<Self::Item> {
        if self.stack.is_empty() {
            return None;
        }

        let last = self.stack.last_mut().unwrap();
        if !last.traversed.0 {
            last.traversed.0 = true;
            let tree = last.tree.child(true).unwrap();
            self.stack.push(StackItem::new(tree));
            self.next()
        } else if !last.traversed.1 {
            last.traversed.1 = true;
            Some(last.to_entry())
        } else if !last.traversed.2 {
            last.traversed.2 = true;
            let tree = last.tree.child(false).unwrap();
            self.stack.push(StackItem::new(tree));
            self.next()
        } else {
            self.stack.pop();
            self.next()
        }
    }
}
