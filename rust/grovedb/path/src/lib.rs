// MIT LICENSE
//
// Copyright (c) 2023 Dash Core Group
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

//! GroveDB subtree path manipulation library.

#![deny(missing_docs)]

mod subtree_path;
mod subtree_path_builder;
mod subtree_path_iter;
mod util;

pub use subtree_path::SubtreePath;
pub use subtree_path_builder::SubtreePathBuilder;
pub use subtree_path_iter::SubtreePathIter;

#[cfg(test)]
mod tests {
    use super::*;
    use crate::util::calculate_hash;

    fn assert_path_properties<B>(path: SubtreePath<'_, B>, reference: Vec<Vec<u8>>)
    where
        B: AsRef<[u8]> + std::fmt::Debug,
    {
        // Assert `to_vec`
        assert_eq!(path.to_vec(), reference);

        // Assert `into_reverse_iter`
        assert!(path.clone().into_reverse_iter().eq(reference.iter().rev()));

        // Assert equality
        assert_eq!(path, SubtreePath::from(reference.as_slice()));

        // Assert hashing done properly
        let subtree_path_ref = SubtreePath::from(reference.as_slice());
        let subtree_path_builder = subtree_path_ref.derive_owned();
        assert_eq!(calculate_hash(&path), calculate_hash(&subtree_path_ref));
        assert_eq!(calculate_hash(&path), calculate_hash(&subtree_path_builder));
        assert_eq!(path.len(), reference.len());
    }

    #[test]
    fn test_root_and_roots_child_derivation_slice() {
        // Go two levels down just to complicate our test a bit:
        let path_array = [b"one", b"two"];
        let path = SubtreePath::from(path_array.as_ref());

        let (root, child) = path.derive_parent().unwrap().0.derive_parent().unwrap();

        assert_eq!(child, b"one");
        assert_eq!(root.to_vec(), Vec::<&[u8]>::new());

        assert_eq!(root.derive_parent(), None);
    }

    #[test]
    fn test_root_and_roots_child_derivation_builder() {
        let mut builder = SubtreePathBuilder::new();
        builder.push_segment(b"one");
        builder.push_segment(b"two");
        let path: SubtreePath<[u8; 0]> = (&builder).into();

        let (root, child) = path.derive_parent().unwrap().0.derive_parent().unwrap();

        assert_eq!(child, b"one");
        assert_eq!(root.to_vec(), Vec::<&[u8]>::new());

        assert_eq!(root.derive_parent(), None);
    }

    #[test]
    fn test_hashes_are_equal() {
        let path_array = [
            b"one".to_vec(),
            b"two".to_vec(),
            b"three".to_vec(),
            b"four".to_vec(),
            b"five".to_vec(),
        ];
        let path_array_refs = [
            b"one".as_ref(),
            b"two".as_ref(),
            b"three".as_ref(),
            b"four".as_ref(),
            b"five".as_ref(),
        ];
        let path_base_slice_slices = SubtreePath::from(path_array_refs.as_ref());

        let path_array_refs_six = [
            b"one".as_ref(),
            b"two".as_ref(),
            b"three".as_ref(),
            b"four".as_ref(),
            b"five".as_ref(),
            b"six".as_ref(),
        ];
        let path_base_slice_too_much = SubtreePath::from(path_array_refs_six.as_ref());
        let path_base_unfinished = SubtreePath::from([b"one", b"two"].as_ref());
        let path_empty = SubtreePathBuilder::new();

        let path_derived_11 = path_empty.derive_owned_with_child(b"one".as_ref());
        let path_derived_12 = path_derived_11.derive_owned_with_child(b"two".as_ref());
        let path_derived_13 = path_derived_12.derive_owned_with_child(b"three".as_ref());
        let path_derived_14 = path_derived_13.derive_owned_with_child(b"four".to_vec());
        let path_derived_1 = path_derived_14.derive_owned_with_child(b"five".as_ref());

        let (path_derived_2, _) = path_base_slice_too_much.derive_parent().unwrap();

        let path_derived_31 = path_base_unfinished.derive_owned_with_child(b"three".to_vec());
        let path_derived_32 = path_derived_31.derive_owned_with_child(b"four".as_ref());
        let path_derived_3 = path_derived_32.derive_owned_with_child(b"five".as_ref());

        assert_path_properties(path_base_slice_slices, path_array.to_vec());
        assert_path_properties(SubtreePath::from(&path_derived_1), path_array.to_vec());
        assert_path_properties(path_derived_2, path_array.to_vec());
        assert_path_properties(SubtreePath::from(&path_derived_3), path_array.to_vec());
    }

    #[test]
    fn test_is_root() {
        let path_empty = SubtreePathBuilder::new();
        assert!(path_empty.is_root());

        let path_derived = path_empty.derive_owned_with_child(b"two".as_ref());
        assert!(!path_derived.is_root());
        assert!(path_derived.derive_parent().unwrap().0.is_root());

        let path_not_empty = SubtreePath::from([b"one"].as_ref());
        assert!(!path_not_empty.is_root());
        assert!(path_not_empty.derive_parent().unwrap().0.is_root());
    }

    #[test]
    fn test_complex_derivation() {
        // Append only operations:
        let base = SubtreePath::from([b"one", b"two"].as_ref());
        let with_child_1 = base.derive_owned_with_child(b"three".to_vec());
        let mut with_child_inplace = with_child_1.derive_owned_with_child(b"four");
        with_child_inplace.push_segment(b"five");
        with_child_inplace.push_segment(b"six");
        with_child_inplace.push_segment(b"seven");
        with_child_inplace.push_segment(b"eight");

        // `with_child_inplace` should be like (substituted for digits for short):
        // [1, 2] -> 3 -> [4, 5, 6, 7, 8]
        assert_eq!(
            with_child_inplace.reverse_iter().fold(0, |acc, _| acc + 1),
            8
        );
        assert_path_properties(
            (&with_child_inplace).into(),
            vec![
                b"one".to_vec(),
                b"two".to_vec(),
                b"three".to_vec(),
                b"four".to_vec(),
                b"five".to_vec(),
                b"six".to_vec(),
                b"seven".to_vec(),
                b"eight".to_vec(),
            ],
        );

        // Now go to ancestors (note that intermediate derivations are dropped and
        // is still compiles, as [SubtreePath]s are intended for):
        let points_five = with_child_inplace
            .derive_parent()
            .unwrap()
            .0
            .derive_parent()
            .unwrap()
            .0
            .derive_parent()
            .unwrap()
            .0;

        assert_path_properties(
            points_five.clone(),
            vec![
                b"one".to_vec(),
                b"two".to_vec(),
                b"three".to_vec(),
                b"four".to_vec(),
                b"five".to_vec(),
            ],
        );

        // And add a couple of other derivations
        let after_five_1 = points_five.derive_owned_with_child(b"four");
        let after_five_2 = after_five_1.derive_owned_with_child(b"twenty");
        let mut after_five_3 = after_five_2.derive_owned();
        after_five_3.push_segment(b"thirteen");
        after_five_3.push_segment(b"thirtyseven");

        // `after_five_3` should be like this:
        // [1, 2] -> 3 -> [4, 5, 6, 7, 8]
        //                    ^-> 4 -> 20 -> [13, 37]
        assert_path_properties(
            (&after_five_3).into(),
            vec![
                b"one".to_vec(),
                b"two".to_vec(),
                b"three".to_vec(),
                b"four".to_vec(),
                b"five".to_vec(),
                b"four".to_vec(),
                b"twenty".to_vec(),
                b"thirteen".to_vec(),
                b"thirtyseven".to_vec(),
            ],
        );
    }
}
