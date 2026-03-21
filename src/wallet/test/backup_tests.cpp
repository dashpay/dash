#include <wallet/wallet.h>
#include <wallet/test/wallet_test_fixture.h>

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <vector>
#include <map>
#include <util/string.h>

namespace wallet {

BOOST_FIXTURE_TEST_SUITE(backup_tests, WalletTestingSetup)

BOOST_AUTO_TEST_CASE(time_based_exponential_retention)
{
    // Capture "now" once so all timestamps are relative to the same point
    auto now = std::chrono::file_clock::now();

    // Helper to create a timestamp N days ago from the captured "now"
    auto make_time = [&now](int64_t days_ago) {
        auto days_duration = std::chrono::duration<int64_t, std::ratio<86400>>(days_ago);
        return fs::file_time_type{now - days_duration};
    };

    // Helper to create a dummy path
    auto make_path = [](int i) {
        return fs::u8path("backup_" + ToString(i) + ".dat");
    };

    std::multimap<fs::file_time_type, fs::path> backups;

    // Case 1: Less than nWalletBackups (10)
    for (int i = 0; i < 5; ++i) {
        backups.insert({make_time(i), make_path(i)});
    }
    auto to_delete = GetBackupsToDelete(backups, 10, 50);
    BOOST_CHECK(to_delete.empty());

    // Case 2: Exactly nWalletBackups (10)
    backups.clear();
    for (int i = 0; i < 10; ++i) {
        backups.insert({make_time(i), make_path(i)});
    }
    to_delete = GetBackupsToDelete(backups, 10, 50);
    BOOST_CHECK(to_delete.empty());

    // Case 3: 11 backups - all recent (< 1 day old)
    // Since all are < 1 day old, no time-based retention applies
    // Keep latest 10, but the 11th is also < 1 day so it doesn't get kept
    backups.clear();
    for (int i = 0; i < 11; ++i) {
        backups.insert({make_time(0), make_path(i)});
    }
    to_delete = GetBackupsToDelete(backups, 10, 50);
    // All backups are 0 days old, so none fall into [1,2) or later ranges
    // Keep only latest 10, delete 1
    BOOST_CHECK_EQUAL(to_delete.size(), 1);

    // Case 4: 20 backups spanning multiple days
    // Latest 10: 0 days old
    // Older backups: 1, 2, 3, 5, 7, 10, 15, 20, 25, 30 days old
    backups.clear();
    for (int i = 0; i < 10; ++i) {
        backups.insert({make_time(0), make_path(i)});
    }
    backups.insert({make_time(1), make_path(10)});   // [1,2) days
    backups.insert({make_time(2), make_path(11)});   // [2,4) days
    backups.insert({make_time(3), make_path(12)});   // [2,4) days
    backups.insert({make_time(5), make_path(13)});   // [4,8) days
    backups.insert({make_time(7), make_path(14)});   // [4,8) days
    backups.insert({make_time(10), make_path(15)});  // [8,16) days
    backups.insert({make_time(15), make_path(16)});  // [8,16) days
    backups.insert({make_time(20), make_path(17)});  // [16,32) days
    backups.insert({make_time(25), make_path(18)});  // [16,32) days
    backups.insert({make_time(30), make_path(19)});  // [16,32) days

    to_delete = GetBackupsToDelete(backups, 10, 50);

    // Should keep:
    // - Latest 10 (by count, indices 0-9): backup_0 through backup_9
    // - Oldest in [1,2): backup_10 (1 day)
    // - Oldest in [2,4): backup_12 (3 days)
    // - Oldest in [4,8): backup_14 (7 days)
    // - Oldest in [8,16): backup_16 (15 days)
    // - Oldest in [16,32): backup_19 (30 days)
    // Total: 15 kept, 5 deleted
    BOOST_CHECK_EQUAL(to_delete.size(), 5);

    // Verify exactly which backups are deleted
    std::set<std::string> expected_deletions = {
        "backup_11.dat",  // 2 days, not oldest in [2,4)
        "backup_13.dat",  // 5 days, not oldest in [4,8)
        "backup_15.dat",  // 10 days, not oldest in [8,16)
        "backup_17.dat",  // 20 days, not oldest in [16,32)
        "backup_18.dat"   // 25 days, not oldest in [16,32)
    };
    std::set<std::string> actual_deletions;
    for (const auto& path : to_delete) {
        actual_deletions.insert(fs::PathToString(path.filename()));
    }
    BOOST_CHECK(expected_deletions == actual_deletions);

    // Case 5: Test that we accumulate over time
    // Simulate 100 days of daily backups
    backups.clear();
    for (int i = 0; i < 100; ++i) {
        backups.insert({make_time(i), make_path(i)});
    }

    to_delete = GetBackupsToDelete(backups, 10, 50);

    // Should keep:
    // - Latest 10 (by count): backup_0 through backup_9
    // - Oldest in [1,2): backup_1 is 1 day (already in latest 10)
    // - Oldest in [2,4): backup_3 is 3 days (already in latest 10)
    // - Oldest in [4,8): backup_7 is 7 days (already in latest 10)
    // - Oldest in [8,16): backup_15 (15 days)
    // - Oldest in [16,32): backup_31 (31 days)
    // - Oldest in [32,64): backup_63 (63 days)
    // - Oldest in [64,128): backup_99 (99 days)
    // Total: 14 kept, 86 deleted
    BOOST_CHECK_EQUAL(to_delete.size(), 86);

    // Verify specific kept backups in exponential ranges
    std::set<std::string> expected_kept = {
        "backup_0.dat", "backup_1.dat", "backup_2.dat", "backup_3.dat", "backup_4.dat",
        "backup_5.dat", "backup_6.dat", "backup_7.dat", "backup_8.dat", "backup_9.dat",
        "backup_15.dat", "backup_31.dat", "backup_63.dat", "backup_99.dat"
    };
    std::set<std::string> actual_kept;
    for (const auto& [time, path] : backups) {
        if (std::find(to_delete.begin(), to_delete.end(), path) == to_delete.end()) {
            actual_kept.insert(fs::PathToString(path.filename()));
        }
    }
    BOOST_CHECK(expected_kept == actual_kept);
}

BOOST_AUTO_TEST_CASE(hard_max_limit)
{
    auto now = std::chrono::file_clock::now();
    auto make_time = [&now](int64_t days_ago) {
        auto days_duration = std::chrono::duration<int64_t, std::ratio<86400>>(days_ago);
        return fs::file_time_type{now - days_duration};
    };
    auto make_path = [](int i) {
        return fs::u8path("backup_" + ToString(i) + ".dat");
    };

    std::multimap<fs::file_time_type, fs::path> backups;

    // Create 100 daily backups and set maxBackups=15
    for (int i = 0; i < 100; ++i) {
        backups.insert({make_time(i), make_path(i)});
    }

    auto to_delete = GetBackupsToDelete(backups, 10, 15);

    // Without maxBackups limit, we'd keep 14 backups (see Case 5 above)
    // With maxBackups=15, we still keep 14 (under the limit)
    BOOST_CHECK_EQUAL(to_delete.size(), 86);

    // Verify same backups kept as in Case 5
    std::set<std::string> expected_kept_15 = {
        "backup_0.dat", "backup_1.dat", "backup_2.dat", "backup_3.dat", "backup_4.dat",
        "backup_5.dat", "backup_6.dat", "backup_7.dat", "backup_8.dat", "backup_9.dat",
        "backup_15.dat", "backup_31.dat", "backup_63.dat", "backup_99.dat"
    };
    std::set<std::string> actual_kept_15;
    for (const auto& [time, path] : backups) {
        if (std::find(to_delete.begin(), to_delete.end(), path) == to_delete.end()) {
            actual_kept_15.insert(fs::PathToString(path.filename()));
        }
    }
    BOOST_CHECK(expected_kept_15 == actual_kept_15);

    // Now test with maxBackups=12 (less than natural retention)
    to_delete = GetBackupsToDelete(backups, 10, 12);

    // Should cap at 12 backups: keep latest 10 + 2 oldest time ranges
    // Total: 12 kept, 88 deleted
    BOOST_CHECK_EQUAL(to_delete.size(), 88);

    // Verify exact backups kept when capped
    std::set<std::string> expected_kept_12 = {
        "backup_0.dat", "backup_1.dat", "backup_2.dat", "backup_3.dat", "backup_4.dat",
        "backup_5.dat", "backup_6.dat", "backup_7.dat", "backup_8.dat", "backup_9.dat",
        "backup_15.dat", "backup_31.dat"
    };
    std::set<std::string> actual_kept_12;
    for (const auto& [time, path] : backups) {
        if (std::find(to_delete.begin(), to_delete.end(), path) == to_delete.end()) {
            actual_kept_12.insert(fs::PathToString(path.filename()));
        }
    }
    BOOST_CHECK(expected_kept_12 == actual_kept_12);
}

BOOST_AUTO_TEST_CASE(irregular_backup_schedule)
{
    auto now = std::chrono::file_clock::now();
    auto make_time = [&now](int64_t days_ago) {
        auto days_duration = std::chrono::duration<int64_t, std::ratio<86400>>(days_ago);
        return fs::file_time_type{now - days_duration};
    };
    auto make_path = [](int i) {
        return fs::u8path("backup_" + ToString(i) + ".dat");
    };

    std::multimap<fs::file_time_type, fs::path> backups;

    // Test irregular schedule: multiple backups some days, gaps on others
    // Day 0: 5 backups
    for (int i = 0; i < 5; ++i) {
        backups.insert({make_time(0), make_path(i)});
    }
    // Day 1: 3 backups
    for (int i = 5; i < 8; ++i) {
        backups.insert({make_time(1), make_path(i)});
    }
    // Day 2: 2 backups
    for (int i = 8; i < 10; ++i) {
        backups.insert({make_time(2), make_path(i)});
    }
    // Day 10: 1 backup (gap)
    backups.insert({make_time(10), make_path(10)});
    // Day 20: 1 backup (gap)
    backups.insert({make_time(20), make_path(11)});

    auto to_delete = GetBackupsToDelete(backups, 10, 50);

    // Should keep:
    // - Latest 10 (5 from day 0, 3 from day 1, 2 from day 2)
    // - Oldest in [8,16): day 10
    // - Oldest in [16,32): day 20
    // Total: 12 kept, 0 deleted
    BOOST_CHECK_EQUAL(to_delete.size(), 0);

    // Verify all backups are kept
    std::set<std::string> expected_kept = {
        "backup_0.dat", "backup_1.dat", "backup_2.dat", "backup_3.dat", "backup_4.dat",
        "backup_5.dat", "backup_6.dat", "backup_7.dat", "backup_8.dat", "backup_9.dat",
        "backup_10.dat", "backup_11.dat"
    };
    std::set<std::string> actual_kept;
    for (const auto& [time, path] : backups) {
        actual_kept.insert(fs::PathToString(path.filename()));
    }
    BOOST_CHECK(expected_kept == actual_kept);
}

BOOST_AUTO_TEST_CASE(long_inactivity_period)
{
    auto now = std::chrono::file_clock::now();
    auto make_time = [&now](int64_t days_ago) {
        auto days_duration = std::chrono::duration<int64_t, std::ratio<86400>>(days_ago);
        return fs::file_time_type{now - days_duration};
    };
    auto make_path = [](int i) {
        return fs::u8path("backup_" + ToString(i) + ".dat");
    };

    std::multimap<fs::file_time_type, fs::path> backups;

    // 15 backups created 60 days ago, then nothing until today
    for (int i = 0; i < 15; ++i) {
        backups.insert({make_time(60), make_path(i)});
    }
    // New backup today
    backups.insert({make_time(0), make_path(15)});

    auto to_delete = GetBackupsToDelete(backups, 10, 50);

    // Should keep:
    // - Latest 10 (1 from today, 9 from 60 days ago)
    // - Oldest in [32,64): 6 backups from 60 days ago qualify, keep the oldest
    // Total: 11 kept, 5 deleted
    BOOST_CHECK_EQUAL(to_delete.size(), 5);

    // Verify exact backups kept
    // Sorted order: backup_15 (0 days), then backup_14-0 in reverse insertion order (all 60 days)
    // Latest 10 by count: backup_15, backup_14, backup_13, ..., backup_6
    // Oldest in [32,64): All backups 0-14 are 60 days old, backup_0 is oldest by insertion order
    std::set<std::string> expected_kept = {
        "backup_0.dat",   // oldest in [32,64) range
        "backup_6.dat", "backup_7.dat", "backup_8.dat", "backup_9.dat",
        "backup_10.dat", "backup_11.dat", "backup_12.dat", "backup_13.dat", "backup_14.dat",
        "backup_15.dat"   // newest
    };
    std::set<std::string> actual_kept;
    for (const auto& [time, path] : backups) {
        if (std::find(to_delete.begin(), to_delete.end(), path) == to_delete.end()) {
            actual_kept.insert(fs::PathToString(path.filename()));
        }
    }
    BOOST_CHECK(expected_kept == actual_kept);
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace wallet
