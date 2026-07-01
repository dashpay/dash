//! Comprehensive test for ProvableCountTree with mixed subtree types
//!
//! Tree Structure:
//! ```text
//!                        [root]
//!                           |
//!                    ProvableCountTree("pcount")  [count=19]
//!                           |
//!     +--------+--------+--------+--------+--------+--------+--------+----------+--------+
//!     |        |        |        |        |        |        |        |          |        |
//!   Tree    Tree    SumTree  SumTree  SumTree  CountTree CountTree CountSumTree Items(7)
//!  "tree1" "tree2"  "sum1"   "sum2"   "sum3"   "cnt1"    "cnt2"    "cntsum1"
//!  [cnt=1] [cnt=1]  [cnt=1]  [cnt=1]  [cnt=1]  [cnt=3]   [cnt=1]   [cnt=3]      [cnt=7]
//!     |        |        |        |        |        |        |          |
//!   (empty) Item"x"  SumItem SumItem  (empty)  Item"a"  Item"b"   SumItem"y"
//!                     =100    =200             Item"c"            SumItem"z"
//!                                              Item"d"            SumItem"w"
//!                                                                  =50 =75 =25
//!
//! Items directly in pcount:
//!   - "item1" -> "value1"
//!   - "item2" -> "value2"
//!   - "item3" -> "value3"
//!   - "item4" -> "value4"
//!   - "item5" -> "value5"
//!   - "item6" -> "value6"
//!   - "item7" -> "value7"
//!
//! Count calculation for "pcount": 19
//!   - tree1: 1 (Tree counts as 1, contents don't propagate)
//!   - tree2: 1 (Tree counts as 1, contents don't propagate)
//!   - sum1: 1 (SumTree counts as 1, contents don't propagate)
//!   - sum2: 1 (SumTree counts as 1, contents don't propagate)
//!   - sum3: 1 (SumTree counts as 1, contents don't propagate)
//!   - cnt1: 3 (CountTree propagates its count: 3 items)
//!   - cnt2: 1 (CountTree propagates its count: 1 item)
//!   - cntsum1: 3 (CountSumTree propagates its count: 3 sum items)
//!   - item1-item7: 7
//!   = 1+1+1+1+1+3+1+3+7 = 19 total
//!
//! Note: CountTree, CountSumTree, and ProvableCountTree propagate their internal
//! counts upward. Regular Tree, SumTree, BigSumTree all count as 1 each.
//! ```

#[cfg(test)]
mod tests {
    use grovedb_merk::{
        proofs::{query::query_item::QueryItem, Query},
        TreeFeatureType,
    };
    use grovedb_version::version::GroveVersion;

    use crate::{tests::make_empty_grovedb, Element, GroveDb, PathQuery, SizedQuery};

    #[test]
    fn test_provable_count_tree_with_mixed_subtrees() {
        let grove_version = GroveVersion::latest();
        let db = make_empty_grovedb();

        // Keys as variables for easier path construction
        let pcount = b"pcount";
        let tree1 = b"tree1";
        let tree2 = b"tree2";
        let sum1 = b"sum1";
        let sum2 = b"sum2";
        let sum3 = b"sum3";
        let cnt1 = b"cnt1";
        let cnt2 = b"cnt2";
        let cntsum1 = b"cntsum1";

        // =================================================================
        // STEP 1: Create the ProvableCountTree at root
        // =================================================================
        db.insert(
            &[] as &[&[u8]],
            pcount,
            Element::empty_provable_count_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert provable count tree");

        // =================================================================
        // STEP 2: Insert 2 normal Trees
        // =================================================================

        // tree1 - empty tree
        db.insert(
            &[pcount.as_slice()],
            tree1,
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert tree1");

        // tree2 - tree with one item
        db.insert(
            &[pcount.as_slice()],
            tree2,
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert tree2");

        db.insert(
            &[pcount.as_slice(), tree2.as_slice()],
            b"x",
            Element::new_item(b"value_x".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item x in tree2");

        // =================================================================
        // STEP 3: Insert 3 SumTrees
        // =================================================================

        // sum1 - with a sum item of value 100
        db.insert(
            &[pcount.as_slice()],
            sum1,
            Element::empty_sum_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert sum1");

        db.insert(
            &[pcount.as_slice(), sum1.as_slice()],
            b"s1_item",
            Element::new_sum_item(100),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert sum item in sum1");

        // sum2 - with a sum item of value 200
        db.insert(
            &[pcount.as_slice()],
            sum2,
            Element::empty_sum_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert sum2");

        db.insert(
            &[pcount.as_slice(), sum2.as_slice()],
            b"s2_item",
            Element::new_sum_item(200),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert sum item in sum2");

        // sum3 - empty sum tree
        db.insert(
            &[pcount.as_slice()],
            sum3,
            Element::empty_sum_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert sum3");

        // =================================================================
        // STEP 4: Insert 2 CountTrees
        // =================================================================

        // cnt1 - with two items
        db.insert(
            &[pcount.as_slice()],
            cnt1,
            Element::empty_count_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert cnt1");

        db.insert(
            &[pcount.as_slice(), cnt1.as_slice()],
            b"a",
            Element::new_item(b"value_a".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item a in cnt1");

        db.insert(
            &[pcount.as_slice(), cnt1.as_slice()],
            b"c",
            Element::new_item(b"value_c".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item c in cnt1");

        db.insert(
            &[pcount.as_slice(), cnt1.as_slice()],
            b"d",
            Element::new_item(b"value_d".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item d in cnt1");

        // cnt2 - with one item
        db.insert(
            &[pcount.as_slice()],
            cnt2,
            Element::empty_count_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert cnt2");

        db.insert(
            &[pcount.as_slice(), cnt2.as_slice()],
            b"b",
            Element::new_item(b"value_b".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert item b in cnt2");

        // =================================================================
        // STEP 5: Insert 1 CountSumTree
        // =================================================================

        // cntsum1 - with two sum items (values 50 and 75)
        db.insert(
            &[pcount.as_slice()],
            cntsum1,
            Element::empty_count_sum_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert cntsum1");

        db.insert(
            &[pcount.as_slice(), cntsum1.as_slice()],
            b"y",
            Element::new_sum_item(50),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert sum item y in cntsum1");

        db.insert(
            &[pcount.as_slice(), cntsum1.as_slice()],
            b"z",
            Element::new_sum_item(75),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert sum item z in cntsum1");

        db.insert(
            &[pcount.as_slice(), cntsum1.as_slice()],
            b"w",
            Element::new_sum_item(25),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("should insert sum item w in cntsum1");

        // =================================================================
        // STEP 6: Insert 7 Items directly in pcount
        // =================================================================
        for i in 1..=7u8 {
            let key = format!("item{}", i);
            let value = format!("value{}", i);
            db.insert(
                &[pcount.as_slice()],
                key.as_bytes(),
                Element::new_item(value.into_bytes()),
                None,
                None,
                grove_version,
            )
            .unwrap()
            .unwrap_or_else(|_| panic!("should insert {}", key));
        }

        // =================================================================
        // STEP 7: Verify the count by querying the ProvableCountTree
        // =================================================================

        // Get the ProvableCountTree element from root
        let pcount_element = db
            .get(&[] as &[&[u8]], pcount, None, grove_version)
            .unwrap()
            .expect("should get pcount");

        println!("ProvableCountTree element: {:?}", pcount_element);

        // The element should be a ProvableCountTree with count 19
        // Count = 1+1+1+1+1+3+1+3+7 = 19
        // (tree1=1, tree2=1, sum1=1, sum2=1, sum3=1, cnt1=3, cnt2=1, cntsum1=3,
        // items=7)
        match &pcount_element {
            Element::ProvableCountTree(root_key, count, _) => {
                println!("  Root key: {:?}", root_key);
                println!("  Count: {}", count);

                assert_eq!(
                    *count, 19,
                    "ProvableCountTree should have count 19, got {}",
                    count
                );
            }
            _ => panic!("Expected ProvableCountTree, got {:?}", pcount_element),
        }

        // =================================================================
        // STEP 8: Query each element and verify via proof
        // =================================================================

        // List of all keys in pcount (sorted alphabetically as they appear in tree)
        // Note: tree1 and sum3 are empty trees, they may return 0 results in queries
        // but they still contribute to the count
        let all_keys: Vec<(&[u8], bool)> = vec![
            (b"cnt1", true),
            (b"cnt2", true),
            (b"cntsum1", true),
            (b"item1", true),
            (b"item2", true),
            (b"item3", true),
            (b"item4", true),
            (b"item5", true),
            (b"item6", true),
            (b"item7", true),
            (b"sum1", true),
            (b"sum2", true),
            (b"sum3", true),   // empty sum tree - returns result
            (b"tree1", false), // empty tree with no root_key - returns 0 results
            (b"tree2", true),
        ];

        println!("\n=== Querying each element ===");

        for (key, expects_result) in &all_keys {
            let key_str = String::from_utf8_lossy(key);

            // Create a path query for this specific key
            let path_query = PathQuery::new(
                vec![pcount.to_vec()],
                SizedQuery::new(
                    Query::new_single_query_item(QueryItem::Key(key.to_vec())),
                    None,
                    None,
                ),
            );

            // Generate proof
            let proof = db
                .prove_query(&path_query, None, grove_version)
                .unwrap()
                .expect("should generate proof");

            // Verify proof and get parent tree info (which includes the count)
            let (verified_hash, parent_tree_type, results) =
                GroveDb::verify_query_get_parent_tree_info(&proof, &path_query, grove_version)
                    .expect("should verify proof");

            println!(
                "Key '{}': parent_tree_type={:?}, results={}",
                key_str,
                parent_tree_type,
                results.len()
            );

            // Verify the parent tree type is ProvableCountedMerkNode with count 19
            match parent_tree_type {
                TreeFeatureType::ProvableCountedMerkNode(count) => {
                    assert_eq!(
                        count, 19,
                        "Parent tree count for '{}' should be 19, got {}",
                        key_str, count
                    );
                }
                _ => panic!(
                    "Expected ProvableCountedMerkNode for '{}', got {:?}",
                    key_str, parent_tree_type
                ),
            }

            // Verify we got expected number of results
            if *expects_result {
                assert_eq!(
                    results.len(),
                    1,
                    "Should get exactly 1 result for key '{}'",
                    key_str
                );
            }

            // Verify the root hash matches
            let expected_root_hash = db.root_hash(None, grove_version).unwrap().unwrap();
            assert_eq!(
                verified_hash, expected_root_hash,
                "Root hash mismatch for key '{}'",
                key_str
            );
        }

        // =================================================================
        // STEP 9: Query all elements at once with RangeFull
        // =================================================================
        println!("\n=== Querying all elements with RangeFull ===");

        let range_query = PathQuery::new(
            vec![pcount.to_vec()],
            SizedQuery::new(
                Query::new_single_query_item(QueryItem::RangeFull(..)),
                None,
                None,
            ),
        );

        let proof = db
            .prove_query(&range_query, None, grove_version)
            .unwrap()
            .expect("should generate range proof");

        let (verified_hash, parent_tree_type, results) =
            GroveDb::verify_query_get_parent_tree_info(&proof, &range_query, grove_version)
                .expect("should verify range proof");

        println!("RangeFull query: {} results", results.len());
        println!("Parent tree type: {:?}", parent_tree_type);

        // Should get 14 elements (15 total minus tree1 which is empty and returns no
        // result) Total elements: cnt1, cnt2, cntsum1, item1-7, sum1, sum2,
        // sum3, tree1, tree2 = 15 tree1 is an empty Tree with no root_key, so
        // it doesn't appear in query results
        assert_eq!(
            results.len(),
            14,
            "RangeFull should return 14 elements (15 - 1 empty tree)"
        );

        // Verify parent tree type has count 19
        match parent_tree_type {
            TreeFeatureType::ProvableCountedMerkNode(count) => {
                assert_eq!(count, 19, "Parent tree count should be 19, got {}", count);
            }
            _ => panic!(
                "Expected ProvableCountedMerkNode, got {:?}",
                parent_tree_type
            ),
        }

        // Verify root hash
        let expected_root_hash = db.root_hash(None, grove_version).unwrap().unwrap();
        assert_eq!(verified_hash, expected_root_hash, "Root hash mismatch");

        println!("\n=== All assertions passed! ===");
        println!("Tree structure verified:");
        println!("  - 2 normal trees (tree1, tree2) -> count 1 each = 2");
        println!("  - 3 sum trees (sum1, sum2, sum3) -> count 1 each = 3");
        println!("  - 2 count trees: cnt1 (3 items), cnt2 (1 item) -> count 3+1 = 4");
        println!("  - 1 count sum tree: cntsum1 (3 items) -> count 3");
        println!("  - 7 items (item1-item7) -> count 7");
        println!("  - Total provable count: 2+3+4+3+7 = 19");
    }

    #[test]
    fn test_provable_count_tree_nested_queries() {
        //! Test querying items inside nested subtrees
        //!
        //! Tree Structure:
        //! ```text
        //!                    [root]
        //!                       |
        //!                ProvableCountTree("pcount")  [count=4]
        //!                       |
        //!         +-------------+-------------+
        //!         |             |             |
        //!      Tree          SumTree      CountTree
        //!     "tree1"        "sum1"        "cnt1"
        //!     [cnt=1]        [cnt=1]       [cnt=2]
        //!         |             |             |
        //!     Item"a"      SumItem"b"     Item"c"
        //!                   =500          Item"d"
        //!
        //! Count = tree1(1) + sum1(1) + cnt1(2) = 4
        //! ```

        let grove_version = GroveVersion::latest();
        let db = make_empty_grovedb();

        let pcount = b"pcount";
        let tree1 = b"tree1";
        let sum1 = b"sum1";
        let cnt1 = b"cnt1";

        // Create ProvableCountTree
        db.insert(
            &[] as &[&[u8]],
            pcount,
            Element::empty_provable_count_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("insert pcount");

        // Insert tree1 with item
        db.insert(
            &[pcount.as_slice()],
            tree1,
            Element::empty_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("insert tree1");

        db.insert(
            &[pcount.as_slice(), tree1.as_slice()],
            b"a",
            Element::new_item(b"value_a".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("insert a");

        // Insert sum1 with sum item
        db.insert(
            &[pcount.as_slice()],
            sum1,
            Element::empty_sum_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("insert sum1");

        db.insert(
            &[pcount.as_slice(), sum1.as_slice()],
            b"b",
            Element::new_sum_item(500),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("insert b");

        // Insert cnt1 with items
        db.insert(
            &[pcount.as_slice()],
            cnt1,
            Element::empty_count_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("insert cnt1");

        db.insert(
            &[pcount.as_slice(), cnt1.as_slice()],
            b"c",
            Element::new_item(b"value_c".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("insert c");

        db.insert(
            &[pcount.as_slice(), cnt1.as_slice()],
            b"d",
            Element::new_item(b"value_d".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("insert d");

        // Verify pcount has count 4 (tree1=1, sum1=1, cnt1=2)
        let pcount_elem = db
            .get(&[] as &[&[u8]], pcount, None, grove_version)
            .unwrap()
            .expect("get pcount");

        match &pcount_elem {
            Element::ProvableCountTree(_, count, _) => {
                assert_eq!(
                    *count, 4,
                    "pcount should have count 4 (tree1=1 + sum1=1 + cnt1=2)"
                );
            }
            _ => panic!("Expected ProvableCountTree"),
        }

        // Query item "a" inside tree1 (path: pcount/tree1, key: a)
        println!("\n=== Querying nested item 'a' in tree1 ===");
        let query_a = PathQuery::new(
            vec![pcount.to_vec(), tree1.to_vec()],
            SizedQuery::new(
                Query::new_single_query_item(QueryItem::Key(b"a".to_vec())),
                None,
                None,
            ),
        );

        let proof_a = db
            .prove_query(&query_a, None, grove_version)
            .unwrap()
            .expect("prove query a");

        let (hash_a, results_a) =
            GroveDb::verify_query(&proof_a, &query_a, grove_version).expect("verify query a");

        assert_eq!(results_a.len(), 1, "Should find item a");
        println!("Found item 'a': {:?}", results_a[0]);

        // Verify root hash
        let expected_hash = db.root_hash(None, grove_version).unwrap().unwrap();
        assert_eq!(hash_a, expected_hash, "Root hash should match for query a");

        // Query sum item "b" inside sum1
        println!("\n=== Querying nested sum item 'b' in sum1 ===");
        let query_b = PathQuery::new(
            vec![pcount.to_vec(), sum1.to_vec()],
            SizedQuery::new(
                Query::new_single_query_item(QueryItem::Key(b"b".to_vec())),
                None,
                None,
            ),
        );

        let proof_b = db
            .prove_query(&query_b, None, grove_version)
            .unwrap()
            .expect("prove query b");

        let (hash_b, results_b) =
            GroveDb::verify_query(&proof_b, &query_b, grove_version).expect("verify query b");

        assert_eq!(results_b.len(), 1, "Should find sum item b");
        println!("Found sum item 'b': {:?}", results_b[0]);
        assert_eq!(hash_b, expected_hash, "Root hash should match for query b");

        // Query items in cnt1
        println!("\n=== Querying nested items in cnt1 ===");
        let query_cnt1 = PathQuery::new(
            vec![pcount.to_vec(), cnt1.to_vec()],
            SizedQuery::new(
                Query::new_single_query_item(QueryItem::RangeFull(..)),
                None,
                None,
            ),
        );

        let proof_cnt1 = db
            .prove_query(&query_cnt1, None, grove_version)
            .unwrap()
            .expect("prove query cnt1");

        let (hash_cnt1, results_cnt1) =
            GroveDb::verify_query(&proof_cnt1, &query_cnt1, grove_version)
                .expect("verify query cnt1");

        assert_eq!(results_cnt1.len(), 2, "Should find 2 items in cnt1");
        println!("Found {} items in cnt1", results_cnt1.len());
        assert_eq!(
            hash_cnt1, expected_hash,
            "Root hash should match for query cnt1"
        );

        println!("\n=== Nested query test passed! ===");
    }

    #[test]
    fn test_proof_comparison_provable_vs_regular_count_tree() {
        //! Compare proof output between ProvableCountTree and regular CountTree
        //!
        //! This test creates two similar structures:
        //! 1. A ProvableCountTree with 3 items
        //! 2. A regular CountTree with 3 items
        //!
        //! Then queries both and prints the proofs to show the difference:
        //! - ProvableCountTree proofs include KVValueHashFeatureType nodes with
        //!   count
        //! - Regular CountTree proofs use standard KV/KVValueHash nodes

        let grove_version = GroveVersion::latest();

        // =====================================================================
        // PART 1: Create and query a ProvableCountTree
        // =====================================================================
        println!("\n======================================================================");
        println!("PROVABLE COUNT TREE PROOF");
        println!("======================================================================\n");

        let db1 = make_empty_grovedb();

        // Create ProvableCountTree
        db1.insert(
            &[] as &[&[u8]],
            b"pcount",
            Element::empty_provable_count_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("insert provable count tree");

        // Insert 3 items
        for i in 1..=3u8 {
            let key = format!("item{}", i);
            let value = format!("value{}", i);
            db1.insert(
                &[b"pcount".as_slice()],
                key.as_bytes(),
                Element::new_item(value.into_bytes()),
                None,
                None,
                grove_version,
            )
            .unwrap()
            .expect("insert item");
        }

        // Get the element to show the count
        let pcount_elem = db1
            .get(&[] as &[&[u8]], b"pcount", None, grove_version)
            .unwrap()
            .expect("get pcount");
        println!("ProvableCountTree element: {:?}\n", pcount_elem);

        // Query all items
        let query1 = PathQuery::new(
            vec![b"pcount".to_vec()],
            SizedQuery::new(
                Query::new_single_query_item(QueryItem::RangeFull(..)),
                None,
                None,
            ),
        );

        // Get non-serialized proof so we can display it
        let proof1 = db1
            .prove_query_non_serialized(&query1, None, grove_version)
            .unwrap()
            .expect("prove query");

        println!("Proof structure for ProvableCountTree:");
        println!("{}", proof1);

        // =====================================================================
        // PART 2: Create and query a regular CountTree
        // =====================================================================
        println!("\n======================================================================");
        println!("REGULAR COUNT TREE PROOF");
        println!("======================================================================\n");

        let db2 = make_empty_grovedb();

        // Create regular CountTree
        db2.insert(
            &[] as &[&[u8]],
            b"count",
            Element::empty_count_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("insert count tree");

        // Insert 3 items
        for i in 1..=3u8 {
            let key = format!("item{}", i);
            let value = format!("value{}", i);
            db2.insert(
                &[b"count".as_slice()],
                key.as_bytes(),
                Element::new_item(value.into_bytes()),
                None,
                None,
                grove_version,
            )
            .unwrap()
            .expect("insert item");
        }

        // Get the element to show the count
        let count_elem = db2
            .get(&[] as &[&[u8]], b"count", None, grove_version)
            .unwrap()
            .expect("get count");
        println!("CountTree element: {:?}\n", count_elem);

        // Query all items
        let query2 = PathQuery::new(
            vec![b"count".to_vec()],
            SizedQuery::new(
                Query::new_single_query_item(QueryItem::RangeFull(..)),
                None,
                None,
            ),
        );

        // Get non-serialized proof
        let proof2 = db2
            .prove_query_non_serialized(&query2, None, grove_version)
            .unwrap()
            .expect("prove query");

        println!("Proof structure for regular CountTree:");
        println!("{}", proof2);

        // =====================================================================
        // PART 3: Highlight the key differences
        // =====================================================================
        println!("\n======================================================================");
        println!("KEY DIFFERENCES");
        println!("======================================================================\n");

        println!("1. ProvableCountTree uses KVValueHashFeatureType nodes which include:");
        println!("   - The element value");
        println!("   - The value hash");
        println!("   - The ProvableCountedMerkNode(count) feature type");
        println!("   This allows the count to be verified as part of the proof.\n");

        println!("2. Regular CountTree uses standard KV or KVValueHash nodes which:");
        println!("   - Only include the key and value");
        println!("   - The count is NOT included in the proof");
        println!("   - Count can only be verified by querying the tree directly.\n");

        println!("3. The root hash calculation differs:");
        println!("   - ProvableCountTree: hash includes the count value");
        println!("   - Regular CountTree: hash does NOT include the count");
    }

    #[test]
    fn test_hash_calculation_difference_provable_vs_regular() {
        //! Manually hash up the entire merk tree structure to reproduce the
        //! exact root hash that GroveDB produces, proving we understand
        //! the hash calculation.
        //!
        //! Tree structure:
        //! ```
        //! Root Merk (NormalTree)
        //!   └── "tree" key -> ProvableCountTree/CountTree element
        //!         Inner Merk (ProvableCountTree or NormalTree)
        //!           └── "key1" key -> Item("value1")
        //! ```
        //!
        //! Hash calculation:
        //! 1. Inner merk: hash the "key1" -> Item node
        //!    - For ProvableCountTree: uses node_hash_with_count(kv, left,
        //!      right, count)
        //!    - For CountTree: uses node_hash(kv, left, right)
        //! 2. Create tree Element with inner merk root hash
        //! 3. Outer merk: for subtrees, uses LAYERED reference hash formula:
        //!    - actual_value_hash = value_hash(serialized_element)
        //!    - combined_value_hash = combine_hash(actual_value_hash,
        //!      inner_root_hash)
        //!    - kv_hash = kv_digest_to_kv_hash(key, combined_value_hash)

        use grovedb_merk::tree::hash::{
            combine_hash, kv_digest_to_kv_hash, node_hash, node_hash_with_count, value_hash,
            NULL_HASH,
        };

        let grove_version = GroveVersion::latest();

        println!("\n======================================================================");
        println!("MANUAL HASH CALCULATION TO MATCH GROVEDB ROOT HASH");
        println!("======================================================================\n");

        // Create the databases
        let db_provable = make_empty_grovedb();
        let db_regular = make_empty_grovedb();

        // Insert ProvableCountTree with one item
        db_provable
            .insert(
                &[] as &[&[u8]],
                b"tree",
                Element::empty_provable_count_tree(),
                None,
                None,
                grove_version,
            )
            .unwrap()
            .expect("insert");

        db_provable
            .insert(
                &[b"tree".as_slice()],
                b"key1",
                Element::new_item(b"value1".to_vec()),
                None,
                None,
                grove_version,
            )
            .unwrap()
            .expect("insert");

        // Insert regular CountTree with identical content
        db_regular
            .insert(
                &[] as &[&[u8]],
                b"tree",
                Element::empty_count_tree(),
                None,
                None,
                grove_version,
            )
            .unwrap()
            .expect("insert");

        db_regular
            .insert(
                &[b"tree".as_slice()],
                b"key1",
                Element::new_item(b"value1".to_vec()),
                None,
                None,
                grove_version,
            )
            .unwrap()
            .expect("insert");

        // Get the actual root hashes from GroveDB
        let grovedb_root_provable = db_provable.root_hash(None, grove_version).unwrap().unwrap();
        let grovedb_root_regular = db_regular.root_hash(None, grove_version).unwrap().unwrap();

        println!("Target root hashes from GroveDB:");
        println!(
            "  ProvableCountTree: {}",
            hex::encode(grovedb_root_provable)
        );
        println!("  Regular CountTree: {}", hex::encode(grovedb_root_regular));
        println!();

        // =====================================================================
        // STEP 1: Calculate inner merk node hash for "key1" -> Item("value1")
        // =====================================================================
        println!("======================================================================");
        println!("STEP 1: Inner merk - hash the 'key1' -> Item node");
        println!("======================================================================\n");

        let inner_key = b"key1";
        let inner_value = Element::new_item(b"value1".to_vec())
            .serialize(grove_version)
            .unwrap();

        // For Items in a merk, we use regular kv_hash
        // kv_hash = kv_digest_to_kv_hash(key, value_hash(value))
        let inner_value_hash = value_hash(&inner_value).unwrap();
        let inner_kv = kv_digest_to_kv_hash(inner_key, &inner_value_hash).unwrap();

        println!("inner_value_hash = value_hash(Item(\"value1\").serialize())");
        println!("                 = {}", hex::encode(inner_value_hash));
        println!();
        println!("inner_kv_hash = kv_digest_to_kv_hash(b\"key1\", inner_value_hash)");
        println!("              = {}", hex::encode(inner_kv));
        println!();

        // For a leaf node, children are NULL_HASH
        // ProvableCountTree: uses node_hash_with_count (count=1 for single item)
        // Regular CountTree: uses node_hash
        let inner_root_provable =
            node_hash_with_count(&inner_kv, &NULL_HASH, &NULL_HASH, 1).unwrap();
        let inner_root_regular = node_hash(&inner_kv, &NULL_HASH, &NULL_HASH).unwrap();

        println!("For ProvableCountTree inner merk (uses node_hash_with_count):");
        println!("  inner_root = node_hash_with_count(kv, NULL, NULL, count=1)");
        println!("             = {}", hex::encode(inner_root_provable));
        println!();

        println!("For regular CountTree inner merk (uses node_hash):");
        println!("  inner_root = node_hash(kv, NULL, NULL)");
        println!("             = {}", hex::encode(inner_root_regular));
        println!();

        // =====================================================================
        // STEP 2: Create the tree Element and calculate outer merk node hash
        // =====================================================================
        println!("======================================================================");
        println!("STEP 2: Outer merk - calculate layered reference hash");
        println!("======================================================================\n");

        // IMPORTANT: The Element stores the ROOT KEY (the key of the root node in
        // the inner merk), NOT the root hash! The root hash is passed separately
        // to the PutLayeredReference operation.
        //
        // Element stores:
        // - root_key: The key of the root node in the inner merk (e.g., b"key1")
        // - count: The aggregate count value
        // - flags: Optional storage flags
        //
        // The root hash is passed separately via PutLayeredReference and is combined
        // with the serialized element via combine_hash.

        // The root_key is the key of the only/root element in the inner merk
        let inner_root_key = Some(b"key1".to_vec());

        let provable_element = Element::ProvableCountTree(inner_root_key.clone(), 1, None);
        let provable_element_bytes = provable_element.serialize(grove_version).unwrap();

        let regular_element = Element::CountTree(inner_root_key, 1, None);
        let regular_element_bytes = regular_element.serialize(grove_version).unwrap();

        println!(
            "ProvableCountTree element serialized = {} bytes",
            provable_element_bytes.len()
        );
        println!(
            "Regular CountTree element serialized = {} bytes",
            regular_element_bytes.len()
        );
        println!();

        // For subtrees (layered references), the hash is calculated as:
        // 1. actual_value_hash = value_hash(serialized_element)
        // 2. combined_value_hash = combine_hash(actual_value_hash, inner_root_hash)
        // 3. kv_hash = kv_digest_to_kv_hash(key, combined_value_hash)

        let outer_key = b"tree";

        // ProvableCountTree
        let provable_actual_value_hash = value_hash(&provable_element_bytes).unwrap();
        let provable_combined_value_hash =
            combine_hash(&provable_actual_value_hash, &inner_root_provable).unwrap();
        let outer_kv_provable =
            kv_digest_to_kv_hash(outer_key, &provable_combined_value_hash).unwrap();

        println!("ProvableCountTree layered hash calculation:");
        println!(
            "  actual_value_hash   = value_hash(element) = {}",
            hex::encode(provable_actual_value_hash)
        );
        println!(
            "  combined_value_hash = combine_hash(actual, inner_root) = {}",
            hex::encode(provable_combined_value_hash)
        );
        println!(
            "  outer_kv_hash       = kv_digest_to_kv_hash(key, combined) = {}",
            hex::encode(outer_kv_provable)
        );
        println!();

        // Regular CountTree
        let regular_actual_value_hash = value_hash(&regular_element_bytes).unwrap();
        let regular_combined_value_hash =
            combine_hash(&regular_actual_value_hash, &inner_root_regular).unwrap();
        let outer_kv_regular =
            kv_digest_to_kv_hash(outer_key, &regular_combined_value_hash).unwrap();

        println!("Regular CountTree layered hash calculation:");
        println!(
            "  actual_value_hash   = value_hash(element) = {}",
            hex::encode(regular_actual_value_hash)
        );
        println!(
            "  combined_value_hash = combine_hash(actual, inner_root) = {}",
            hex::encode(regular_combined_value_hash)
        );
        println!(
            "  outer_kv_hash       = kv_digest_to_kv_hash(key, combined) = {}",
            hex::encode(outer_kv_regular)
        );
        println!();

        // =====================================================================
        // STEP 3: Calculate outer merk root hash
        // =====================================================================
        println!("======================================================================");
        println!("STEP 3: Calculate outer merk root hash");
        println!("======================================================================\n");

        // The root merk is always a NormalTree, so it uses regular node_hash
        let manual_root_provable = node_hash(&outer_kv_provable, &NULL_HASH, &NULL_HASH).unwrap();
        let manual_root_regular = node_hash(&outer_kv_regular, &NULL_HASH, &NULL_HASH).unwrap();

        println!(
            "manual_root (ProvableCountTree) = node_hash(outer_kv, NULL, NULL) = {}",
            hex::encode(manual_root_provable)
        );
        println!(
            "manual_root (Regular CountTree) = node_hash(outer_kv, NULL, NULL) = {}",
            hex::encode(manual_root_regular)
        );
        println!();

        // =====================================================================
        // STEP 4: VERIFY - Manual calculation equals GroveDB root hash
        // =====================================================================
        println!("======================================================================");
        println!("VERIFICATION: Manual calculation vs GroveDB");
        println!("======================================================================\n");

        println!("ProvableCountTree:");
        println!("  GroveDB:  {}", hex::encode(grovedb_root_provable));
        println!("  Manual:   {}", hex::encode(manual_root_provable));
        assert_eq!(
            grovedb_root_provable, manual_root_provable,
            "Manual ProvableCountTree hash must equal GroveDB!"
        );
        println!("  ✓ MATCH!\n");

        println!("Regular CountTree:");
        println!("  GroveDB:  {}", hex::encode(grovedb_root_regular));
        println!("  Manual:   {}", hex::encode(manual_root_regular));
        assert_eq!(
            grovedb_root_regular, manual_root_regular,
            "Manual CountTree hash must equal GroveDB!"
        );
        println!("  ✓ MATCH!\n");

        // =====================================================================
        // SUMMARY
        // =====================================================================
        println!("======================================================================");
        println!("SUCCESS: We manually calculated the exact root hash!");
        println!("======================================================================\n");

        println!("Key differences between ProvableCountTree and CountTree:\n");

        println!("1. Inner merk node hash calculation:");
        println!("   - ProvableCountTree: node_hash_with_count(kv, left, right, count)");
        println!("   - Regular CountTree: node_hash(kv, left, right)");
        println!();

        println!("2. This causes the inner root hash to differ:");
        println!(
            "   - ProvableCountTree inner: {}",
            hex::encode(inner_root_provable)
        );
        println!(
            "   - Regular CountTree inner: {}",
            hex::encode(inner_root_regular)
        );
        println!();

        println!("3. Outer merk uses LAYERED reference hash (for all subtrees):");
        println!("   combined_value_hash = combine_hash(value_hash(element), inner_root)");
        println!("   This binds the inner merk's root hash into the outer merk's kv_hash.");
        println!();

        println!("4. The count is cryptographically bound because:");
        println!("   - It's included in node_hash_with_count input for ProvableCountTree");
        println!("   - Any change to count changes the inner root hash");
        println!("   - Which changes the combined_value_hash in outer merk");
        println!("   - Which changes the outer root hash");
        println!("   - A verifier would detect the mismatch immediately");
    }

    #[test]
    fn test_hash_calculation_with_two_items() {
        //! More complex test with 2 items creating a non-trivial inner merk
        //! structure.
        //!
        //! Tree structure:
        //! ```
        //! Root Merk (NormalTree)
        //!   └── "tree" key -> ProvableCountTree/CountTree element
        //!         Inner Merk (ProvableCountTree or NormalTree)
        //!           └── "key1" (root)
        //!                  └── right: "key2"
        //! ```
        //!
        //! With 2 items inserted in order "key1" then "key2":
        //! - "key1" becomes the root (inserted first)
        //! - "key2" becomes the right child (key2 > key1 lexicographically)
        //!
        //! Inner merk hash calculation for 2 nodes:
        //! 1. Calculate kv_hash for "key2" leaf node
        //! 2. Calculate node_hash for "key2" (leaf, no children)
        //! 3. Calculate kv_hash for "key1" root node
        //! 4. Calculate node_hash for "key1" with right child = key2's hash
        //!    - For ProvableCountTree: node_hash_with_count includes count=2
        //!    - For CountTree: regular node_hash

        use grovedb_merk::tree::hash::{
            combine_hash, kv_digest_to_kv_hash, node_hash, node_hash_with_count, value_hash,
            NULL_HASH,
        };

        let grove_version = GroveVersion::latest();

        println!("\n======================================================================");
        println!("MANUAL HASH CALCULATION WITH 2 ITEMS (MORE COMPLEX TREE)");
        println!("======================================================================\n");

        // Create the databases
        let db_provable = make_empty_grovedb();
        let db_regular = make_empty_grovedb();

        // Insert ProvableCountTree with TWO items
        db_provable
            .insert(
                &[] as &[&[u8]],
                b"tree",
                Element::empty_provable_count_tree(),
                None,
                None,
                grove_version,
            )
            .unwrap()
            .expect("insert");

        db_provable
            .insert(
                &[b"tree".as_slice()],
                b"key1",
                Element::new_item(b"value1".to_vec()),
                None,
                None,
                grove_version,
            )
            .unwrap()
            .expect("insert");

        db_provable
            .insert(
                &[b"tree".as_slice()],
                b"key2",
                Element::new_item(b"value2".to_vec()),
                None,
                None,
                grove_version,
            )
            .unwrap()
            .expect("insert");

        // Insert regular CountTree with identical content
        db_regular
            .insert(
                &[] as &[&[u8]],
                b"tree",
                Element::empty_count_tree(),
                None,
                None,
                grove_version,
            )
            .unwrap()
            .expect("insert");

        db_regular
            .insert(
                &[b"tree".as_slice()],
                b"key1",
                Element::new_item(b"value1".to_vec()),
                None,
                None,
                grove_version,
            )
            .unwrap()
            .expect("insert");

        db_regular
            .insert(
                &[b"tree".as_slice()],
                b"key2",
                Element::new_item(b"value2".to_vec()),
                None,
                None,
                grove_version,
            )
            .unwrap()
            .expect("insert");

        // Get the actual root hashes from GroveDB
        let grovedb_root_provable = db_provable.root_hash(None, grove_version).unwrap().unwrap();
        let grovedb_root_regular = db_regular.root_hash(None, grove_version).unwrap().unwrap();

        println!("Target root hashes from GroveDB:");
        println!(
            "  ProvableCountTree: {}",
            hex::encode(grovedb_root_provable)
        );
        println!("  Regular CountTree: {}", hex::encode(grovedb_root_regular));
        println!();

        // =====================================================================
        // STEP 1: Calculate inner merk with 2 nodes
        // =====================================================================
        println!("======================================================================");
        println!("STEP 1: Inner merk - hash 2-node tree structure");
        println!("======================================================================\n");

        // Serialize the item values
        let value1_serialized = Element::new_item(b"value1".to_vec())
            .serialize(grove_version)
            .unwrap();
        let value2_serialized = Element::new_item(b"value2".to_vec())
            .serialize(grove_version)
            .unwrap();

        // Calculate kv_hash for "key2" (will be right child of "key1")
        let key2_value_hash = value_hash(&value2_serialized).unwrap();
        let key2_kv_hash = kv_digest_to_kv_hash(b"key2", &key2_value_hash).unwrap();

        println!("Node 'key2' (leaf, right child of root):");
        println!("  value_hash = {}", hex::encode(key2_value_hash));
        println!("  kv_hash    = {}", hex::encode(key2_kv_hash));

        // For a leaf node, both children are NULL_HASH
        // In ProvableCountTree, each leaf contributes count=1
        let key2_node_hash_provable =
            node_hash_with_count(&key2_kv_hash, &NULL_HASH, &NULL_HASH, 1).unwrap();
        let key2_node_hash_regular = node_hash(&key2_kv_hash, &NULL_HASH, &NULL_HASH).unwrap();

        println!(
            "  node_hash (ProvableCountTree, count=1) = {}",
            hex::encode(key2_node_hash_provable)
        );
        println!(
            "  node_hash (Regular)                    = {}",
            hex::encode(key2_node_hash_regular)
        );
        println!();

        // Calculate kv_hash for "key1" (root node)
        let key1_value_hash = value_hash(&value1_serialized).unwrap();
        let key1_kv_hash = kv_digest_to_kv_hash(b"key1", &key1_value_hash).unwrap();

        println!("Node 'key1' (root, has right child 'key2'):");
        println!("  value_hash = {}", hex::encode(key1_value_hash));
        println!("  kv_hash    = {}", hex::encode(key1_kv_hash));

        // key1 is root with:
        // - left child: NULL_HASH (no left child since key2 > key1)
        // - right child: key2's node hash
        // For ProvableCountTree root: count = 1 (self) + 1 (right child) = 2
        let inner_root_provable = node_hash_with_count(
            &key1_kv_hash,
            &NULL_HASH,               // left child
            &key2_node_hash_provable, // right child
            2,                        // total count: key1 + key2
        )
        .unwrap();

        let inner_root_regular = node_hash(
            &key1_kv_hash,
            &NULL_HASH,              // left child
            &key2_node_hash_regular, // right child
        )
        .unwrap();

        println!(
            "  inner_root (ProvableCountTree, count=2) = {}",
            hex::encode(inner_root_provable)
        );
        println!(
            "  inner_root (Regular)                    = {}",
            hex::encode(inner_root_regular)
        );
        println!();

        // =====================================================================
        // STEP 2: Create the tree Element and calculate outer merk node hash
        // =====================================================================
        println!("======================================================================");
        println!("STEP 2: Outer merk - calculate layered reference hash");
        println!("======================================================================\n");

        // The root_key is "key1" (the key of the root node in the inner merk)
        let inner_root_key = Some(b"key1".to_vec());

        // Element stores root_key and count (count=2 for 2 items)
        let provable_element = Element::ProvableCountTree(inner_root_key.clone(), 2, None);
        let provable_element_bytes = provable_element.serialize(grove_version).unwrap();

        let regular_element = Element::CountTree(inner_root_key, 2, None);
        let regular_element_bytes = regular_element.serialize(grove_version).unwrap();

        println!(
            "ProvableCountTree element: count=2, serialized = {} bytes",
            provable_element_bytes.len()
        );
        println!(
            "Regular CountTree element: count=2, serialized = {} bytes",
            regular_element_bytes.len()
        );
        println!();

        // Layered reference hash calculation
        let outer_key = b"tree";

        // ProvableCountTree
        let provable_actual_value_hash = value_hash(&provable_element_bytes).unwrap();
        let provable_combined_value_hash =
            combine_hash(&provable_actual_value_hash, &inner_root_provable).unwrap();
        let outer_kv_provable =
            kv_digest_to_kv_hash(outer_key, &provable_combined_value_hash).unwrap();

        println!("ProvableCountTree layered hash:");
        println!(
            "  actual_value_hash   = {}",
            hex::encode(provable_actual_value_hash)
        );
        println!(
            "  combined_value_hash = {}",
            hex::encode(provable_combined_value_hash)
        );
        println!("  outer_kv_hash       = {}", hex::encode(outer_kv_provable));
        println!();

        // Regular CountTree
        let regular_actual_value_hash = value_hash(&regular_element_bytes).unwrap();
        let regular_combined_value_hash =
            combine_hash(&regular_actual_value_hash, &inner_root_regular).unwrap();
        let outer_kv_regular =
            kv_digest_to_kv_hash(outer_key, &regular_combined_value_hash).unwrap();

        println!("Regular CountTree layered hash:");
        println!(
            "  actual_value_hash   = {}",
            hex::encode(regular_actual_value_hash)
        );
        println!(
            "  combined_value_hash = {}",
            hex::encode(regular_combined_value_hash)
        );
        println!("  outer_kv_hash       = {}", hex::encode(outer_kv_regular));
        println!();

        // =====================================================================
        // STEP 3: Calculate outer merk root hash
        // =====================================================================
        println!("======================================================================");
        println!("STEP 3: Calculate outer merk root hash");
        println!("======================================================================\n");

        let manual_root_provable = node_hash(&outer_kv_provable, &NULL_HASH, &NULL_HASH).unwrap();
        let manual_root_regular = node_hash(&outer_kv_regular, &NULL_HASH, &NULL_HASH).unwrap();

        println!(
            "manual_root (ProvableCountTree) = {}",
            hex::encode(manual_root_provable)
        );
        println!(
            "manual_root (Regular CountTree) = {}",
            hex::encode(manual_root_regular)
        );
        println!();

        // =====================================================================
        // STEP 4: VERIFY - Manual calculation equals GroveDB root hash
        // =====================================================================
        println!("======================================================================");
        println!("VERIFICATION: Manual calculation vs GroveDB");
        println!("======================================================================\n");

        println!("ProvableCountTree (2 items):");
        println!("  GroveDB:  {}", hex::encode(grovedb_root_provable));
        println!("  Manual:   {}", hex::encode(manual_root_provable));
        assert_eq!(
            grovedb_root_provable, manual_root_provable,
            "Manual ProvableCountTree hash must equal GroveDB!"
        );
        println!("  ✓ MATCH!\n");

        println!("Regular CountTree (2 items):");
        println!("  GroveDB:  {}", hex::encode(grovedb_root_regular));
        println!("  Manual:   {}", hex::encode(manual_root_regular));
        assert_eq!(
            grovedb_root_regular, manual_root_regular,
            "Manual CountTree hash must equal GroveDB!"
        );
        println!("  ✓ MATCH!\n");

        // =====================================================================
        // SUMMARY
        // =====================================================================
        println!("======================================================================");
        println!("SUCCESS: Manual hash calculation matches for 2-item tree!");
        println!("======================================================================\n");

        println!("Tree structure:");
        println!("  'key1' (root)");
        println!("     └── right: 'key2' (leaf)");
        println!();

        println!("Hash propagation in ProvableCountTree:");
        println!("  1. key2 leaf: node_hash_with_count(kv2, NULL, NULL, count=1)");
        println!("  2. key1 root: node_hash_with_count(kv1, NULL, key2_hash, count=2)");
        println!("  3. The count=2 at root includes both items");
        println!();

        println!("This proves that:");
        println!("  - Count propagates correctly up the tree (1+1=2)");
        println!("  - Each node's hash includes its subtree's aggregate count");
        println!("  - Changing any count anywhere would invalidate the root hash");
    }

    #[test]
    fn test_nested_provable_count_trees_with_batch_operations() {
        //! Test deeply nested ProvableCountTrees with batch operations across
        //! multiple levels.
        //!
        //! Tree structure (before batch):
        //! ```text
        //!                              [root]
        //!                                 |
        //!                      ProvableCountTree("level0")
        //!                       [initial count = 8]
        //!                                 |
        //!     +--------+--------+--------+--------+--------+
        //!     |        |        |        |        |        |
        //!  Item"a"  Item"b"  Item"c"  Item"d"  Item"e"  ProvableCountTree("level1")
        //!                                                   [count = 3]
        //!                                                      |
        //!                              +--------+--------+--------+
        //!                              |        |        |        |
        //!                           Item"f"  Item"g"  ProvableCountTree("level2")
        //!                                                   [count = 1]
        //!                                                      |
        //!                                                   Item"h"
        //!
        //! Count calculation (before batch):
        //!   level2: 1 (just item h)
        //!   level1: f(1) + g(1) + level2(1) = 3
        //!   level0: a(1) + b(1) + c(1) + d(1) + e(1) + level1(3) = 8
        //! ```
        //!
        //! After batch operation (adds 2 items to each level):
        //! ```text
        //!   level2: h + i + j = 3
        //!   level1: f + g + level2(3) + k + l = 7
        //!   level0: a + b + c + d + e + level1(7) + m + n = 14
        //! ```

        use crate::batch::QualifiedGroveDbOp;

        let grove_version = GroveVersion::latest();
        let db = make_empty_grovedb();

        // Keys
        let level0 = b"level0";
        let level1 = b"level1";
        let level2 = b"level2";

        // =================================================================
        // PHASE 1: Build initial structure with individual inserts
        // =================================================================
        println!("\n======================================================================");
        println!("PHASE 1: Building initial nested ProvableCountTree structure");
        println!("======================================================================\n");

        // Create level0 ProvableCountTree at root
        db.insert(
            &[] as &[&[u8]],
            level0,
            Element::empty_provable_count_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("insert level0");

        // Insert 5 items at level0
        for key in &[b"a", b"b", b"c", b"d", b"e"] {
            db.insert(
                &[level0.as_slice()],
                *key,
                Element::new_item(format!("value_{}", String::from_utf8_lossy(*key)).into_bytes()),
                None,
                None,
                grove_version,
            )
            .unwrap()
            .expect("insert item at level0");
        }

        // Create level1 ProvableCountTree inside level0
        db.insert(
            &[level0.as_slice()],
            level1,
            Element::empty_provable_count_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("insert level1");

        // Insert 2 items at level1
        for key in &[b"f", b"g"] {
            db.insert(
                &[level0.as_slice(), level1.as_slice()],
                *key,
                Element::new_item(format!("value_{}", String::from_utf8_lossy(*key)).into_bytes()),
                None,
                None,
                grove_version,
            )
            .unwrap()
            .expect("insert item at level1");
        }

        // Create level2 ProvableCountTree inside level1
        db.insert(
            &[level0.as_slice(), level1.as_slice()],
            level2,
            Element::empty_provable_count_tree(),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("insert level2");

        // Insert 1 item at level2
        db.insert(
            &[level0.as_slice(), level1.as_slice(), level2.as_slice()],
            b"h",
            Element::new_item(b"value_h".to_vec()),
            None,
            None,
            grove_version,
        )
        .unwrap()
        .expect("insert item h at level2");

        // =================================================================
        // Verify initial counts
        // =================================================================
        println!("Verifying initial counts...\n");

        // level2 should have count 1
        let level2_elem = db
            .get(
                &[level0.as_slice(), level1.as_slice()],
                level2,
                None,
                grove_version,
            )
            .unwrap()
            .expect("get level2");

        match &level2_elem {
            Element::ProvableCountTree(_, count, _) => {
                println!("level2 count: {} (expected: 1)", count);
                assert_eq!(*count, 1, "level2 should have count 1");
            }
            _ => panic!("Expected ProvableCountTree for level2"),
        }

        // level1 should have count 3 (f + g + level2=1)
        let level1_elem = db
            .get(&[level0.as_slice()], level1, None, grove_version)
            .unwrap()
            .expect("get level1");

        match &level1_elem {
            Element::ProvableCountTree(_, count, _) => {
                println!("level1 count: {} (expected: 3)", count);
                assert_eq!(*count, 3, "level1 should have count 3");
            }
            _ => panic!("Expected ProvableCountTree for level1"),
        }

        // level0 should have count 8 (a+b+c+d+e + level1=3)
        let level0_elem = db
            .get(&[] as &[&[u8]], level0, None, grove_version)
            .unwrap()
            .expect("get level0");

        match &level0_elem {
            Element::ProvableCountTree(_, count, _) => {
                println!("level0 count: {} (expected: 8)", count);
                assert_eq!(*count, 8, "level0 should have count 8");
            }
            _ => panic!("Expected ProvableCountTree for level0"),
        }

        let root_hash_before = db.root_hash(None, grove_version).unwrap().unwrap();
        println!(
            "\nRoot hash before batch: {}",
            hex::encode(root_hash_before)
        );

        // =================================================================
        // PHASE 2: Batch operation - add 2 items to each level
        // =================================================================
        println!("\n======================================================================");
        println!("PHASE 2: Executing batch operation across all levels");
        println!("======================================================================\n");

        let ops = vec![
            // Add 2 items to level2
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![level0.to_vec(), level1.to_vec(), level2.to_vec()],
                b"i".to_vec(),
                Element::new_item(b"value_i".to_vec()),
            ),
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![level0.to_vec(), level1.to_vec(), level2.to_vec()],
                b"j".to_vec(),
                Element::new_item(b"value_j".to_vec()),
            ),
            // Add 2 items to level1
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![level0.to_vec(), level1.to_vec()],
                b"k".to_vec(),
                Element::new_item(b"value_k".to_vec()),
            ),
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![level0.to_vec(), level1.to_vec()],
                b"l".to_vec(),
                Element::new_item(b"value_l".to_vec()),
            ),
            // Add 2 items to level0
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![level0.to_vec()],
                b"m".to_vec(),
                Element::new_item(b"value_m".to_vec()),
            ),
            QualifiedGroveDbOp::insert_or_replace_op(
                vec![level0.to_vec()],
                b"n".to_vec(),
                Element::new_item(b"value_n".to_vec()),
            ),
        ];

        println!("Batch operations:");
        println!("  - level2: +i, +j (1 -> 3)");
        println!("  - level1: +k, +l (3 -> 7, because level2 now contributes 3)");
        println!("  - level0: +m, +n (8 -> 14, because level1 now contributes 7)");
        println!();

        db.apply_batch(ops, None, None, grove_version)
            .unwrap()
            .expect("apply batch");

        println!("Batch applied successfully!\n");

        // =================================================================
        // PHASE 3: Verify counts after batch
        // =================================================================
        println!("======================================================================");
        println!("PHASE 3: Verifying counts after batch operation");
        println!("======================================================================\n");

        // level2 should now have count 3
        let level2_elem = db
            .get(
                &[level0.as_slice(), level1.as_slice()],
                level2,
                None,
                grove_version,
            )
            .unwrap()
            .expect("get level2");

        match &level2_elem {
            Element::ProvableCountTree(_, count, _) => {
                println!("level2 count: {} (expected: 3)", count);
                assert_eq!(*count, 3, "level2 should have count 3 after batch");
            }
            _ => panic!("Expected ProvableCountTree for level2"),
        }

        // level1 should now have count 7 (f + g + k + l + level2=3)
        let level1_elem = db
            .get(&[level0.as_slice()], level1, None, grove_version)
            .unwrap()
            .expect("get level1");

        match &level1_elem {
            Element::ProvableCountTree(_, count, _) => {
                println!("level1 count: {} (expected: 7)", count);
                assert_eq!(*count, 7, "level1 should have count 7 after batch");
            }
            _ => panic!("Expected ProvableCountTree for level1"),
        }

        // level0 should now have count 14 (a+b+c+d+e+m+n + level1=7)
        let level0_elem = db
            .get(&[] as &[&[u8]], level0, None, grove_version)
            .unwrap()
            .expect("get level0");

        match &level0_elem {
            Element::ProvableCountTree(_, count, _) => {
                println!("level0 count: {} (expected: 14)", count);
                assert_eq!(*count, 14, "level0 should have count 14 after batch");
            }
            _ => panic!("Expected ProvableCountTree for level0"),
        }

        let root_hash_after = db.root_hash(None, grove_version).unwrap().unwrap();
        println!("\nRoot hash after batch: {}", hex::encode(root_hash_after));

        // Verify root hash changed
        assert_ne!(
            root_hash_before, root_hash_after,
            "Root hash should change after batch"
        );

        // =================================================================
        // PHASE 4: Verify proofs work correctly at all levels
        // =================================================================
        println!("\n======================================================================");
        println!("PHASE 4: Verifying proofs at all nesting levels");
        println!("======================================================================\n");

        // Query and prove items at level0
        let query_level0 = PathQuery::new(
            vec![level0.to_vec()],
            SizedQuery::new(
                Query::new_single_query_item(QueryItem::RangeFull(..)),
                None,
                None,
            ),
        );

        let proof_level0 = db
            .prove_query(&query_level0, None, grove_version)
            .unwrap()
            .expect("prove level0");

        let (hash_level0, tree_type_level0, results_level0) =
            GroveDb::verify_query_get_parent_tree_info(&proof_level0, &query_level0, grove_version)
                .expect("verify level0");

        println!("Level0 query results: {} elements", results_level0.len());
        match tree_type_level0 {
            TreeFeatureType::ProvableCountedMerkNode(count) => {
                println!("Level0 proof tree type: ProvableCountedMerkNode({})", count);
                assert_eq!(count, 14, "Proof should show count 14 at level0");
            }
            _ => panic!(
                "Expected ProvableCountedMerkNode at level0, got {:?}",
                tree_type_level0
            ),
        }
        assert_eq!(
            hash_level0, root_hash_after,
            "Level0 proof root hash mismatch"
        );

        // Query and prove items at level1
        let query_level1 = PathQuery::new(
            vec![level0.to_vec(), level1.to_vec()],
            SizedQuery::new(
                Query::new_single_query_item(QueryItem::RangeFull(..)),
                None,
                None,
            ),
        );

        let proof_level1 = db
            .prove_query(&query_level1, None, grove_version)
            .unwrap()
            .expect("prove level1");

        let (hash_level1, tree_type_level1, results_level1) =
            GroveDb::verify_query_get_parent_tree_info(&proof_level1, &query_level1, grove_version)
                .expect("verify level1");

        println!("Level1 query results: {} elements", results_level1.len());
        match tree_type_level1 {
            TreeFeatureType::ProvableCountedMerkNode(count) => {
                println!("Level1 proof tree type: ProvableCountedMerkNode({})", count);
                assert_eq!(count, 7, "Proof should show count 7 at level1");
            }
            _ => panic!(
                "Expected ProvableCountedMerkNode at level1, got {:?}",
                tree_type_level1
            ),
        }
        assert_eq!(
            hash_level1, root_hash_after,
            "Level1 proof root hash mismatch"
        );

        // Query and prove items at level2
        let query_level2 = PathQuery::new(
            vec![level0.to_vec(), level1.to_vec(), level2.to_vec()],
            SizedQuery::new(
                Query::new_single_query_item(QueryItem::RangeFull(..)),
                None,
                None,
            ),
        );

        let proof_level2 = db
            .prove_query(&query_level2, None, grove_version)
            .unwrap()
            .expect("prove level2");

        let (hash_level2, tree_type_level2, results_level2) =
            GroveDb::verify_query_get_parent_tree_info(&proof_level2, &query_level2, grove_version)
                .expect("verify level2");

        println!("Level2 query results: {} elements", results_level2.len());
        match tree_type_level2 {
            TreeFeatureType::ProvableCountedMerkNode(count) => {
                println!("Level2 proof tree type: ProvableCountedMerkNode({})", count);
                assert_eq!(count, 3, "Proof should show count 3 at level2");
            }
            _ => panic!(
                "Expected ProvableCountedMerkNode at level2, got {:?}",
                tree_type_level2
            ),
        }
        assert_eq!(
            hash_level2, root_hash_after,
            "Level2 proof root hash mismatch"
        );

        // =================================================================
        // PHASE 5: Query specific items to verify content
        // =================================================================
        println!("\n======================================================================");
        println!("PHASE 5: Verifying specific items at each level");
        println!("======================================================================\n");

        // Verify items at level2
        let items_level2 = vec![b"h", b"i", b"j"];
        for key in items_level2 {
            let item = db
                .get(
                    &[level0.as_slice(), level1.as_slice(), level2.as_slice()],
                    key,
                    None,
                    grove_version,
                )
                .unwrap()
                .expect("get item");
            match item {
                Element::Item(value, _) => {
                    println!(
                        "  level2/{}: {}",
                        String::from_utf8_lossy(key),
                        String::from_utf8_lossy(&value)
                    );
                }
                _ => panic!("Expected Item"),
            }
        }

        // Verify items at level1
        let items_level1 = vec![b"f", b"g", b"k", b"l"];
        for key in items_level1 {
            let item = db
                .get(
                    &[level0.as_slice(), level1.as_slice()],
                    key,
                    None,
                    grove_version,
                )
                .unwrap()
                .expect("get item");
            match item {
                Element::Item(value, _) => {
                    println!(
                        "  level1/{}: {}",
                        String::from_utf8_lossy(key),
                        String::from_utf8_lossy(&value)
                    );
                }
                _ => panic!("Expected Item"),
            }
        }

        // Verify items at level0
        let items_level0 = vec![b"a", b"b", b"c", b"d", b"e", b"m", b"n"];
        for key in items_level0 {
            let item = db
                .get(&[level0.as_slice()], key, None, grove_version)
                .unwrap()
                .expect("get item");
            match item {
                Element::Item(value, _) => {
                    println!(
                        "  level0/{}: {}",
                        String::from_utf8_lossy(key),
                        String::from_utf8_lossy(&value)
                    );
                }
                _ => panic!("Expected Item"),
            }
        }

        println!("\n======================================================================");
        println!("SUCCESS: Nested ProvableCountTrees with batch operations work correctly!");
        println!("======================================================================\n");

        println!("Summary:");
        println!("  - 3 levels of nested ProvableCountTrees");
        println!("  - Batch operation modified all 3 levels simultaneously");
        println!("  - Counts propagate correctly through all levels");
        println!("  - Proofs verify correctly at each level");
        println!("  - Root hash changes appropriately with batch operations");
    }
}
