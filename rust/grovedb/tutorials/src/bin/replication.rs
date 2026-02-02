use std::collections::VecDeque;
use std::path::Path;
use std::time::Instant;
use grovedb::{operations::insert::InsertOptions, Element, GroveDb, PathQuery, Query, Transaction};
use grovedb::reference_path::ReferencePathType;
use rand::{distributions::Alphanumeric, Rng, };
use grovedb::element::SumValue;
use grovedb::replication::CURRENT_STATE_SYNC_VERSION;
use grovedb_version::version::GroveVersion;

const MAIN_KEY: &[u8] = b"key_main";
const MAIN_KEY_EMPTY: &[u8] = b"key_main_empty";

const KEY_INT_0: &[u8] = b"key_int_0";
const KEY_INT_1: &[u8] = b"key_int_1";
const KEY_INT_2: &[u8] = b"key_int_2";
const KEY_INT_REF_0: &[u8] = b"key_int_ref_0";
const KEY_INT_A: &[u8] = b"key_sum_0";

const KEY_INT_COUNT: &[u8] = b"key_count_0";
const ROOT_PATH: &[&[u8]] = &[];

// Allow insertions to overwrite trees
// This is necessary so the tutorial can be rerun easily
const INSERT_OPTIONS: Option<InsertOptions> = Some(InsertOptions {
    validate_insertion_does_not_override: false,
    validate_insertion_does_not_override_tree: false,
    base_root_storage_is_free: true,
});

fn populate_db(grovedb_path: String, grove_version: &GroveVersion) -> GroveDb {
    let db = GroveDb::open(grovedb_path).unwrap();

    insert_empty_tree_db(&db, ROOT_PATH, MAIN_KEY, grove_version);
    insert_empty_tree_db(&db, ROOT_PATH, MAIN_KEY_EMPTY, grove_version);
    insert_empty_tree_db(&db, &[MAIN_KEY], KEY_INT_0, grove_version);
    insert_empty_tree_db(&db, &[MAIN_KEY], KEY_INT_1, grove_version);
    insert_empty_tree_db(&db, &[MAIN_KEY], KEY_INT_2, grove_version);

    let tx = db.start_transaction();
    let batch_size = 50;
    for i in 0..=100 {
        insert_range_values_db(&db, &[MAIN_KEY, KEY_INT_0], i * batch_size, i * batch_size + batch_size - 1, &tx, grove_version);
    }
    let _ = db.commit_transaction(tx);

    let tx = db.start_transaction();
    let batch_size = 50;
    for i in 0..=100 {
        insert_range_values_db(&db, &[MAIN_KEY, KEY_INT_1], i * batch_size, i * batch_size + batch_size - 1, &tx, grove_version);
    }
    let _ = db.commit_transaction(tx);

    let tx = db.start_transaction();
    let batch_size = 50;
    for i in 0..=5 {
        insert_range_values_db(&db, &[MAIN_KEY, KEY_INT_2], i * batch_size, i * batch_size + batch_size - 1, &tx, grove_version);
    }
    let _ = db.commit_transaction(tx);

    insert_empty_tree_db(&db, &[MAIN_KEY], KEY_INT_REF_0, grove_version);

    let tx_2 = db.start_transaction();
    insert_range_ref_double_values_db(&db, &[MAIN_KEY, KEY_INT_REF_0], KEY_INT_0, 1, 50, &tx_2, grove_version);
    let _ = db.commit_transaction(tx_2);

    insert_empty_sum_tree_db(&db, &[MAIN_KEY], KEY_INT_A, grove_version);

    let tx_3 = db.start_transaction();
    insert_range_values_db(&db, &[MAIN_KEY, KEY_INT_A], 1, 500, &tx_3, grove_version);
    insert_sum_element_db(&db, &[MAIN_KEY, KEY_INT_A], 501, 550, &tx_3, grove_version);
    let _ = db.commit_transaction(tx_3);

    insert_empty_count_tree_db(&db, &[MAIN_KEY], KEY_INT_COUNT, grove_version);

    let tx_4 = db.start_transaction();
    insert_range_values_db(&db, &[MAIN_KEY, KEY_INT_COUNT], 1, 50, &tx_4, grove_version);
    let _ = db.commit_transaction(tx_4);

    db
}

fn create_empty_db(grovedb_path: String) -> GroveDb   {
    GroveDb::open(grovedb_path).unwrap()
}

fn main() {
    let grove_version = GroveVersion::latest();
    let path_source = generate_random_path("../tutorial-storage/", "/db_0", 24);
    let db_source = populate_db(path_source.clone(), grove_version);

    let checkpoint_dir = path_source + "/checkpoint";
    let path_checkpoint = Path::new(checkpoint_dir.as_str());

    db_source.create_checkpoint(path_checkpoint).expect("cannot create checkpoint");
    let db_checkpoint_0 = GroveDb::open(path_checkpoint).expect("cannot open groveDB from checkpoint");

    let path_destination = generate_random_path("../tutorial-storage/", "/db_copy", 24);
    let db_destination = create_empty_db(path_destination.clone());

    println!("\n######### root_hashes:");
    let root_hash_source = db_source.root_hash(None, grove_version).unwrap().unwrap();
    println!("root_hash_source: {:?}", hex::encode(root_hash_source));
    let root_hash_checkpoint_0 = db_checkpoint_0.root_hash(None, grove_version).unwrap().unwrap();
    println!("root_hash_checkpoint_0: {:?}", hex::encode(root_hash_checkpoint_0));
    let root_hash_destination = db_destination.root_hash(None, grove_version).unwrap().unwrap();
    println!("root_hash_destination: {:?}", hex::encode(root_hash_destination));

    println!("\n######### db_checkpoint_0 -> db_destination state sync");
    sync_db_demo(&db_checkpoint_0, &db_destination, grove_version).unwrap();

    println!("\n######### verify db_destination");
    let incorrect_hashes = db_destination.verify_grovedb(None, true, false, grove_version).unwrap();
    if !incorrect_hashes.is_empty() {
        println!("DB verification failed!");
    }
    else {
        println!("DB verification success");
    }

    println!("\n######### root_hashes:");
    let root_hash_source = db_source.root_hash(None, grove_version).unwrap().unwrap();
    println!("root_hash_source: {:?}", hex::encode(root_hash_source));
    let root_hash_checkpoint_0 = db_checkpoint_0.root_hash(None, grove_version).unwrap().unwrap();
    println!("root_hash_checkpoint_0: {:?}", hex::encode(root_hash_checkpoint_0));
    let root_hash_destination = db_destination.root_hash(None, grove_version).unwrap().unwrap();
    println!("root_hash_destination: {:?}", hex::encode(root_hash_destination));

    let query_path = &[MAIN_KEY, KEY_INT_0];
    let query_key = 20487u32.to_be_bytes().to_vec();
    println!("\n######## Query on db_checkpoint_0:");
    query_db(&db_checkpoint_0, query_path, query_key.clone(), grove_version);
    println!("\n######## Query on db_destination:");
    query_db(&db_destination, query_path, query_key.clone(), grove_version);

    let query_path = &[MAIN_KEY, KEY_INT_COUNT];
    let query_key = 40u32.to_be_bytes().to_vec();
    println!("\n######## Query on db_checkpoint_0:");
    query_db(&db_checkpoint_0, query_path, query_key.clone(), grove_version);
    println!("\n######## Query on db_destination:");
    query_db(&db_destination, query_path, query_key.clone(), grove_version);
}

fn insert_empty_tree_db(db: &GroveDb, path: &[&[u8]], key: &[u8], grove_version: &GroveVersion)
{
    db.insert(path, key, Element::empty_tree(), INSERT_OPTIONS, None, grove_version)
        .unwrap()
        .expect("successfully inserted tree");
}
fn insert_range_values_db(db: &GroveDb, path: &[&[u8]], min_i: u32, max_i: u32, transaction: &Transaction, grove_version: &GroveVersion)
{
    for i in min_i..=max_i {
        let i_vec = i.to_be_bytes().to_vec();
        db.insert(
            path,
            &i_vec,
            Element::new_item(i_vec.to_vec()),
            INSERT_OPTIONS,
            Some(transaction),
            grove_version,
        )
            .unwrap()
            .expect("successfully inserted values");
    }
}

fn insert_range_ref_double_values_db(db: &GroveDb, path: &[&[u8]], ref_key: &[u8], min_i: u32, max_i: u32, transaction: &Transaction, grove_version: &GroveVersion)
{
    for i in min_i..=max_i {
        let i_vec = i.to_be_bytes().to_vec();
        let value = i * 2;
        let value_vec = value.to_be_bytes().to_vec();
        db.insert(
            path,
            &i_vec,
            Element::new_reference(ReferencePathType::AbsolutePathReference(vec![
                MAIN_KEY.to_vec(),
                ref_key.to_vec(),
                value_vec.to_vec()
            ])),
            INSERT_OPTIONS,
            Some(transaction),
            grove_version,
        )
            .unwrap()
            .expect("successfully inserted values");
    }
}

fn insert_empty_sum_tree_db(db: &GroveDb, path: &[&[u8]], key: &[u8], grove_version: &GroveVersion)
{
    db.insert(path, key, Element::empty_sum_tree(), INSERT_OPTIONS, None, grove_version)
        .unwrap()
        .expect("successfully inserted tree");
}
fn insert_sum_element_db(db: &GroveDb, path: &[&[u8]], min_i: u32, max_i: u32, transaction: &Transaction, grove_version: &GroveVersion)
{
    for i in min_i..=max_i {
        //let value : u32 = i;
        let value = i as u64;
        //let value: u64 = 1;
        let i_vec = i.to_be_bytes().to_vec();
        db.insert(
            path,
            &i_vec,
            Element::new_sum_item(value as SumValue),
            INSERT_OPTIONS,
            Some(transaction),
            grove_version,
        )
            .unwrap()
            .expect("successfully inserted values");
    }
}

fn insert_empty_count_tree_db(db: &GroveDb, path: &[&[u8]], key: &[u8], grove_version: &GroveVersion)
{
    db.insert(path, key, Element::empty_count_tree(), INSERT_OPTIONS, None, grove_version)
        .unwrap()
        .expect("successfully inserted tree");
}

fn generate_random_path(prefix: &str, suffix: &str, len: usize) -> String {
    let random_string: String = rand::thread_rng()
        .sample_iter(&Alphanumeric)
        .take(len)
        .map(char::from)
        .collect();
    format!("{}{}{}", prefix, random_string, suffix)
}

fn query_db(db: &GroveDb, path: &[&[u8]], key: Vec<u8>, grove_version: &GroveVersion) {
    let path_vec: Vec<Vec<u8>> = path.iter()
        .map(|&slice| slice.to_vec())
        .collect();

    let mut query = Query::new();
    query.insert_key(key);

   let path_query = PathQuery::new_unsized(path_vec, query.clone());

    let (elements, _) = db
        .query_item_value(&path_query, true, false, true, None, grove_version)
        .unwrap()
        .expect("expected successful get_path_query");
    for e in elements.into_iter() {
        println!(">> {:?}", e);
    }

    let proof = db.prove_query(&path_query, None, grove_version).unwrap().unwrap();
    // Get hash from query proof and print to terminal along with GroveDB root hash.
    let (verify_hash, _) = GroveDb::verify_query(&proof, &path_query, grove_version).unwrap();
    println!("verify_hash: {:?}", hex::encode(verify_hash));
    if verify_hash == db.root_hash(None, grove_version).unwrap().unwrap() {
        println!("Query verified");
    } else { println!("Verification FAILED"); };
}

fn sync_db_demo(
    source_db: &GroveDb,
    target_db: &GroveDb,
    grove_version: &GroveVersion,
) -> Result<(), grovedb::Error> {
    let start_time = Instant::now();
    let app_hash = source_db.root_hash(None, grove_version).value?;
    const SUBTREES_BATCH_SIZE: usize = 2; // Small value for demo purposes
    let mut session = target_db.start_snapshot_syncing(app_hash, SUBTREES_BATCH_SIZE, CURRENT_STATE_SYNC_VERSION, grove_version)?;

    let mut chunk_queue : VecDeque<Vec<u8>> = VecDeque::new();

    // The very first chunk to fetch is always identified by the root app_hash
    chunk_queue.push_back(app_hash.to_vec());

    let mut num_chunks = 0;
    while let Some(chunk_id) = chunk_queue.pop_front() {
        num_chunks += 1;
        let ops = source_db.fetch_chunk(chunk_id.as_slice(), None, CURRENT_STATE_SYNC_VERSION, grove_version)?;
        let more_chunks = session.apply_chunk(chunk_id.as_slice(), &ops, CURRENT_STATE_SYNC_VERSION, grove_version)?;
        chunk_queue.extend(more_chunks);
    }
    println!("num_chunks: {}", num_chunks);

    if session.is_sync_completed() {
        println!("state_sync completed");
        target_db.commit_session(session)?;
    }
    let elapsed = start_time.elapsed();
    println!("state_synced in {:.2?}", elapsed);


    Ok(())
}

