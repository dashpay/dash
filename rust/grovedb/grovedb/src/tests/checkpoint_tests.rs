mod tests {
    use grovedb_element::Element;
    use grovedb_version::version::GroveVersion;
    use tempfile::TempDir;

    use crate::{
        tests::{common::EMPTY_PATH, make_test_grovedb},
        Error, GroveDb,
    };

    #[test]
    fn test_checkpoint() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);
        let element1 = Element::new_item(b"ayy".to_vec());

        db.insert(
            EMPTY_PATH,
            b"key1",
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("cannot insert a subtree 1 into GroveDB");
        db.insert(
            [b"key1".as_ref()].as_ref(),
            b"key2",
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("cannot insert a subtree 2 into GroveDB");
        db.insert(
            [b"key1".as_ref(), b"key2".as_ref()].as_ref(),
            b"key3",
            element1.clone(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("cannot insert an item into GroveDB");

        assert_eq!(
            db.get(
                [b"key1".as_ref(), b"key2".as_ref()].as_ref(),
                b"key3",
                None,
                grove_version
            )
            .unwrap()
            .expect("cannot get from grovedb"),
            element1
        );

        let tempdir_parent = TempDir::new().expect("cannot open tempdir");
        let checkpoint_tempdir = tempdir_parent.path().join("checkpoint");
        db.create_checkpoint(&checkpoint_tempdir)
            .expect("cannot create checkpoint");

        let checkpoint_db = GroveDb::open_checkpoint(checkpoint_tempdir)
            .expect("cannot open grovedb from checkpoint");

        assert_eq!(
            db.get(
                [b"key1".as_ref(), b"key2".as_ref()].as_ref(),
                b"key3",
                None,
                grove_version
            )
            .unwrap()
            .expect("cannot get from grovedb"),
            element1
        );
        assert_eq!(
            checkpoint_db
                .get(
                    [b"key1".as_ref(), b"key2".as_ref()].as_ref(),
                    b"key3",
                    None,
                    grove_version
                )
                .unwrap()
                .expect("cannot get from checkpoint"),
            element1
        );

        let element2 = Element::new_item(b"ayy2".to_vec());
        let element3 = Element::new_item(b"ayy3".to_vec());

        checkpoint_db
            .insert(
                [b"key1".as_ref()].as_ref(),
                b"key4",
                element2.clone(),
                None,
                None,
                grove_version,
            )
            .unwrap()
            .expect("cannot insert into checkpoint");

        db.insert(
            [b"key1".as_ref()].as_ref(),
            b"key4",
            element3.clone(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("cannot insert into GroveDB");

        assert_eq!(
            checkpoint_db
                .get([b"key1".as_ref()].as_ref(), b"key4", None, grove_version)
                .unwrap()
                .expect("cannot get from checkpoint"),
            element2,
        );

        assert_eq!(
            db.get([b"key1".as_ref()].as_ref(), b"key4", None, grove_version)
                .unwrap()
                .expect("cannot get from GroveDB"),
            element3
        );

        checkpoint_db
            .insert(
                [b"key1".as_ref()].as_ref(),
                b"key5",
                element3.clone(),
                None,
                None,
                grove_version,
            )
            .unwrap()
            .expect("cannot insert into checkpoint");

        db.insert(
            [b"key1".as_ref()].as_ref(),
            b"key6",
            element3,
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("cannot insert into GroveDB");

        assert!(matches!(
            checkpoint_db
                .get([b"key1".as_ref()].as_ref(), b"key6", None, grove_version)
                .unwrap(),
            Err(Error::PathKeyNotFound(_))
        ));

        assert!(matches!(
            db.get([b"key1".as_ref()].as_ref(), b"key5", None, grove_version)
                .unwrap(),
            Err(Error::PathKeyNotFound(_))
        ));
    }

    #[test]
    fn test_delete_checkpoint() {
        let grove_version = GroveVersion::latest();
        let db = make_test_grovedb(grove_version);

        // Insert some data
        db.insert(
            EMPTY_PATH,
            b"key1",
            Element::new_item(b"value1".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("cannot insert into GroveDB");

        // Create a checkpoint
        let tempdir_parent = TempDir::new().expect("cannot open tempdir");
        let checkpoint_path = tempdir_parent.path().join("checkpoint");
        db.create_checkpoint(&checkpoint_path)
            .expect("cannot create checkpoint");

        // Verify checkpoint directory exists
        assert!(
            checkpoint_path.exists(),
            "checkpoint should exist after creation"
        );

        // Verify checkpoint can be opened and data is accessible
        {
            let checkpoint_db =
                GroveDb::open_checkpoint(&checkpoint_path).expect("cannot open checkpoint");
            let result = checkpoint_db
                .get(EMPTY_PATH, b"key1", None, grove_version)
                .unwrap()
                .expect("cannot get from checkpoint");
            assert_eq!(result, Element::new_item(b"value1".to_vec()));
        } // checkpoint_db is dropped here

        // Delete the checkpoint
        GroveDb::delete_checkpoint(&checkpoint_path).expect("cannot delete checkpoint");

        // Verify checkpoint directory no longer exists
        assert!(
            !checkpoint_path.exists(),
            "checkpoint should not exist after deletion"
        );

        // Verify original database is unaffected
        let result = db
            .get(EMPTY_PATH, b"key1", None, grove_version)
            .unwrap()
            .expect("cannot get from original db");
        assert_eq!(result, Element::new_item(b"value1".to_vec()));
    }

    /// Test that checkpoint with WAL under 50MB threshold preserves the WAL
    /// file. When WAL is under the threshold, RocksDB skips flushing and
    /// copies the WAL to the checkpoint, which will be replayed on open.
    #[test]
    fn test_checkpoint_wal_under_threshold_is_preserved() {
        use std::fs;

        let grove_version = GroveVersion::latest();
        let tmp_dir = TempDir::new().expect("cannot open tempdir");
        let db = GroveDb::open(tmp_dir.path()).expect("cannot open grovedb");

        // Insert a tree to hold our data
        db.insert(
            EMPTY_PATH,
            b"data",
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("cannot insert tree");

        // Write approximately 49MB of data (under 50MB threshold)
        // Using 100KB values, need ~490 items
        let value_size = 100 * 1024; // 100KB
        let num_items = 490;
        let large_value = vec![0xABu8; value_size];

        for i in 0u32..num_items {
            let key = format!("key_{:06}", i);
            db.insert(
                [b"data".as_ref()].as_ref(),
                key.as_bytes(),
                Element::new_item(large_value.clone()),
                None,
                None,
                grove_version,
            )
            .unwrap()
            .expect("cannot insert item");
        }

        // Create checkpoint
        let checkpoint_path = tmp_dir.path().join("checkpoint");
        db.create_checkpoint(&checkpoint_path)
            .expect("cannot create checkpoint");

        // Find WAL files in the checkpoint directory
        let wal_files: Vec<_> = fs::read_dir(&checkpoint_path)
            .expect("cannot read checkpoint dir")
            .filter_map(|entry| entry.ok())
            .filter(|entry| entry.path().extension().is_some_and(|ext| ext == "log"))
            .collect();

        // Should have at least one WAL file
        assert!(
            !wal_files.is_empty(),
            "Checkpoint should contain WAL file(s) when under threshold"
        );

        // WAL file should be non-empty (data was not flushed)
        let total_wal_size: u64 = wal_files.iter().map(|f| f.metadata().unwrap().len()).sum();
        assert!(
            total_wal_size > 0,
            "WAL file(s) should be non-empty when under 50MB threshold, got {} bytes",
            total_wal_size
        );

        // Verify checkpoint can be opened and data is accessible
        let checkpoint_db =
            GroveDb::open_checkpoint(&checkpoint_path).expect("cannot open checkpoint");
        let result = checkpoint_db
            .get(
                [b"data".as_ref()].as_ref(),
                b"key_000000",
                None,
                grove_version,
            )
            .unwrap()
            .expect("cannot get from checkpoint");
        assert_eq!(result, Element::new_item(large_value));
    }

    /// Test that checkpoint with WAL over 50MB threshold triggers a flush.
    /// When WAL exceeds the threshold at checkpoint creation time, RocksDB
    /// flushes memtables, resulting in an empty WAL in the checkpoint.
    ///
    /// Note: This test carefully writes data to get WAL just over 50 MiB
    /// (52,428,800 bytes) but under the 64MB memtable auto-flush limit.
    ///
    /// IGNORED: This test is currently ignored due to a bug in RocksDB where
    /// the `log_size_for_flush` parameter in `CreateCheckpoint()` is
    /// non-functional. The bug is in `WalManager::GetSortedWalFiles()` - it
    /// fails to return live WAL files, causing WAL size to always be
    /// calculated as 0, which means the threshold comparison never triggers
    /// a flush.
    ///
    /// Re-enable this test when <https://github.com/facebook/rocksdb/pull/14193>
    /// is merged and released.
    #[test]
    #[ignore]
    fn test_checkpoint_wal_over_threshold_is_flushed() {
        use std::fs;

        let grove_version = GroveVersion::latest();
        let tmp_dir = TempDir::new().expect("cannot open tempdir");
        let db = GroveDb::open(tmp_dir.path()).expect("cannot open grovedb");

        // Insert a tree to hold our data
        db.insert(
            EMPTY_PATH,
            b"data",
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("cannot insert tree");

        // Helper to get current WAL size
        let get_wal_size = |path: &std::path::Path| -> u64 {
            fs::read_dir(path)
                .expect("cannot read dir")
                .filter_map(|entry| entry.ok())
                .filter(|entry| entry.path().extension().is_some_and(|ext| ext == "log"))
                .map(|entry| entry.metadata().unwrap().len())
                .sum()
        };

        // 50 MiB threshold in bytes
        let threshold: u64 = 50 * 1024 * 1024;

        // Write data until WAL exceeds threshold (but stay under 64MB memtable limit)
        let value_size = 100 * 1024; // 100KB
        let large_value = vec![0xEFu8; value_size];
        let mut item_count = 0u32;

        loop {
            let key = format!("key_{:06}", item_count);
            db.insert(
                [b"data".as_ref()].as_ref(),
                key.as_bytes(),
                Element::new_item(large_value.clone()),
                None,
                None,
                grove_version,
            )
            .unwrap()
            .expect("cannot insert item");

            item_count += 1;
            let current_wal_size = get_wal_size(tmp_dir.path());

            // Stop when WAL exceeds threshold
            if current_wal_size > threshold {
                // List all WAL files for debugging
                let wal_files: Vec<_> = fs::read_dir(tmp_dir.path())
                    .expect("cannot read dir")
                    .filter_map(|entry| entry.ok())
                    .filter(|entry| entry.path().extension().is_some_and(|ext| ext == "log"))
                    .collect();
                println!(
                    "WAL exceeded threshold after {} items: {} bytes ({:.2} MB), threshold = {} \
                     bytes ({:.2} MB)",
                    item_count,
                    current_wal_size,
                    current_wal_size as f64 / (1024.0 * 1024.0),
                    threshold,
                    threshold as f64 / (1024.0 * 1024.0)
                );
                println!(
                    "WAL files: {:?}",
                    wal_files
                        .iter()
                        .map(|f| {
                            let path = f.path();
                            let size = f.metadata().unwrap().len();
                            (
                                path.file_name().unwrap().to_string_lossy().to_string(),
                                size,
                            )
                        })
                        .collect::<Vec<_>>()
                );
                break;
            }

            // Safety: don't exceed 64MB to avoid memtable auto-flush
            if current_wal_size > 62 * 1024 * 1024 {
                panic!(
                    "WAL reached 62MB without exceeding 50 MiB threshold - test assumptions \
                     invalid"
                );
            }
        }

        // Verify WAL is over threshold before checkpoint
        let pre_checkpoint_wal = get_wal_size(tmp_dir.path());
        assert!(
            pre_checkpoint_wal > threshold,
            "WAL should be over threshold before checkpoint: {} bytes",
            pre_checkpoint_wal
        );

        // Create checkpoint - this should trigger a flush
        let checkpoint_path = tmp_dir.path().join("checkpoint");
        db.create_checkpoint(&checkpoint_path)
            .expect("cannot create checkpoint");

        // Find WAL files in the checkpoint directory
        let checkpoint_wal_files: Vec<_> = fs::read_dir(&checkpoint_path)
            .expect("cannot read checkpoint dir")
            .filter_map(|entry| entry.ok())
            .filter(|entry| entry.path().extension().is_some_and(|ext| ext == "log"))
            .collect();
        let checkpoint_wal_size: u64 = checkpoint_wal_files
            .iter()
            .map(|f| f.metadata().unwrap().len())
            .sum();

        println!(
            "Checkpoint WAL size: {} bytes ({:.2} MB)",
            checkpoint_wal_size,
            checkpoint_wal_size as f64 / (1024.0 * 1024.0)
        );
        println!(
            "Checkpoint WAL files: {:?}",
            checkpoint_wal_files
                .iter()
                .map(|f| {
                    let path = f.path();
                    let size = f.metadata().unwrap().len();
                    (
                        path.file_name().unwrap().to_string_lossy().to_string(),
                        size,
                    )
                })
                .collect::<Vec<_>>()
        );

        // WAL in checkpoint should be empty (flush was triggered)
        assert_eq!(
            checkpoint_wal_size, 0,
            "WAL should be empty in checkpoint when over 50MB threshold (flush triggered), got {} \
             bytes",
            checkpoint_wal_size
        );

        // Verify checkpoint can be opened and data is accessible (from SST files)
        let checkpoint_db =
            GroveDb::open_checkpoint(&checkpoint_path).expect("cannot open checkpoint");
        let result = checkpoint_db
            .get(
                [b"data".as_ref()].as_ref(),
                b"key_000000",
                None,
                grove_version,
            )
            .unwrap()
            .expect("cannot get from checkpoint");
        assert_eq!(result, Element::new_item(large_value.clone()));

        // Also verify last item is accessible
        let last_key = format!("key_{:06}", item_count - 1);
        let result = checkpoint_db
            .get(
                [b"data".as_ref()].as_ref(),
                last_key.as_bytes(),
                None,
                grove_version,
            )
            .unwrap()
            .expect("cannot get last item from checkpoint");
        assert_eq!(result, Element::new_item(large_value));
    }
}
