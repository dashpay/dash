//! Tests for understanding WAL and SST storage overhead.
//!
//! These tests measure the storage overhead of GroveDB operations:
//! - WAL overhead: ~6x before flush (stores all intermediate tree operations)
//! - SST after compaction: ~1x (final state only, with ~3-16% metadata
//!   overhead)

mod tests {
    use std::fs;

    use grovedb_element::Element;
    use grovedb_version::version::GroveVersion;
    use rand::{rngs::StdRng, Rng, SeedableRng};
    use tempfile::TempDir;

    use crate::{tests::common::EMPTY_PATH, GroveDb};

    /// Helper to get total size of files with a given extension in a directory
    fn get_files_size(path: &std::path::Path, extension: &str) -> u64 {
        fs::read_dir(path)
            .expect("cannot read dir")
            .filter_map(|entry| entry.ok())
            .filter(|entry| entry.path().extension().is_some_and(|ext| ext == extension))
            .map(|entry| entry.metadata().unwrap().len())
            .sum()
    }

    /// Test that measures WAL and SST storage overhead with random data.
    ///
    /// Key findings:
    /// - RocksDB flushes memtable at ~64MB (default memtable size)
    /// - WAL overhead before flush: ~6x raw data size
    /// - SST after compaction: ~1x raw data size (with ~3-16% metadata
    ///   overhead)
    /// - The 50MB checkpoint threshold only triggers if WAL reaches 50MB
    ///   without hitting the 64MB memtable limit first
    #[test]
    fn test_wal_and_sst_storage_overhead() {
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

        // Write 50MB of random data to observe flush behavior
        let value_size = 100 * 1024; // 100KB
        let num_items: u32 = 500; // 50MB raw data
        let mut rng = StdRng::seed_from_u64(12345);

        let mut prev_wal_size: u64 = 0;
        for i in 0..num_items {
            let key = format!("key_{:06}", i);
            // Generate random data for each item (won't compress well)
            let random_value: Vec<u8> = (0..value_size).map(|_| rng.random()).collect();
            db.insert(
                [b"data".as_ref()].as_ref(),
                key.as_bytes(),
                Element::new_item(random_value),
                None,
                None,
                grove_version,
            )
            .unwrap()
            .expect("cannot insert item");

            let current_wal_size = get_files_size(tmp_dir.path(), "log");

            // Detect flush: WAL size decreased
            if current_wal_size < prev_wal_size {
                let sst_size = get_files_size(tmp_dir.path(), "sst");
                let raw_data_so_far = ((i + 1) as usize * value_size) as f64 / (1024.0 * 1024.0);
                println!(
                    "FLUSH after item {}: WAL {:.2} MB → {:.2} MB, SST total = {:.2} MB, raw data \
                     = {:.2} MB, SST/raw = {:.2}x",
                    i,
                    prev_wal_size as f64 / (1024.0 * 1024.0),
                    current_wal_size as f64 / (1024.0 * 1024.0),
                    sst_size as f64 / (1024.0 * 1024.0),
                    raw_data_so_far,
                    sst_size as f64 / (raw_data_so_far * 1024.0 * 1024.0)
                );
            }

            // Print progress every 50 items
            if i % 50 == 0 {
                let raw_data_mb = ((i + 1) as usize * value_size) as f64 / (1024.0 * 1024.0);
                let wal_mb = current_wal_size as f64 / (1024.0 * 1024.0);
                let overhead = if raw_data_mb > 0.0 {
                    wal_mb / raw_data_mb
                } else {
                    0.0
                };
                println!(
                    "After item {}: raw data = {:.2} MB, WAL = {:.2} MB, overhead = {:.2}x",
                    i, raw_data_mb, wal_mb, overhead
                );
            }

            prev_wal_size = current_wal_size;
        }

        // Final measurements
        let final_wal_size = get_files_size(tmp_dir.path(), "log");
        let final_sst_size = get_files_size(tmp_dir.path(), "sst");
        let total_raw_data = (value_size * num_items as usize) as f64 / (1024.0 * 1024.0);

        println!(
            "\n=== Final Storage Summary ===\nRaw data written: {:.2} MB\nFinal WAL size: {:.2} \
             MB\nFinal SST size: {:.2} MB\nSST/raw ratio: {:.2}x",
            total_raw_data,
            final_wal_size as f64 / (1024.0 * 1024.0),
            final_sst_size as f64 / (1024.0 * 1024.0),
            final_sst_size as f64 / (total_raw_data * 1024.0 * 1024.0)
        );

        // Verify SST is roughly 1x raw data (with some overhead for metadata)
        // Allow up to 1.5x for tree metadata overhead
        let sst_ratio = final_sst_size as f64 / (total_raw_data * 1024.0 * 1024.0);
        assert!(
            sst_ratio < 1.5,
            "SST/raw ratio ({:.2}x) should be under 1.5x",
            sst_ratio
        );

        // Verify data is accessible
        let result = db
            .get(
                [b"data".as_ref()].as_ref(),
                b"key_000000",
                None,
                grove_version,
            )
            .unwrap()
            .expect("cannot get first item");
        if let Element::Item(data, ..) = result {
            assert_eq!(
                data.len(),
                value_size,
                "first item should have correct size"
            );
        } else {
            panic!("expected Item element");
        }
    }
}
