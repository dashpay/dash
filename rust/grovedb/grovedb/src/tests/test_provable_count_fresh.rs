use grovedb_merk::proofs::Query;
use grovedb_version::version::GroveVersion;

use crate::{
    tests::{make_test_grovedb, TempGroveDb},
    Element, Error, GroveDb, PathQuery,
};

#[test]
fn test_provable_count_tree_fresh_proof() {
    let grove_version = GroveVersion::latest();
    let db = make_test_grovedb(grove_version);

    // Create a ProvableCountTree
    db.insert(
        &[] as &[&[u8]],
        b"test_tree",
        Element::empty_provable_count_tree(),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("should insert tree");

    // Insert an item
    db.insert(
        &[b"test_tree"],
        b"key1",
        Element::new_item(b"value1".to_vec()),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("should insert item");

    // Query for the item
    let mut query = Query::new();
    query.insert_key(b"key1".to_vec());
    let path_query = PathQuery::new_unsized(vec![b"test_tree".to_vec()], query);

    // Generate proof
    let proof = db
        .prove_query(&path_query, None, grove_version)
        .unwrap()
        .expect("should generate proof");

    // Verify proof
    let (root_hash, proved_values) =
        GroveDb::verify_query_raw(&proof, &path_query, grove_version).expect("should verify proof");

    // Check root hash matches
    let actual_root_hash = db
        .root_hash(None, grove_version)
        .unwrap()
        .expect("should get root hash");

    assert_eq!(root_hash, actual_root_hash, "Root hash should match");
    assert_eq!(proved_values.len(), 1, "Should have 1 proved value");
    assert_eq!(proved_values[0].key, b"key1");
    // The value might be wrapped in an Element serialization
    println!("Proved value: {:?}", proved_values[0].value);
}
