use grovedb::{Element, GroveDb};
use grovedb_version::version::GroveVersion;

fn main() {
    let root_path: &[&[u8]] = &[];
    let grove_version = GroveVersion::latest();

    // Specify a path and open GroveDB at the path as db
    let path = String::from("../tutorial-storage");
    let db = GroveDb::open(path).unwrap();

    // Define key-values for insertion
    let key1 = b"hello";
    let val1 = b"world";
    let key2 = b"grovedb";
    let val2 = b"rocks";

    // Insert key-value 1 into the root tree
    db.insert(
        root_path,
        key1,
        Element::Item(val1.to_vec(), None),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("successful key1 insert");

    // Insert key-value 2 into the root tree
    db.insert(
        root_path,
        key2,
        Element::Item(val2.to_vec(), None),
        None,
        None,
        grove_version,
    )
    .unwrap()
    .expect("successful key2 insert");

    // Check the key-values are there
    let result1 = db.get(root_path, key1, None, grove_version).unwrap();
    let result2 = db.get(root_path, key2, None, grove_version).unwrap();
    println!("Before deleting, we have key1: {:?}", result1);
    println!("Before deleting, we have key2: {:?}", result2);

    // Delete the values
    db.delete(root_path, key1, None, None, grove_version)
        .unwrap()
        .expect("successfully deleted key1");
    db.delete(root_path, key2, None, None, grove_version)
        .unwrap()
        .expect("successfully deleted key2");

    // Check the key-values again
    let result3 = db.get(root_path, key1, None, grove_version).unwrap();
    let result4 = db.get(root_path, key2, None, grove_version).unwrap();
    println!("After deleting, we have key1: {:?}", result3);
    println!("After deleting, we have key2: {:?}", result4);
}
