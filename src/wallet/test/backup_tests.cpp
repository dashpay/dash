#include <wallet/wallet.h>
#include <wallet/test/wallet_test_fixture.h>

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <vector>
#include <map>
#include <util/string.h>

namespace wallet {

BOOST_FIXTURE_TEST_SUITE(backup_tests, WalletTestingSetup)

BOOST_AUTO_TEST_CASE(exponential_backup_logic)
{
    // Helper to create a dummy timestamp
    auto make_time = [](int64_t seconds_ago) {
        return fs::file_time_type{std::chrono::file_clock::time_point{std::chrono::seconds(seconds_ago)}};
    };

    // Helper to create a dummy path
    auto make_path = [](int i) {
        return fs::u8path("backup_" + ToString(i) + ".dat");
    };

    std::multimap<fs::file_time_type, fs::path> backups;

    // Case 1: Less than nWalletBackups (10)
    for (int i = 0; i < 5; ++i) {
        backups.insert({make_time(i * 100), make_path(i)});
    }
    auto to_delete = GetBackupsToDelete(backups, 10, 50);
    BOOST_CHECK(to_delete.empty());

    // Case 2: Exactly nWalletBackups (10)
    backups.clear();
    for (int i = 0; i < 10; ++i) {
        backups.insert({make_time(i * 100), make_path(i)});
    }
    to_delete = GetBackupsToDelete(backups, 10, 50);
    BOOST_CHECK(to_delete.empty());

    // Case 2b: 11 backups - the critical edge case that proves exponential retention works
    // Before the fix, the 11th backup would always be deleted, never allowing accumulation.
    // With the fix, we keep the oldest in range [10,16), so index 10 is kept.
    backups.clear();
    for (int i = 0; i < 11; ++i) {
        backups.insert({make_time(i * 100), make_path(i)});
    }
    to_delete = GetBackupsToDelete(backups, 10, 50);
    // Keep indices 0-9 (latest 10) + index 10 (oldest in [10,16)) = 11 total
    // Nothing to delete!
    BOOST_CHECK(to_delete.empty());

    // Case 2c: 12 backups - now we start deleting
    backups.clear();
    for (int i = 0; i < 12; ++i) {
        backups.insert({make_time(i * 100), make_path(i)});
    }
    to_delete = GetBackupsToDelete(backups, 10, 50);
    // Keep indices 0-9 (latest 10) + index 11 (oldest in [10,16)) = 11 total
    // Delete index 10
    BOOST_CHECK_EQUAL(to_delete.size(), 1);

    // Case 3: More than 10, all very recent
    // Logic: INDEX-BASED retention with exponential ranges
    // Keep the latest 10 (indices 0-9 when sorted newest first)
    // For older backups, keep the oldest in each exponential range:
    //   [10,16): keep index 15 (or highest available)
    //   [16,32): keep index 19 (highest available, capped by size-1)
    // Total kept: 12 (indices 0-9, 15, 19), deleted: 8

    backups.clear();
    for (int i = 0; i < 20; ++i) {
        // 20 backups, 1 second apart.
        // 0 is oldest, 19 is newest.
        backups.insert({make_time(200 - i * 10), make_path(i)});
    }
    // Times:
    // i=0: 200s ago (Oldest)
    // i=19: 10s ago (Newest)

    to_delete = GetBackupsToDelete(backups, 10, 50);
    // Keep: indices 0-9 (latest 10), 15 (oldest in [10,16)), 19 (oldest in [16,32))
    // Delete: indices 10-14, 16-18 (8 backups deleted).
    // Total kept: 12, deleted: 8.
    BOOST_CHECK_EQUAL(to_delete.size(), 8);

    // Case 4: Index-based exponential distribution
    backups.clear();
    // Create 18 backups with varied ages (not that it matters for index-based logic)
    // After sorting newest first, we get indices 0-17
    // Keep: indices 0-9 (latest 10), 15 (oldest in [10,16)), 17 (oldest in [16,32))
    // Delete: indices 10-14, 16 (6 total)

    std::vector<int64_t> ages;
    for(int i=0; i<10; ++i) ages.push_back(i * 3600); // 0 to 9 hours
    ages.push_back(24 * 3600); // 1 day
    ages.push_back(2 * 24 * 3600); // 2 days
    ages.push_back(4 * 24 * 3600); // 4 days
    ages.push_back(8 * 24 * 3600); // 8 days
    ages.push_back(16 * 24 * 3600); // 16 days
    ages.push_back(32 * 24 * 3600); // 32 days
    ages.push_back(3 * 24 * 3600); // 3 days
    ages.push_back(5 * 24 * 3600); // 5 days

    int id = 0;
    for (auto age : ages) {
        backups.insert({make_time(age), make_path(id++)});
    }

    to_delete = GetBackupsToDelete(backups, 10, 50);

    // Total files: 18
    // After sorting by time (newest first), we have indices 0-17
    // Keep indices: 0-9 (latest 10), 15 (oldest in [10,16)), 17 (oldest in [16,32))
    // Delete: 10-14, 16 = 6 files
    BOOST_CHECK_EQUAL(to_delete.size(), 6);
}

BOOST_AUTO_TEST_CASE(hard_max_limit)
{
    auto make_time = [](int64_t seconds_ago) {
        return fs::file_time_type{std::chrono::file_clock::time_point{std::chrono::seconds(seconds_ago)}};
    };
    auto make_path = [](int i) {
        return fs::u8path("backup_" + ToString(i) + ".dat");
    };

    std::multimap<fs::file_time_type, fs::path> backups;

    // Test INDEX-BASED exponential retention with ranges:
    // - Keep latest nWalletBackups (10) backups
    // - For older backups, keep oldest in each exponential range
    // - Enforce hard max limit (maxBackups)
    //
    // With 100 backups and nWalletBackups=10:
    // Keep indices: 0-9 (latest 10), 15 (oldest in [10,16)), 31 (oldest in [16,32)),
    //               63 (oldest in [32,64)), 99 (oldest in [64,128))
    // Total kept: 14, deleted: 86

    backups.clear();
    for (int i = 0; i < 100; ++i) {
        backups.insert({make_time(1000 - i), make_path(i)});
    }

    // GetBackupsToDelete should return everything EXCEPT:
    // Indices 0-9, 15, 31, 63, 99
    // Total kept: 14, deleted: 86

    auto to_delete = GetBackupsToDelete(backups, 10, 50);
    BOOST_CHECK_EQUAL(to_delete.size(), 86);
}

BOOST_AUTO_TEST_CASE(hard_max_limit_caps_retention)
{
    auto make_time = [](int64_t seconds_ago) {
        return fs::file_time_type{std::chrono::file_clock::time_point{std::chrono::seconds(seconds_ago)}};
    };
    auto make_path = [](int i) {
        return fs::u8path("backup_" + ToString(i) + ".dat");
    };

    std::multimap<fs::file_time_type, fs::path> backups;

    // Test that maxBackups hard cap limits retention when exponential/index-based
    // retention would keep more than maxBackups. With 50 backups and nWalletBackups=10,
    // exponential logic would keep indices 0-9 (latest 10), 15, 31, 49 = 13 backups.
    // But with maxBackups=12, the hard cap should limit kept backups to 12, resulting in 38 deletions.

    backups.clear();
    for (int i = 0; i < 50; ++i) {
        backups.insert({make_time(1000 - i), make_path(i)});
    }

    auto to_delete = GetBackupsToDelete(backups, 10, 12);
    BOOST_CHECK_EQUAL(to_delete.size(), 38); // 50 - 12 = 38
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace wallet
