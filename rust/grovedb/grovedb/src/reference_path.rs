//! Space efficient methods for referencing other elements in GroveDB

use std::collections::HashSet;

use grovedb_costs::{cost_return_on_error, cost_return_on_error_into_no_add, CostResult, CostsExt};
pub use grovedb_element::reference_path::*;
use grovedb_merk::{element::get::ElementFetchFromStorageExtensions, CryptoHash};
use grovedb_path::SubtreePathBuilder;
use grovedb_version::check_grovedb_v0_with_cost;

use crate::{
    merk_cache::{MerkCache, MerkHandle},
    operations::MAX_REFERENCE_HOPS,
    Element, Error,
};

pub(crate) struct ResolvedReference<'db, 'b, 'c, B> {
    pub target_merk: MerkHandle<'db, 'c>,
    pub target_path: SubtreePathBuilder<'b, B>,
    pub target_key: Vec<u8>,
    pub target_element: Element,
    pub target_node_value_hash: CryptoHash,
}

pub(crate) fn follow_reference<'db, 'b, 'c, B: AsRef<[u8]>>(
    merk_cache: &'c MerkCache<'db, 'b, B>,
    path: SubtreePathBuilder<'b, B>,
    key: &[u8],
    ref_path: ReferencePathType,
) -> CostResult<ResolvedReference<'db, 'b, 'c, B>, Error> {
    // TODO: this is a new version of follow reference

    check_grovedb_v0_with_cost!(
        "follow_reference",
        merk_cache
            .version
            .grovedb_versions
            .operations
            .get
            .follow_reference
    );

    let mut cost = Default::default();

    let mut hops_left = MAX_REFERENCE_HOPS;
    let mut visited = HashSet::new();

    let mut qualified_path = path.clone();
    qualified_path.push_segment(key);

    visited.insert(qualified_path);

    let mut current_path = path;
    let mut current_key = key.to_vec();
    let mut current_ref = ref_path;

    while hops_left > 0 {
        let referred_qualified_path = cost_return_on_error_into_no_add!(
            cost,
            current_ref.absolute_qualified_path(current_path, &current_key)
        );

        if !visited.insert(referred_qualified_path.clone()) {
            return Err(Error::CyclicReference).wrap_with_cost(cost);
        }

        let Some((referred_path, referred_key)) = referred_qualified_path.derive_parent_owned()
        else {
            return Err(Error::InvalidCodeExecution("empty reference")).wrap_with_cost(cost);
        };

        let mut referred_merk =
            cost_return_on_error!(&mut cost, merk_cache.get_merk(referred_path.clone()));
        let (element, value_hash) = cost_return_on_error!(
            &mut cost,
            referred_merk
                .for_merk(|m| {
                    Element::get_with_value_hash(m, &referred_key, true, merk_cache.version)
                })
                .map_err(|e| match e {
                    grovedb_merk::error::Error::PathKeyNotFound(s) =>
                        Error::CorruptedReferencePathKeyNotFound(s),
                    e => e.into(),
                })
        );

        match element {
            Element::Reference(ref_path, ..) => {
                current_path = referred_path;
                current_key = referred_key;
                current_ref = ref_path;
                hops_left -= 1;
            }
            e => {
                return Ok(ResolvedReference {
                    target_merk: referred_merk,
                    target_path: referred_path,
                    target_key: referred_key,
                    target_element: e,
                    target_node_value_hash: value_hash,
                })
                .wrap_with_cost(cost)
            }
        }
    }

    Err(Error::ReferenceLimit).wrap_with_cost(cost)
}

/// Follow references stopping at the immediate element without following
/// further.
pub(crate) fn follow_reference_once<'db, 'b, 'c, B: AsRef<[u8]>>(
    merk_cache: &'c MerkCache<'db, 'b, B>,
    path: SubtreePathBuilder<'b, B>,
    key: &[u8],
    ref_path: ReferencePathType,
) -> CostResult<ResolvedReference<'db, 'b, 'c, B>, Error> {
    check_grovedb_v0_with_cost!(
        "follow_reference_once",
        merk_cache
            .version
            .grovedb_versions
            .operations
            .get
            .follow_reference_once
    );

    let mut cost = Default::default();

    let referred_qualified_path = cost_return_on_error_into_no_add!(
        cost,
        ref_path.absolute_qualified_path(path.clone(), key)
    );

    let Some((referred_path, referred_key)) = referred_qualified_path.derive_parent_owned() else {
        return Err(Error::InvalidCodeExecution("empty reference")).wrap_with_cost(cost);
    };

    if path == referred_path && key == referred_key {
        return Err(Error::CyclicReference).wrap_with_cost(cost);
    }

    let mut referred_merk =
        cost_return_on_error!(&mut cost, merk_cache.get_merk(referred_path.clone()));
    let (element, value_hash) = cost_return_on_error!(
        &mut cost,
        referred_merk
            .for_merk(|m| {
                Element::get_with_value_hash(m, &referred_key, true, merk_cache.version)
            })
            .map_err(|e| match e {
                grovedb_merk::error::Error::PathKeyNotFound(s) =>
                    Error::CorruptedReferencePathKeyNotFound(s),
                e => e.into(),
            })
    );

    Ok(ResolvedReference {
        target_merk: referred_merk,
        target_path: referred_path,
        target_key: referred_key,
        target_element: element,
        target_node_value_hash: value_hash,
    })
    .wrap_with_cost(cost)
}

#[cfg(test)]
mod tests {
    use grovedb_element::{reference_path::ReferencePathType, Element};
    use grovedb_merk::proofs::Query;
    use grovedb_version::version::GroveVersion;

    use crate::{
        tests::{make_deep_tree, TEST_LEAF},
        GroveDb, PathQuery,
    };

    #[test]
    fn test_query_many_with_different_reference_types() {
        let grove_version = GroveVersion::latest();
        let db = make_deep_tree(grove_version);

        db.insert(
            [TEST_LEAF, b"innertree4"].as_ref(),
            b"ref1",
            Element::new_reference(ReferencePathType::AbsolutePathReference(vec![
                TEST_LEAF.to_vec(),
                b"innertree".to_vec(),
                b"key1".to_vec(),
            ])),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert successfully");

        db.insert(
            [TEST_LEAF, b"innertree4"].as_ref(),
            b"ref2",
            Element::new_reference(ReferencePathType::UpstreamRootHeightReference(
                1,
                vec![b"innertree".to_vec(), b"key1".to_vec()],
            )),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert successfully");

        db.insert(
            [TEST_LEAF, b"innertree4"].as_ref(),
            b"ref3",
            Element::new_reference(ReferencePathType::UpstreamFromElementHeightReference(
                1,
                vec![b"innertree".to_vec(), b"key1".to_vec()],
            )),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert successfully");

        // Query all the elements in Test Leaf
        let mut query = Query::new();
        query.insert_all();
        let path_query =
            PathQuery::new_unsized(vec![TEST_LEAF.to_vec(), b"innertree4".to_vec()], query);
        let result = db
            .query_item_value(&path_query, true, true, true, None, grove_version)
            .unwrap()
            .expect("should query items");
        assert_eq!(result.0.len(), 5);
        assert_eq!(
            result.0,
            vec![
                b"value4".to_vec(),
                b"value5".to_vec(),
                b"value1".to_vec(),
                b"value1".to_vec(),
                b"value1".to_vec()
            ]
        );

        let proof = db
            .prove_query(&path_query, None, grove_version)
            .unwrap()
            .expect("should generate proof");
        let (hash, result) = GroveDb::verify_query_raw(&proof, &path_query, grove_version)
            .expect("should verify proof");
        assert_eq!(hash, db.root_hash(None, grove_version).unwrap().unwrap());
        assert_eq!(result.len(), 5);
    }
}
