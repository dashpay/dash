#include <wallet/wallet.h>
#include <wallet/test/wallet_test_fixture.h>

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <vector>
#include <map>
#include <util/string.h>

namespace wallet {

// Forward declaration of the function we want to test (will be implemented in wallet.cpp)
std::vector<fs::path> GetBackupsToDelete(const std::multimap<fs::file_time_type, fs::path>& backups, int nWalletBackups, int maxBackups);

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

    // Case 3: More than 10, all very recent
    // Logic: INDEX-BASED retention (not time-based)
    // Keep the latest 10 (indices 0-9 when sorted newest first)
    // Then keep backups at ranks that are powers of 2: 16th, 32nd, 64th, etc.
    // (i.e., indices 15, 31, 63, ... when 0-indexed)
    // If we have 20 backups all created seconds apart, the algorithm doesn't care about time.
    // It just keeps: indices 0-9 (latest 10), then index 15 (16th backup)
    // All others are deleted.

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
    // Should keep latest 10 (indices 0-9 in descending sorted order = i=10 to 19).
    // Then keep index 15 (16th backup = i=4).
    // i=0 to 3 and i=5 to 9 are deleted (9 backups deleted).
    // Total kept: 11, deleted: 9.
    BOOST_CHECK_EQUAL(to_delete.size(), 9);

    // Case 4: Index-based exponential distribution
    backups.clear();
    // Create 18 backups with varied ages (not that it matters for index-based logic)
    // After sorting newest first, we get indices 0-17
    // Keep: indices 0-9 (latest 10), index 15 (16th backup)
    // Delete: indices 10-14, 16-17 (7 total)

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
    // Keep indices: 0-9 (latest 10), 15 (16th backup, power of 2)
    // Delete: 10-14, 16-17 = 7 files
    BOOST_CHECK_EQUAL(to_delete.size(), 7);
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

    // Test INDEX-BASED exponential retention:
    // - Keep latest nWalletBackups (10) backups
    // - For older backups, keep those at ranks that are powers of 2 (16th, 32nd, 64th, etc.)
    // - Enforce hard max limit (maxBackups)
    //
    // With 100 backups and nWalletBackups=10:
    // Keep indices: 0-9 (latest 10), 15 (16th), 31 (32nd), 63 (64th)
    // Total kept: 13, deleted: 87

    backups.clear();
    for (int i = 0; i < 100; ++i) {
        backups.insert({make_time(1000 - i), make_path(i)});
    }

    // GetBackupsToDelete should return everything EXCEPT:
    // Indices 0-9 (latest 10), 15 (16th), 31 (32nd), 63 (64th)
    // Total kept: 13, deleted: 87

    auto to_delete = GetBackupsToDelete(backups, 10, 50);
    BOOST_CHECK_EQUAL(to_delete.size(), 87);
}

BOOST_AUTO_TEST_CASE(hard_max_limit_caps_retention)
{
    auto make_time = [](int64_t seconds_ago) {
        return fs::file_time_type::clock::now() - std::chrono::seconds(seconds_ago);
    };
    auto make_path = [](int i) {
        return fs::u8path("backup_" + ToString(i) + ".dat");
    };

    std::multimap<fs::file_time_type, fs::path> backups;

    // Test that maxBackups hard cap limits retention when exponential/index-based
    // retention would keep more than maxBackups. With 50 backups and nWalletBackups=10,
    // exponential logic would keep indices 0-9 (latest 10), 15 (16th), 31 (32nd) = 13 backups.
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
