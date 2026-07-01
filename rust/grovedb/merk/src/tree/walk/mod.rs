//! Merk tree walk

#[cfg(feature = "minimal")]
mod fetch;
#[cfg(feature = "minimal")]
mod ref_walker;

#[cfg(feature = "minimal")]
pub use fetch::Fetch;
#[cfg(feature = "minimal")]
use grovedb_costs::{cost_return_on_error, CostResult, CostsExt, OperationCost};
use grovedb_costs::{
    cost_return_on_error_no_add,
    storage_cost::{removal::StorageRemovedBytes, StorageCost},
};
use grovedb_version::version::GroveVersion;
#[cfg(feature = "minimal")]
pub use ref_walker::RefWalker;

#[cfg(feature = "minimal")]
use super::{Link, TreeNode};
use crate::tree::kv::ValueDefinedCostType;
#[cfg(feature = "minimal")]
use crate::{owner::Owner, tree::tree_feature_type::TreeFeatureType, CryptoHash, Error};

#[cfg(feature = "minimal")]
/// Allows traversal of a `Tree`, fetching from the given source when traversing
/// to a pruned node, detaching children as they are traversed.
pub struct Walker<S>
where
    S: Fetch + Sized + Clone,
{
    tree: Owner<TreeNode>,
    source: S,
}

#[cfg(feature = "minimal")]
impl<S> Walker<S>
where
    S: Fetch + Sized + Clone,
{
    /// Creates a `Walker` with the given tree and source.
    pub fn new(tree: TreeNode, source: S) -> Self {
        Self {
            tree: Owner::new(tree),
            source,
        }
    }

    /// Similar to `Tree#detach`, but yields a `Walker` which fetches from the
    /// same source as `self`. Returned tuple is `(updated_self,
    /// maybe_child_walker)`.
    pub fn detach<V>(
        mut self,
        left: bool,
        value_defined_cost_fn: Option<&V>,
        grove_version: &GroveVersion,
    ) -> CostResult<(Self, Option<Self>), Error>
    where
        V: Fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>,
    {
        let mut cost = OperationCost::default();

        let link = match self.tree.link(left) {
            None => return Ok((self, None)).wrap_with_cost(cost),
            Some(link) => link,
        };

        let child = if link.tree().is_some() {
            match self.tree.own_return(|t| t.detach(left)) {
                Some(child) => child,
                _ => unreachable!("Expected Some"),
            }
        } else {
            let link = self.tree.slot_mut(left).take();
            match link {
                Some(Link::Reference { .. }) => (),
                _ => unreachable!("Expected Some(Link::Reference)"),
            }
            cost_return_on_error!(
                &mut cost,
                self.source
                    .fetch(&link.unwrap(), value_defined_cost_fn, grove_version)
            )
        };

        let child = self.wrap(child);
        Ok((self, Some(child))).wrap_with_cost(cost)
    }

    /// Similar to `Tree#detach_expect`, but yields a `Walker` which fetches
    /// from the same source as `self`. Returned tuple is `(updated_self,
    /// child_walker)`.
    pub fn detach_expect<V>(
        self,
        left: bool,
        value_defined_cost_fn: Option<&V>,
        grove_version: &GroveVersion,
    ) -> CostResult<(Self, Self), Error>
    where
        V: Fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>,
    {
        self.detach(left, value_defined_cost_fn, grove_version)
            .map_ok(|(walker, maybe_child)| {
                if let Some(child) = maybe_child {
                    (walker, child)
                } else {
                    panic!(
                        "Expected {} child, got None",
                        if left { "left" } else { "right" }
                    );
                }
            })
    }

    /// Similar to `Tree#walk`, but yields a `Walker` which fetches from the
    /// same source as `self`.
    pub fn walk<F, T, V>(
        self,
        left: bool,
        f: F,
        value_defined_cost_fn: Option<&V>,
        grove_version: &GroveVersion,
    ) -> CostResult<Self, Error>
    where
        F: FnOnce(Option<Self>) -> CostResult<Option<T>, Error>,
        T: Into<TreeNode>,
        V: Fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>,
    {
        let mut cost = OperationCost::default();

        let (mut walker, maybe_child) = cost_return_on_error!(
            &mut cost,
            self.detach(left, value_defined_cost_fn, grove_version)
        );
        let new_child = match f(maybe_child).unwrap_add_cost(&mut cost) {
            Ok(x) => x.map(|t| t.into()),
            Err(e) => return Err(e).wrap_with_cost(cost),
        };
        walker.tree.own(|t| t.attach(left, new_child));
        Ok(walker).wrap_with_cost(cost)
    }

    /// Similar to `Tree#walk_expect` but yields a `Walker` which fetches from
    /// the same source as `self`.
    pub fn walk_expect<F, T, V>(
        self,
        left: bool,
        f: F,
        value_defined_cost_fn: Option<&V>,
        grove_version: &GroveVersion,
    ) -> CostResult<Self, Error>
    where
        F: FnOnce(Self) -> CostResult<Option<T>, Error>,
        T: Into<TreeNode>,
        V: Fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>,
    {
        let mut cost = OperationCost::default();

        let (mut walker, child) = cost_return_on_error!(
            &mut cost,
            self.detach_expect(left, value_defined_cost_fn, grove_version)
        );
        let new_child = match f(child).unwrap_add_cost(&mut cost) {
            Ok(x) => x.map(|t| t.into()),
            Err(e) => return Err(e).wrap_with_cost(cost),
        };
        walker.tree.own(|t| t.attach(left, new_child));
        Ok(walker).wrap_with_cost(cost)
    }

    /// Returns an immutable reference to the `Tree` wrapped by this walker.
    pub fn tree(&self) -> &TreeNode {
        &self.tree
    }

    /// Consumes the `Walker` and returns the `Tree` it wraps.
    pub fn into_inner(self) -> TreeNode {
        self.tree.into_inner()
    }

    /// Takes a `Tree` and returns a `Walker` which fetches from the same source
    /// as `self`.
    fn wrap(&self, tree: TreeNode) -> Self {
        Self::new(tree, self.source.clone())
    }

    /// Returns a clone of this `Walker`'s source.
    pub fn clone_source(&self) -> S {
        self.source.clone()
    }

    /// Similar to `Tree#attach`, but can also take a `Walker` since it
    /// implements `Into<Tree>`.
    pub fn attach<T>(mut self, left: bool, maybe_child: Option<T>) -> Self
    where
        T: Into<TreeNode>,
    {
        self.tree
            .own(|t| t.attach(left, maybe_child.map(|t| t.into())));
        self
    }

    /// Similar to `Tree#put_value`.
    pub fn put_value(
        mut self,
        value: Vec<u8>,
        feature_type: TreeFeatureType,
        old_specialized_cost: &impl Fn(&Vec<u8>, &Vec<u8>) -> Result<u32, Error>,
        get_temp_new_value_with_old_flags: &impl Fn(
            &Vec<u8>,
            &Vec<u8>,
        ) -> Result<Option<Vec<u8>>, Error>,
        update_tree_value_based_on_costs: &mut impl FnMut(
            &StorageCost,
            &Vec<u8>,
            &mut Vec<u8>,
        ) -> Result<
            (bool, Option<ValueDefinedCostType>),
            Error,
        >,
        section_removal_bytes: &mut impl FnMut(
            &Vec<u8>,
            u32,
            u32,
        ) -> Result<
            (StorageRemovedBytes, StorageRemovedBytes),
            Error,
        >,
    ) -> CostResult<Self, Error> {
        let mut cost = OperationCost::default();
        cost_return_on_error_no_add!(
            cost,
            self.tree.own_result(|t| t
                .put_value(
                    value,
                    feature_type,
                    old_specialized_cost,
                    get_temp_new_value_with_old_flags,
                    update_tree_value_based_on_costs,
                    section_removal_bytes
                )
                .unwrap_add_cost(&mut cost))
        );
        Ok(self).wrap_with_cost(cost)
    }

    /// Similar to `Tree#put_value_with_fixed_cost`.
    pub fn put_value_with_fixed_cost(
        mut self,
        value: Vec<u8>,
        value_fixed_cost: u32,
        feature_type: TreeFeatureType,
        old_specialized_cost: &impl Fn(&Vec<u8>, &Vec<u8>) -> Result<u32, Error>,
        get_temp_new_value_with_old_flags: &impl Fn(
            &Vec<u8>,
            &Vec<u8>,
        ) -> Result<Option<Vec<u8>>, Error>,
        update_tree_value_based_on_costs: &mut impl FnMut(
            &StorageCost,
            &Vec<u8>,
            &mut Vec<u8>,
        ) -> Result<
            (bool, Option<ValueDefinedCostType>),
            Error,
        >,
        section_removal_bytes: &mut impl FnMut(
            &Vec<u8>,
            u32,
            u32,
        ) -> Result<
            (StorageRemovedBytes, StorageRemovedBytes),
            Error,
        >,
    ) -> CostResult<Self, Error> {
        let mut cost = OperationCost::default();
        cost_return_on_error_no_add!(
            cost,
            self.tree.own_result(|t| t
                .put_value_with_fixed_cost(
                    value,
                    value_fixed_cost,
                    feature_type,
                    old_specialized_cost,
                    get_temp_new_value_with_old_flags,
                    update_tree_value_based_on_costs,
                    section_removal_bytes
                )
                .unwrap_add_cost(&mut cost))
        );
        Ok(self).wrap_with_cost(cost)
    }

    /// Similar to `Tree#put_value_and_reference_value_hash`.
    pub fn put_value_and_reference_value_hash(
        mut self,
        value: Vec<u8>,
        value_hash: CryptoHash,
        feature_type: TreeFeatureType,
        old_specialized_cost: &impl Fn(&Vec<u8>, &Vec<u8>) -> Result<u32, Error>,
        get_temp_new_value_with_old_flags: &impl Fn(
            &Vec<u8>,
            &Vec<u8>,
        ) -> Result<Option<Vec<u8>>, Error>,
        update_tree_value_based_on_costs: &mut impl FnMut(
            &StorageCost,
            &Vec<u8>,
            &mut Vec<u8>,
        ) -> Result<
            (bool, Option<ValueDefinedCostType>),
            Error,
        >,
        section_removal_bytes: &mut impl FnMut(
            &Vec<u8>,
            u32,
            u32,
        ) -> Result<
            (StorageRemovedBytes, StorageRemovedBytes),
            Error,
        >,
    ) -> CostResult<Self, Error> {
        let mut cost = OperationCost::default();
        cost_return_on_error_no_add!(
            cost,
            self.tree.own_result(|t| t
                .put_value_and_reference_value_hash(
                    value,
                    value_hash,
                    feature_type,
                    old_specialized_cost,
                    get_temp_new_value_with_old_flags,
                    update_tree_value_based_on_costs,
                    section_removal_bytes
                )
                .unwrap_add_cost(&mut cost))
        );
        Ok(self).wrap_with_cost(cost)
    }

    /// Similar to `Tree#put_value_with_reference_value_hash_and_value_cost`.
    pub fn put_value_with_reference_value_hash_and_value_cost(
        mut self,
        value: Vec<u8>,
        value_hash: CryptoHash,
        value_fixed_cost: u32,
        feature_type: TreeFeatureType,
        old_specialized_cost: &impl Fn(&Vec<u8>, &Vec<u8>) -> Result<u32, Error>,
        get_temp_new_value_with_old_flags: &impl Fn(
            &Vec<u8>,
            &Vec<u8>,
        ) -> Result<Option<Vec<u8>>, Error>,
        update_tree_value_based_on_costs: &mut impl FnMut(
            &StorageCost,
            &Vec<u8>,
            &mut Vec<u8>,
        ) -> Result<
            (bool, Option<ValueDefinedCostType>),
            Error,
        >,
        section_removal_bytes: &mut impl FnMut(
            &Vec<u8>,
            u32,
            u32,
        ) -> Result<
            (StorageRemovedBytes, StorageRemovedBytes),
            Error,
        >,
    ) -> CostResult<Self, Error> {
        let mut cost = OperationCost::default();
        cost_return_on_error_no_add!(
            cost,
            self.tree.own_result(|t| t
                .put_value_with_reference_value_hash_and_value_cost(
                    value,
                    value_hash,
                    value_fixed_cost,
                    feature_type,
                    old_specialized_cost,
                    get_temp_new_value_with_old_flags,
                    update_tree_value_based_on_costs,
                    section_removal_bytes
                )
                .unwrap_add_cost(&mut cost))
        );
        Ok(self).wrap_with_cost(cost)
    }
}

#[cfg(feature = "minimal")]
impl<S> From<Walker<S>> for TreeNode
where
    S: Fetch + Sized + Clone,
{
    fn from(walker: Walker<S>) -> Self {
        walker.into_inner()
    }
}

#[cfg(feature = "minimal")]
#[cfg(test)]
mod test {
    use grovedb_costs::CostsExt;
    use grovedb_version::version::GroveVersion;

    use super::{super::NoopCommit, *};
    use crate::tree::{AggregateData, TreeFeatureType::BasicMerkNode, TreeNode};

    #[derive(Clone)]
    struct MockSource {}

    impl Fetch for MockSource {
        fn fetch(
            &self,
            link: &Link,
            _value_defined_cost_fn: Option<
                &impl Fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>,
            >,
            _grove_version: &GroveVersion,
        ) -> CostResult<TreeNode, Error> {
            TreeNode::new(link.key().to_vec(), b"foo".to_vec(), None, BasicMerkNode).map(Ok)
        }
    }

    #[test]
    fn walk_modified() {
        let grove_version = GroveVersion::latest();
        let tree = TreeNode::new(b"test".to_vec(), b"abc".to_vec(), None, BasicMerkNode)
            .unwrap()
            .attach(
                true,
                Some(TreeNode::new(b"foo".to_vec(), b"bar".to_vec(), None, BasicMerkNode).unwrap()),
            );

        let source = MockSource {};
        let walker = Walker::new(tree, source);

        let walker = walker
            .walk(
                true,
                |child| -> CostResult<Option<TreeNode>, Error> {
                    assert_eq!(child.expect("should have child").tree().key(), b"foo");
                    Ok(None).wrap_with_cost(Default::default())
                },
                None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                grove_version,
            )
            .unwrap()
            .expect("walk failed");
        assert!(walker.into_inner().child(true).is_none());
    }

    #[test]
    fn walk_stored() {
        let grove_version = GroveVersion::latest();
        let mut tree = TreeNode::new(b"test".to_vec(), b"abc".to_vec(), None, BasicMerkNode)
            .unwrap()
            .attach(
                true,
                Some(TreeNode::new(b"foo".to_vec(), b"bar".to_vec(), None, BasicMerkNode).unwrap()),
            );
        tree.commit(&mut NoopCommit {}, &|_, _| Ok(0))
            .unwrap()
            .expect("commit failed");

        let source = MockSource {};
        let walker = Walker::new(tree, source);

        let walker = walker
            .walk(
                true,
                |child| -> CostResult<Option<TreeNode>, Error> {
                    assert_eq!(child.expect("should have child").tree().key(), b"foo");
                    Ok(None).wrap_with_cost(Default::default())
                },
                None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                grove_version,
            )
            .unwrap()
            .expect("walk failed");
        assert!(walker.into_inner().child(true).is_none());
    }

    #[test]
    fn walk_pruned() {
        let grove_version = GroveVersion::latest();
        let tree = TreeNode::from_fields(
            b"test".to_vec(),
            b"abc".to_vec(),
            Default::default(),
            Some(Link::Reference {
                hash: Default::default(),
                key: b"foo".to_vec(),
                child_heights: (0, 0),
                aggregate_data: AggregateData::NoAggregateData,
            }),
            None,
            BasicMerkNode,
        )
        .unwrap();

        let source = MockSource {};
        let walker = Walker::new(tree, source);

        let walker = walker
            .walk_expect(
                true,
                |child| -> CostResult<Option<TreeNode>, Error> {
                    assert_eq!(child.tree().key(), b"foo");
                    Ok(None).wrap_with_cost(Default::default())
                },
                None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                grove_version,
            )
            .unwrap()
            .expect("walk failed");
        assert!(walker.into_inner().child(true).is_none());
    }

    #[test]
    fn walk_none() {
        let grove_version = GroveVersion::latest();
        let tree = TreeNode::new(b"test".to_vec(), b"abc".to_vec(), None, BasicMerkNode).unwrap();

        let source = MockSource {};
        let walker = Walker::new(tree, source);

        walker
            .walk(
                true,
                |child| -> CostResult<Option<TreeNode>, Error> {
                    assert!(child.is_none());
                    Ok(None).wrap_with_cost(Default::default())
                },
                None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
                grove_version,
            )
            .unwrap()
            .expect("walk failed");
    }
}
