use grovedb::{
    operations::insert::InsertOptions, Element, GroveDb, PathQuery, Query, QueryItem, SizedQuery,
};
use rand::Rng;
use grovedb_version::version::GroveVersion;

const KEY1: &[u8] = b"key1";
const KEY2: &[u8] = b"key2";
const KEY3: &[u8] = b"key3";

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

    // Define the path to the highest-level subtree we want to query.
    let path = vec![KEY1.to_vec(), KEY2.to_vec()];

    // Instantiate new queries.
    let mut query = Query::new();
    let mut subquery = Query::new();
    let mut subquery2 = Query::new();

    // Insert query items into the queries.
    // Query 20-30 at path.
    query.insert_range(20_u8.to_be_bytes().to_vec()..31_u8.to_be_bytes().to_vec());
    // If any 20-30 are subtrees and meet the subquery condition,
    // follow the path and query 60, 70 from there.
    subquery.insert_keys(vec![vec![60], vec![70]]);
    // If either 60, 70 are subtrees and meet the subquery condition,
    // follow the path and query 90-94 from there.
    subquery2.insert_range(90_u8.to_be_bytes().to_vec()..95_u8.to_be_bytes().to_vec());

    // Add subquery branches.
    // If 60 is a subtree, navigate through SUBTREE4 and run subquery2 on SUBTREE5.
    subquery.add_conditional_subquery(
        QueryItem::Key(vec![60]),
        Some(vec![KEY3.to_vec()]),
        Some(subquery2),
    );
    // If anything up to and including 25 is a subtree, run subquery on it. No path.
    query.add_conditional_subquery(
        QueryItem::RangeToInclusive(std::ops::RangeToInclusive { end: vec![25] }),
        None,
        Some(subquery),
    );

    // Put the query into a sized query. Limit the result set to 10,
    // and impose an offset of 3.
    let sized_query = SizedQuery::new(query, Some(10), Some(3));

    // Put the sized query into a new path query.
    let path_query = PathQuery::new(path, sized_query.clone());

    // Execute the path query and collect the result items in "elements".
    let (elements, _) = db
        .query_item_value(&path_query, true, false, true, None, grove_version)
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

    // Populate SUBTREE2 with values 0 through 49 under keys 0 through 49.
    for i in 0u8..50 {
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
        .expect("successfully inserted values in SUBTREE2");
    }

    // Set random_numbers
    let mut rng = rand::thread_rng();
    let rn1: &[u8] = &(rng.gen_range(15..26) as u8).to_be_bytes();
    let rn2: &[u8] = &(rng.gen_range(60..62) as u8).to_be_bytes();

    // Overwrite key rn1 with a subtree
    // Call this SUBTREE3
    db.insert(
        &[KEY1, KEY2],
        &rn1,
        Element::empty_tree(),
        INSERT_OPTIONS,
        None,
        grove_version,
    )
    .unwrap()
    .expect("successful SUBTREE3 insert");

    // Populate SUBTREE3 with values 50 through 74 under keys 50 through 74
    for i in 50u8..75 {
        let i_vec = (i as u8).to_be_bytes().to_vec();
        db.insert(
            &[KEY1, KEY2, rn1],
            &i_vec,
            Element::new_item(i_vec.clone()),
            INSERT_OPTIONS,
            None,
            grove_version,
        )
        .unwrap()
        .expect("successfully inserted values in SUBTREE3");
    }

    // Overwrite key rn2 with a subtree
    // Call this SUBTREE4
    db.insert(
        &[KEY1, KEY2, rn1],
        &rn2,
        Element::empty_tree(),
        INSERT_OPTIONS,
        None,
        grove_version,
    )
    .unwrap()
    .expect("successful SUBTREE4 insert");

    // Put an empty subtree into SUBTREE4 at KEY3.
    // Call this SUBTREE5.
    db.insert(
        &[KEY1, KEY2, rn1, rn2],
        KEY3,
        Element::empty_tree(),
        INSERT_OPTIONS,
        None,
        grove_version,
    )
    .unwrap()
    .expect("successful SUBTREE5 insert");

    // Populate SUBTREE5 with values 75 through 99 under keys 75 through 99
    for i in 75u8..99 {
        let i_vec = (i as u8).to_be_bytes().to_vec();
        db.insert(
            &[KEY1, KEY2, rn1, rn2, KEY3],
            &i_vec,
            Element::new_item(i_vec.clone()),
            INSERT_OPTIONS,
            None,
            grove_version,
        )
        .unwrap()
        .expect("successfully inserted values in SUBTREE5");
    }
}
