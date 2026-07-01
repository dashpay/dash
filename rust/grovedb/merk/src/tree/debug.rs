//! Merk tree debug

use std::fmt::{Debug, Formatter, Result};

#[cfg(feature = "colored")]
use colored::Colorize;

use super::{Link, TreeNode};

#[cfg(all(feature = "minimal", feature = "colored"))]
impl Debug for TreeNode {
    // TODO: unwraps should be results that bubble up
    fn fmt(&self, f: &mut Formatter) -> Result {
        fn traverse(
            f: &mut Formatter,
            cursor: &TreeNode,
            stack: &mut Vec<(Vec<u8>, Vec<u8>)>,
            left: bool,
        ) {
            if let Some(child_link) = cursor.link(true) {
                stack.push((child_link.key().to_vec(), cursor.key().to_vec()));
                if let Some(child_tree) = child_link.tree() {
                    traverse(f, child_tree, stack, true);
                } else {
                    traverse_pruned(f, child_link, stack, true);
                }
                stack.pop();
            }

            let depth = stack.len();
            if depth > 0 {
                // draw ancestor's vertical lines
                for (low, high) in stack.iter().take(depth - 1) {
                    let draw_line = cursor.key() > low && cursor.key() < high;
                    write!(f, "{}", if draw_line { " │  " } else { "    " }.dimmed()).unwrap();
                }
            }

            let prefix = if depth == 0 {
                ""
            } else if left {
                " ┌-"
            } else {
                " └-"
            };
            writeln!(
                f,
                "{}{}",
                prefix.dimmed(),
                format!("{:?}", cursor.key()).on_bright_black()
            )
            .unwrap();

            if let Some(child_link) = cursor.link(false) {
                stack.push((cursor.key().to_vec(), child_link.key().to_vec()));
                if let Some(child_tree) = child_link.tree() {
                    traverse(f, child_tree, stack, false);
                } else {
                    traverse_pruned(f, child_link, stack, false);
                }
                stack.pop();
            }
        }

        fn traverse_pruned(
            f: &mut Formatter,
            link: &Link,
            stack: &mut [(Vec<u8>, Vec<u8>)],
            left: bool,
        ) {
            let depth = stack.len();

            if depth > 0 {
                // draw ancestor's vertical lines
                for (low, high) in stack.iter().take(depth - 1) {
                    let draw_line = link.key() > low && link.key() < high;
                    write!(f, "{}", if draw_line { " │  " } else { "    " }.dimmed()).unwrap();
                }
            }

            let prefix = if depth == 0 {
                ""
            } else if left {
                " ┌-"
            } else {
                " └-"
            };
            writeln!(
                f,
                "{}{}",
                prefix.dimmed(),
                format!("{:?}", link.key()).blue()
            )
            .unwrap();
        }

        let mut stack = vec![];
        traverse(f, self, &mut stack, false);
        writeln!(f)
    }
}

#[cfg(all(feature = "minimal", not(feature = "colored")))]
impl Debug for TreeNode {
    fn fmt(&self, f: &mut Formatter) -> Result {
        fn traverse(
            f: &mut Formatter,
            cursor: &TreeNode,
            stack: &mut Vec<(Vec<u8>, Vec<u8>)>,
            left: bool,
        ) {
            if let Some(child_link) = cursor.link(true) {
                stack.push((child_link.key().to_vec(), cursor.key().to_vec()));
                if let Some(child_tree) = child_link.tree() {
                    traverse(f, child_tree, stack, true);
                } else {
                    traverse_pruned(f, child_link, stack, true);
                }
                stack.pop();
            }

            let depth = stack.len();
            if depth > 0 {
                for (low, high) in stack.iter().take(depth - 1) {
                    let draw_line = cursor.key() > low && cursor.key() < high;
                    write!(f, "{}", if draw_line { " │  " } else { "    " }).unwrap();
                }
            }

            let prefix = if depth == 0 {
                ""
            } else if left {
                " ┌-"
            } else {
                " └-"
            };
            writeln!(f, "{}{:?}", prefix, cursor.key()).unwrap();

            if let Some(child_link) = cursor.link(false) {
                stack.push((cursor.key().to_vec(), child_link.key().to_vec()));
                if let Some(child_tree) = child_link.tree() {
                    traverse(f, child_tree, stack, false);
                } else {
                    traverse_pruned(f, child_link, stack, false);
                }
                stack.pop();
            }
        }

        fn traverse_pruned(
            f: &mut Formatter,
            link: &Link,
            stack: &mut [(Vec<u8>, Vec<u8>)],
            left: bool,
        ) {
            let depth = stack.len();
            if depth > 0 {
                for (low, high) in stack.iter().take(depth - 1) {
                    let draw_line = link.key() > low && link.key() < high;
                    write!(f, "{}", if draw_line { " │  " } else { "    " }).unwrap();
                }
            }

            let prefix = if depth == 0 {
                ""
            } else if left {
                " ┌-"
            } else {
                " └-"
            };
            writeln!(f, "{}{:?}", prefix, link.key()).unwrap();
        }

        let mut stack = vec![];
        traverse(f, self, &mut stack, false);
        writeln!(f)
    }
}
