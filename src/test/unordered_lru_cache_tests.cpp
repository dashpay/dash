// Copyright (c) 2024 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <unordered_lru_cache.h>

#include <boost/test/unit_test.hpp>

struct IntHasher {
    size_t operator()(int v) const noexcept {
        return std::hash<int>{}(v);
    }
};

BOOST_AUTO_TEST_SUITE(unordered_lru_cache_tests)

BOOST_AUTO_TEST_CASE(construction_and_configuration)
{
    // Runtime-sized cache with default truncateThreshold
    unordered_lru_cache<int, int, IntHasher> c1(5);
    BOOST_CHECK_EQUAL(c1.max_size(), 5U);
    
    // Custom truncateThreshold
    unordered_lru_cache<int, int, IntHasher> c2(5, 10);
    BOOST_CHECK_EQUAL(c2.max_size(), 5U);
    
    // truncateThreshold less than maxSize
    unordered_lru_cache<int, int, IntHasher> c3(10, 5);
    BOOST_CHECK_EQUAL(c3.max_size(), 10U);
    
    // truncateThreshold equal to maxSize
    unordered_lru_cache<int, int, IntHasher> c4(5, 5);
    BOOST_CHECK_EQUAL(c4.max_size(), 5U);
}

BOOST_AUTO_TEST_CASE(compile_time_maxsize)
{
    // Compile-time MaxSize template parameter
    unordered_lru_cache<int, int, IntHasher, 10> c;
    BOOST_CHECK_EQUAL(c.max_size(), 10U);
}

BOOST_AUTO_TEST_CASE(basic_insert_and_get)
{
    unordered_lru_cache<int, int, IntHasher> c(10);
    
    // Insert new key
    c.insert(1, 10);
    auto opt = c.get(1);
    BOOST_CHECK(opt.has_value());
    BOOST_CHECK_EQUAL(opt.value(), 10);
    
    // Get non-existent key
    auto opt2 = c.get(2);
    BOOST_CHECK(!opt2.has_value());
    
    // Insert another key
    c.insert(2, 20);
    opt2 = c.get(2);
    BOOST_CHECK(opt2.has_value());
    BOOST_CHECK_EQUAL(opt2.value(), 20);
}

BOOST_AUTO_TEST_CASE(emplace_behavior)
{
    unordered_lru_cache<int, int, IntHasher> c(10);
    
    // Emplace new key
    c.emplace(1, 10);
    auto opt = c.get(1);
    BOOST_CHECK(opt.has_value());
    BOOST_CHECK_EQUAL(opt.value(), 10);
    
    // Emplace existing key updates value
    c.emplace(1, 15);
    opt = c.get(1);
    BOOST_CHECK(opt.has_value());
    BOOST_CHECK_EQUAL(opt.value(), 15);
}

BOOST_AUTO_TEST_CASE(exists_touches_recency)
{
    unordered_lru_cache<int, int, IntHasher> c(3, 5);
    
    // Insert keys 1-5
    for (int i = 1; i <= 5; ++i) {
        c.insert(i, i * 10);
    }
    
    // Touch key 1 via exists
    BOOST_CHECK(c.exists(1));
    
    // Insert key 6, which should trigger truncation
    // Key 1 should survive because it was touched
    c.insert(6, 60);
    
    BOOST_CHECK(c.exists(1)); // Key 1 should still exist
    BOOST_CHECK(!c.exists(2)); // Key 2 should be evicted
}

BOOST_AUTO_TEST_CASE(erase_behavior)
{
    unordered_lru_cache<int, int, IntHasher> c(10);
    
    c.insert(1, 10);
    BOOST_CHECK(c.exists(1));
    
    c.erase(1);
    BOOST_CHECK(!c.exists(1));
    BOOST_CHECK(!c.get(1).has_value());
    
    // Erasing non-existent key is a no-op
    c.erase(999);
}

BOOST_AUTO_TEST_CASE(clear_empties_cache)
{
    unordered_lru_cache<int, int, IntHasher> c(10);
    
    for (int i = 1; i <= 5; ++i) {
        c.insert(i, i * 10);
    }
    
    c.clear();
    
    for (int i = 1; i <= 5; ++i) {
        BOOST_CHECK(!c.exists(i));
        BOOST_CHECK(!c.get(i).has_value());
    }
}

BOOST_AUTO_TEST_CASE(get_returns_copy)
{
    unordered_lru_cache<int, int, IntHasher> c(10);
    
    c.insert(1, 10);
    auto opt = c.get(1);
    BOOST_REQUIRE(opt.has_value());
    
    // Mutate returned value
    opt.value() = 99;
    
    // Original value should be unchanged
    auto opt2 = c.get(1);
    BOOST_CHECK(opt2.has_value());
    BOOST_CHECK_EQUAL(opt2.value(), 10);
}

BOOST_AUTO_TEST_CASE(no_truncation_below_threshold)
{
    unordered_lru_cache<int, int, IntHasher> c(5, 10);
    
    // Insert up to threshold
    for (int i = 1; i <= 10; ++i) {
        c.insert(i, i * 10);
    }
    
    // All keys should still exist
    for (int i = 1; i <= 10; ++i) {
        BOOST_CHECK(c.exists(i));
    }
}

BOOST_AUTO_TEST_CASE(truncation_on_first_overflow)
{
    unordered_lru_cache<int, int, IntHasher> c(5, 10);
    
    // Insert up to threshold
    for (int i = 1; i <= 10; ++i) {
        c.insert(i, i * 10);
    }
    
    // Insert one more to trigger truncation
    c.insert(11, 110);
    
    // Should have exactly maxSize entries
    int count = 0;
    for (int i = 1; i <= 11; ++i) {
        if (c.exists(i)) {
            count++;
        }
    }
    BOOST_CHECK_EQUAL(count, 5U);
    
    // Most recent entries should survive
    BOOST_CHECK(c.exists(11)); // Most recent
    BOOST_CHECK(c.exists(10));
    BOOST_CHECK(c.exists(9));
    BOOST_CHECK(c.exists(8));
    BOOST_CHECK(c.exists(7));
}

BOOST_AUTO_TEST_CASE(sequential_inserts_preserve_most_recent)
{
    unordered_lru_cache<int, int, IntHasher> c(3, 5);
    
    // Insert 5 items
    for (int i = 1; i <= 5; ++i) {
        c.insert(i, i * 10);
    }
    
    // Insert 6th item triggers truncation
    c.insert(6, 60);
    
    // Last 3 inserted should survive (4, 5, 6)
    BOOST_CHECK(!c.exists(1));
    BOOST_CHECK(!c.exists(2));
    BOOST_CHECK(!c.exists(3));
    BOOST_CHECK(c.exists(4));
    BOOST_CHECK(c.exists(5));
    BOOST_CHECK(c.exists(6));
}

BOOST_AUTO_TEST_CASE(touching_older_entry_preserves_it)
{
    unordered_lru_cache<int, int, IntHasher> c(3, 5);
    
    // Insert 5 items
    for (int i = 1; i <= 5; ++i) {
        c.insert(i, i * 10);
    }
    
    // Touch key 2 (older entry)
    c.get(2);
    
    // Insert 6th item triggers truncation
    c.insert(6, 60);
    
    // Key 2 should survive because it was touched (most recent before 6)
    BOOST_CHECK(c.exists(2));
    // Keys 5 and 6 should also survive (most recent)
    BOOST_CHECK(c.exists(5));
    BOOST_CHECK(c.exists(6));
    
    // Keys 1, 3, and 4 should be evicted
    BOOST_CHECK(!c.exists(1));
    BOOST_CHECK(!c.exists(3));
    BOOST_CHECK(!c.exists(4));
}

BOOST_AUTO_TEST_CASE(reinserting_updates_recency)
{
    unordered_lru_cache<int, int, IntHasher> c(3, 5);
    
    // Insert 5 items
    for (int i = 1; i <= 5; ++i) {
        c.insert(i, i * 10);
    }
    
    // Re-insert key 1 (oldest)
    c.insert(1, 100);
    
    // Insert 6th item triggers truncation
    c.insert(6, 60);
    
    // Key 1 should survive because it was re-inserted
    BOOST_CHECK(c.exists(1));
    BOOST_CHECK_EQUAL(c.get(1).value(), 100); // Value should be updated
    
    // Keys 5 and 6 should also survive
    BOOST_CHECK(c.exists(5));
    BOOST_CHECK(c.exists(6));
    
    // Keys 2, 3, 4 should be evicted
    BOOST_CHECK(!c.exists(2));
    BOOST_CHECK(!c.exists(3));
    BOOST_CHECK(!c.exists(4));
}

BOOST_AUTO_TEST_CASE(multiple_truncation_triggers)
{
    unordered_lru_cache<int, int, IntHasher> c(3, 5);
    
    // Insert 5 items
    for (int i = 1; i <= 5; ++i) {
        c.insert(i, i * 10);
    }
    
    // Insert 6th item triggers truncation (6 > threshold 5)
    c.insert(6, 60);
    
    // After truncation, we have 3 items. Insert more to exceed threshold again
    c.insert(7, 70);
    c.insert(8, 80);
    c.insert(9, 90);
    // Now we have 6 items (after inserting 7, 8, 9), which exceeds threshold 5
    // This triggers truncation back to 3
    
    // Should have exactly 3 entries
    int count = 0;
    for (int i = 1; i <= 9; ++i) {
        if (c.exists(i)) {
            count++;
        }
    }
    BOOST_CHECK_EQUAL(count, 3U);
    
    // Most recent 3 should survive (7, 8, 9)
    BOOST_CHECK(c.exists(7));
    BOOST_CHECK(c.exists(8));
    BOOST_CHECK(c.exists(9));
}

BOOST_AUTO_TEST_CASE(edge_case_maxsize_one)
{
    unordered_lru_cache<int, int, IntHasher> c(1, 2);
    
    // Insert first item
    c.insert(1, 10);
    BOOST_CHECK(c.exists(1));
    
    // Insert second item (no truncation yet)
    c.insert(2, 20);
    BOOST_CHECK(c.exists(1));
    BOOST_CHECK(c.exists(2));
    
    // Insert third item triggers truncation
    c.insert(3, 30);
    BOOST_CHECK(!c.exists(1));
    BOOST_CHECK(!c.exists(2));
    BOOST_CHECK(c.exists(3));
    
    // Get preserves the only entry
    c.get(3);
    BOOST_CHECK(c.exists(3));
}

BOOST_AUTO_TEST_CASE(edge_case_threshold_equals_maxsize)
{
    unordered_lru_cache<int, int, IntHasher> c(5, 5);
    
    // Insert 5 items (no truncation yet)
    for (int i = 1; i <= 5; ++i) {
        c.insert(i, i * 10);
    }
    
    // All should exist
    for (int i = 1; i <= 5; ++i) {
        BOOST_CHECK(c.exists(i));
    }
    
    // Insert 6th item triggers immediate truncation
    c.insert(6, 60);
    
    // Should have exactly 5 entries
    int count = 0;
    for (int i = 1; i <= 6; ++i) {
        if (c.exists(i)) {
            count++;
        }
    }
    BOOST_CHECK_EQUAL(count, 5U);
    
    // Last 5 should survive (2, 3, 4, 5, 6)
    BOOST_CHECK(!c.exists(1));
    BOOST_CHECK(c.exists(2));
    BOOST_CHECK(c.exists(3));
    BOOST_CHECK(c.exists(4));
    BOOST_CHECK(c.exists(5));
    BOOST_CHECK(c.exists(6));
}

BOOST_AUTO_TEST_CASE(edge_case_threshold_less_than_maxsize)
{
    unordered_lru_cache<int, int, IntHasher> c(10, 5);
    
    // Insert 5 items (no truncation yet)
    for (int i = 1; i <= 5; ++i) {
        c.insert(i, i * 10);
    }
    
    // All should exist
    for (int i = 1; i <= 5; ++i) {
        BOOST_CHECK(c.exists(i));
    }
    
    // Insert 6th item triggers truncation
    c.insert(6, 60);
    
    // Should truncate to maxSize (10), but we only have 6 items
    // So all should still exist
    for (int i = 1; i <= 6; ++i) {
        BOOST_CHECK(c.exists(i));
    }
    
    // Insert more items to exceed maxSize
    for (int i = 7; i <= 15; ++i) {
        c.insert(i, i * 10);
    }
    
    // Should have exactly 10 entries
    int count = 0;
    for (int i = 1; i <= 15; ++i) {
        if (c.exists(i)) {
            count++;
        }
    }
    BOOST_CHECK_EQUAL(count, 10U);
}

BOOST_AUTO_TEST_SUITE_END()

