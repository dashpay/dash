use grovedb::{operations::insert::InsertOptions, Element, GroveDb, PathQuery, Query};
use grovedb_version::version::GroveVersion;

const KEY1: &[u8] = b"key1";
const KEY2: &[u8] = b"key2";

// Allow insertions to overwrite trees
// This is necessary so the tutorial can be rerun easily
const INSERT_OPTIONS: Option<InsertOptions> = Some(InsertOptions {
    validate_insertion_does_not_override: false,
    validate_insertion_does_not_override_tree: false,
    base_root_storage_is_free: true,
});

fn main() {
    // Specify the path where the GroveDB instance exists.
    let path = String::from("../tutorial-storage");

    let grove_version = GroveVersion::latest();

    // Open GroveDB at the path.
    let db = GroveDb::open(path).unwrap();

    // Populate GroveDB with values. This function is defined below.
    populate(&db);

    // Define the path to the subtree we want to query.
    let path = vec![KEY1.to_vec(), KEY2.to_vec()];

    // Instantiate a new query.
    let mut query = Query::new();

    // Insert a range of keys to the query that we would like returned.
    // In this case, we are asking for keys 30 through 34.
    query.insert_range(30_u8.to_be_bytes().to_vec()..35_u8.to_be_bytes().to_vec());

    // Put the query into a new unsized path query.
    let path_query = PathQuery::new_unsized(path, query.clone());

    // Execute the query and collect the result items in "elements".
    let (elements, _) = db
        .query_item_value(&path_query, true, false, true,None, &grove_version)
        .unwrap()
        .expect("expected successful get_path_query");

    // Print result items to terminal.
    println!("{:?}", elements);
}

fn populate(db: &GroveDb) {
    let root_path: &[&[u8]] = &[];

    let grove_version = GroveVersion::latest();

    // Put an empty subtree into the root tree nodes at KEY1.
    // Call this SUBTREE1.
    db.insert(root_path, KEY1, Element::empty_tree(), INSERT_OPTIONS, None, grove_version)
        .unwrap()
        .expect("successful SUBTREE1 insert");

    // Put an empty subtree into subtree1 at KEY2.
    // Call this SUBTREE2.
    db.insert(&[KEY1], KEY2, Element::empty_tree(), INSERT_OPTIONS, None, grove_version)
        .unwrap()
        .expect("successful SUBTREE2 insert");

    // Populate SUBTREE2 with values 0 through 99 under keys 0 through 99.
    for i in 0u8..100 {
        let i_vec = (i as u8).to_be_bytes().to_vec();
        db.insert(
            &[KEY1, KEY2],
            &i_vec,
            Element::new_item(i_vec.clone()),
            INSERT_OPTIONS,
            None,
            grove_version,
        )
        .unwrap()
        .expect("successfully inserted values");
    }
}
