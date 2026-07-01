use std::cmp::Ordering;

use grovedb_costs::{
    storage_cost::{
        removal::{StorageRemovedBytes, StorageRemovedBytes::BasicStorageRemoval},
        StorageCost,
    },
    CostResult, CostsExt,
};
use grovedb_storage::StorageContext;
use grovedb_version::version::GroveVersion;

use crate::{
    merk::NodeType,
    tree::{
        kv::{ValueDefinedCostType, KV},
        AuxMerkBatch, Walker,
    },
    Error, Merk, MerkBatch, MerkOptions,
};

impl<'db, S> Merk<S>
where
    S: StorageContext<'db>,
{
    /// Applies a batch of operations (puts and deletes) to the tree.
    ///
    /// This will fail if the keys in `batch` are not sorted and unique. This
    /// check creates some overhead, so if you are sure your batch is sorted and
    /// unique you can use the unsafe `apply_unchecked` for a small performance
    /// gain.
    ///
    /// # Example
    /// ```
    /// # let grove_version = GroveVersion::latest();
    /// # let mut store = grovedb_merk::test_utils::TempMerk::new(grove_version);
    /// # store.apply::<_, Vec<_>>(
    ///     &[(vec![4,5,6],
    ///     Op::Put(vec![0], BasicMerkNode))],
    ///     &[],
    ///     None,
    ///     grove_version
    ///  ).unwrap().expect("");
    ///
    /// use grovedb_merk::Op;
    /// use grovedb_merk::TreeFeatureType::BasicMerkNode;
    /// use grovedb_version::version::GroveVersion;
    ///
    /// let batch = &[
    ///     // puts value [4,5,6] to key[1,2,3]
    ///     (vec![1, 2, 3], Op::Put(vec![4, 5, 6], BasicMerkNode)),
    ///     // deletes key [4,5,6]
    ///     (vec![4, 5, 6], Op::Delete),
    /// ];
    /// store.apply::<_, Vec<_>>(batch, &[], None,grove_version).unwrap().expect("");
    /// ```
    pub fn apply<KB, KA>(
        &mut self,
        batch: &MerkBatch<KB>,
        aux: &AuxMerkBatch<KA>,
        options: Option<MerkOptions>,
        grove_version: &GroveVersion,
    ) -> CostResult<(), Error>
    where
        KB: AsRef<[u8]>,
        KA: AsRef<[u8]>,
    {
        let node_type: NodeType = self.tree_type.inner_node_type();
        self.apply_with_costs_just_in_time_value_update(
            batch,
            aux,
            options,
            &|key, value| {
                Ok(KV::layered_value_byte_cost_size_for_key_and_value_lengths(
                    key.len() as u32,
                    value.len() as u32,
                    node_type,
                ))
            },
            None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
            &|_old_value, _value| Ok(None),
            &mut |_costs, _old_value, _value| Ok((false, None)),
            &mut |_a, key_bytes_to_remove, value_bytes_to_remove| {
                Ok((
                    BasicStorageRemoval(key_bytes_to_remove),
                    BasicStorageRemoval(value_bytes_to_remove),
                ))
            },
            grove_version,
        )
    }

    /// Applies a batch of operations (puts and deletes) to the tree.
    ///
    /// This will fail if the keys in `batch` are not sorted and unique. This
    /// check creates some overhead, so if you are sure your batch is sorted and
    /// unique you can use the unsafe `apply_unchecked` for a small performance
    /// gain.
    ///
    /// # Example
    /// ```
    /// # let grove_version = GroveVersion::latest();
    /// # let mut store = grovedb_merk::test_utils::TempMerk::new(grove_version);
    /// # store.apply::<_, Vec<_>>(
    ///     &[(vec![4,5,6],
    ///     Op::Put(vec![0], BasicMerkNode))],
    ///     &[],
    ///     None,
    ///     grove_version
    /// ).unwrap().expect("");
    ///
    /// use grovedb_merk::Op;
    /// use grovedb_merk::TreeFeatureType::BasicMerkNode;
    /// use grovedb_version::version::GroveVersion;
    ///
    /// let batch = &[
    ///     // puts value [4,5,6] to key[1,2,3]
    ///     (vec![1, 2, 3], Op::Put(vec![4, 5, 6], BasicMerkNode)),
    ///     // deletes key [4,5,6]
    ///     (vec![4, 5, 6], Op::Delete),
    /// ];
    /// store.apply::<_, Vec<_>>(batch, &[], None,grove_version).unwrap().expect("");
    /// ```
    pub fn apply_with_specialized_costs<KB, KA>(
        &mut self,
        batch: &MerkBatch<KB>,
        aux: &AuxMerkBatch<KA>,
        options: Option<MerkOptions>,
        old_specialized_cost: &impl Fn(&Vec<u8>, &Vec<u8>) -> Result<u32, Error>,
        value_defined_cost_fn: Option<
            &impl Fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>,
        >,
        grove_version: &GroveVersion,
    ) -> CostResult<(), Error>
    where
        KB: AsRef<[u8]>,
        KA: AsRef<[u8]>,
    {
        self.apply_with_costs_just_in_time_value_update(
            batch,
            aux,
            options,
            old_specialized_cost,
            value_defined_cost_fn,
            &|_, _| Ok(None),
            &mut |_costs, _old_value, _value| Ok((false, None)),
            &mut |_a, key_bytes_to_remove, value_bytes_to_remove| {
                Ok((
                    BasicStorageRemoval(key_bytes_to_remove),
                    BasicStorageRemoval(value_bytes_to_remove),
                ))
            },
            grove_version,
        )
    }

    /// Applies a batch of operations (puts and deletes) to the tree with the
    /// ability to update values based on costs.
    ///
    /// This will fail if the keys in `batch` are not sorted and unique. This
    /// check creates some overhead, so if you are sure your batch is sorted and
    /// unique you can use the unsafe `apply_unchecked` for a small performance
    /// gain.
    ///
    /// # Example
    /// ```
    /// # let grove_version = GroveVersion::latest();
    /// # let mut store = grovedb_merk::test_utils::TempMerk::new(grove_version);
    /// # store.apply_with_costs_just_in_time_value_update::<_, Vec<_>>(    /// /// /// ///
    ///     &[(vec![4,5,6], Op::Put(vec![0], BasicMerkNode))],
    ///     &[],
    ///     None,
    ///     &|k, v| Ok(0),
    ///     None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
    ///     &|k, v| Ok(None),
    ///     &mut |s, v, o| Ok((false, None)),
    ///     &mut |s, k, v| Ok((NoStorageRemoval, NoStorageRemoval)),
    ///     grove_version,
    /// ).unwrap().expect("");
    ///
    /// use grovedb_costs::storage_cost::removal::StorageRemovedBytes::NoStorageRemoval;
    /// use grovedb_merk::Op;
    /// use grovedb_merk::tree::kv::ValueDefinedCostType;
    /// use grovedb_merk::TreeFeatureType::BasicMerkNode;
    /// use grovedb_version::version::GroveVersion;
    ///
    /// let batch = &[
    ///     // puts value [4,5,6] to key[1,2,3]
    ///     (vec![1, 2, 3], Op::Put(vec![4, 5, 6], BasicMerkNode)),
    ///     // deletes key [4,5,6]
    ///     (vec![4, 5, 6], Op::Delete),
    /// ];
    ///
    /// store.apply_with_costs_just_in_time_value_update::<_, Vec<_>>(
    /// batch,
    ///     &[],
    ///     None,
    ///     &|k, v| Ok(0),
    ///     None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
    ///     &|k, v| Ok(None),
    ///     &mut |s, v, o| Ok((false, None)),
    ///     &mut |s, k, v| Ok((NoStorageRemoval, NoStorageRemoval)),
    ///     grove_version,
    /// ).unwrap().expect("");
    /// ```
    pub fn apply_with_costs_just_in_time_value_update<KB, KA>(
        &mut self,
        batch: &MerkBatch<KB>,
        aux: &AuxMerkBatch<KA>,
        options: Option<MerkOptions>,
        old_specialized_cost: &impl Fn(&Vec<u8>, &Vec<u8>) -> Result<u32, Error>,
        value_defined_cost_fn: Option<
            &impl Fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>,
        >,
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
        grove_version: &GroveVersion,
    ) -> CostResult<(), Error>
    where
        KB: AsRef<[u8]>,
        KA: AsRef<[u8]>,
    {
        // ensure keys in batch are sorted and unique
        let mut maybe_prev_key: Option<&KB> = None;
        for (key, ..) in batch.iter() {
            if let Some(prev_key) = maybe_prev_key {
                match prev_key.as_ref().cmp(key.as_ref()) {
                    Ordering::Greater => {
                        return Err(Error::InvalidInputError("Keys in batch must be sorted"))
                            .wrap_with_cost(Default::default())
                    }
                    Ordering::Equal => {
                        return Err(Error::InvalidInputError("Keys in batch must be unique"))
                            .wrap_with_cost(Default::default())
                    }
                    _ => (),
                }
            }
            maybe_prev_key = Some(key);
        }

        self.apply_unchecked(
            batch,
            aux,
            options,
            old_specialized_cost,
            value_defined_cost_fn,
            get_temp_new_value_with_old_flags,
            update_tree_value_based_on_costs,
            section_removal_bytes,
            grove_version,
        )
    }

    /// Applies a batch of operations (puts and deletes) to the tree.
    ///
    /// # Safety
    /// This is unsafe because the keys in `batch` must be sorted and unique -
    /// if they are not, there will be undefined behavior. For a safe version of
    /// this method which checks to ensure the batch is sorted and unique, see
    /// `apply`.
    ///
    /// # Example
    /// ```
    /// # let grove_version = GroveVersion::latest();
    /// # let mut store = grovedb_merk::test_utils::TempMerk::new(grove_version);
    /// # store.apply_with_costs_just_in_time_value_update::<_, Vec<_>>(    /// /// /// ///
    ///     &[(vec![4,5,6], Op::Put(vec![0], BasicMerkNode))],
    ///     &[],
    ///     None,
    ///     &|k, v| Ok(0),
    ///     None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
    ///     &|k, v| Ok(None),
    ///     &mut |s, o, v| Ok((false, None)),
    ///     &mut |s, k, v| Ok((NoStorageRemoval, NoStorageRemoval)),
    ///     grove_version,
    /// ).unwrap().expect("");
    ///
    /// use grovedb_costs::storage_cost::removal::StorageRemovedBytes::NoStorageRemoval;
    /// use grovedb_merk::Op;
    /// use grovedb_merk::tree::kv::ValueDefinedCostType;
    /// use grovedb_merk::TreeFeatureType::BasicMerkNode;
    /// use grovedb_version::version::GroveVersion;
    ///
    /// let batch = &[
    ///     // puts value [4,5,6] to key [1,2,3]
    ///     (vec![1, 2, 3], Op::Put(vec![4, 5, 6], BasicMerkNode)),
    ///     // deletes key [4,5,6]
    ///     (vec![4, 5, 6], Op::Delete),
    /// ];
    /// unsafe { store.apply_unchecked::<_, Vec<_>, _, _, _, _, _>(
    /// batch,
    ///     &[],
    ///     None,
    ///     &|k, v| Ok(0),
    ///     None::<&fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>>,
    ///     &|o, v| Ok(None),
    ///     &mut |s, o, v| Ok((false, None)),
    ///     &mut |s, k, v| Ok((NoStorageRemoval, NoStorageRemoval)),
    ///     grove_version,
    /// ).unwrap().expect("");
    /// }
    /// ```
    pub fn apply_unchecked<KB, KA, C, V, T, U, R>(
        &mut self,
        batch: &MerkBatch<KB>,
        aux: &AuxMerkBatch<KA>,
        options: Option<MerkOptions>,
        old_specialized_cost: &C,
        value_defined_cost_fn: Option<&V>,
        get_temp_new_value_with_old_flags: &T,
        update_tree_value_based_on_costs: &mut U,
        section_removal_bytes: &mut R,
        grove_version: &GroveVersion,
    ) -> CostResult<(), Error>
    where
        KB: AsRef<[u8]>,
        KA: AsRef<[u8]>,
        C: Fn(&Vec<u8>, &Vec<u8>) -> Result<u32, Error>,
        V: Fn(&[u8], &GroveVersion) -> Option<ValueDefinedCostType>,
        T: Fn(&Vec<u8>, &Vec<u8>) -> Result<Option<Vec<u8>>, Error>,
        U: FnMut(
            &StorageCost,
            &Vec<u8>,
            &mut Vec<u8>,
        ) -> Result<(bool, Option<ValueDefinedCostType>), Error>,
        R: FnMut(&Vec<u8>, u32, u32) -> Result<(StorageRemovedBytes, StorageRemovedBytes), Error>,
    {
        let maybe_walker = self
            .tree
            .take()
            .take()
            .map(|tree| Walker::new(tree, self.source()));

        Walker::apply_to(
            maybe_walker,
            batch,
            self.source(),
            old_specialized_cost,
            value_defined_cost_fn,
            get_temp_new_value_with_old_flags,
            update_tree_value_based_on_costs,
            section_removal_bytes,
            grove_version,
        )
        .flat_map_ok(|(maybe_tree, key_updates)| {
            // we set the new root node of the merk tree
            self.tree.set(maybe_tree);
            // commit changes to db
            self.commit(key_updates, aux, options, old_specialized_cost)
        })
    }
}
