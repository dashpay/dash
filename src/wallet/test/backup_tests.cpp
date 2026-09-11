#include <wallet/test/wallet_test_fixture.h>
#include <wallet/wallet.h>

#include <boost/test/unit_test.hpp>

#include <chrono>
#include <map>
#include <util/string.h>
#include <vector>

namespace wallet {

namespace {
//! Fixtures are anchored on a fixed date so that day-bucket boundaries are exact
//! rather than dependent on when the test happens to run.
constexpr std::chrono::sys_days BACKUP_ANCHOR{std::chrono::year{2026} / 8 / 4};

//! Timestamp of a backup taken `days_ago` days before the anchor.
std::chrono::system_clock::time_point MakeBackupTime(int days_ago)
{
    return BACKUP_ANCHOR - std::chrono::days{days_ago};
}

//! Backup filename AutoBackupWallet() writes for that timestamp. `sequence` distinguishes
//! several backups taken on the same day, as the minute field of the name.
fs::path MakeBackupPath(int days_ago, int sequence = 0)
{
    const std::chrono::year_month_day date{BACKUP_ANCHOR - std::chrono::days{days_ago}};
    return fs::u8path(strprintf("wallet.dat.%04i-%02u-%02u-00-%02i", int(date.year()), unsigned(date.month()),
                                unsigned(date.day()), sequence));
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(backup_tests, WalletTestingSetup)

BOOST_AUTO_TEST_CASE(time_based_exponential_retention)
{
    std::multimap<std::chrono::system_clock::time_point, fs::path> backups;

    // Case 1: Less than nWalletBackups (10)
    for (int i = 0; i < 5; ++i) {
        backups.insert({MakeBackupTime(i), MakeBackupPath(i)});
    }
    auto to_delete = GetBackupsToDelete(backups, 10, 50);
    BOOST_CHECK(to_delete.empty());

    // Case 2: Exactly nWalletBackups (10)
    backups.clear();
    for (int i = 0; i < 10; ++i) {
        backups.insert({MakeBackupTime(i), MakeBackupPath(i)});
    }
    to_delete = GetBackupsToDelete(backups, 10, 50);
    BOOST_CHECK(to_delete.empty());

    // Case 3: 11 backups - all recent (< 1 day old)
    // Since all are < 1 day old, no time-based retention applies
    // Keep latest 10, but the 11th is also < 1 day so it doesn't get kept
    backups.clear();
    for (int i = 0; i < 11; ++i) {
        backups.insert({MakeBackupTime(0), MakeBackupPath(0, i)});
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
        backups.insert({MakeBackupTime(0), MakeBackupPath(0, i)});
    }
    backups.insert({MakeBackupTime(1), MakeBackupPath(1)});   // [1,2) days
    backups.insert({MakeBackupTime(2), MakeBackupPath(2)});   // [2,4) days
    backups.insert({MakeBackupTime(3), MakeBackupPath(3)});   // [2,4) days
    backups.insert({MakeBackupTime(5), MakeBackupPath(5)});   // [4,8) days
    backups.insert({MakeBackupTime(7), MakeBackupPath(7)});   // [4,8) days
    backups.insert({MakeBackupTime(10), MakeBackupPath(10)}); // [8,16) days
    backups.insert({MakeBackupTime(15), MakeBackupPath(15)}); // [8,16) days
    backups.insert({MakeBackupTime(20), MakeBackupPath(20)}); // [16,32) days
    backups.insert({MakeBackupTime(25), MakeBackupPath(25)}); // [16,32) days
    backups.insert({MakeBackupTime(30), MakeBackupPath(30)}); // [16,32) days

    to_delete = GetBackupsToDelete(backups, 10, 50);

    // Should keep:
    // - Latest 10 (by count): all backups from today
    // - Oldest in each populated exponential time range
    // Total: 15 kept, 5 deleted
    BOOST_CHECK_EQUAL(to_delete.size(), 5);

    // Verify exactly which backups are deleted
    std::set<fs::path> expected_deletions = {
        MakeBackupPath(2),  // not oldest in [2,4)
        MakeBackupPath(5),  // not oldest in [4,8)
        MakeBackupPath(10), // not oldest in [8,16)
        MakeBackupPath(20), // not oldest in [16,32)
        MakeBackupPath(25)  // not oldest in [16,32)
    };
    const std::set<fs::path> actual_deletions{to_delete.begin(), to_delete.end()};
    BOOST_CHECK(expected_deletions == actual_deletions);

    // Case 5: Test that we accumulate over time
    // Simulate 100 days of daily backups
    backups.clear();
    for (int i = 0; i < 100; ++i) {
        backups.insert({MakeBackupTime(i), MakeBackupPath(i)});
    }

    to_delete = GetBackupsToDelete(backups, 10, 50);

    // Should keep:
    // - Latest 10 by count and the oldest backup in each exponential time range
    // Total: 14 kept, 86 deleted
    BOOST_CHECK_EQUAL(to_delete.size(), 86);

    // Verify specific kept backups in exponential ranges
    std::set<fs::path> expected_kept = {MakeBackupPath(0),  MakeBackupPath(1), MakeBackupPath(2),  MakeBackupPath(3),
                                        MakeBackupPath(4),  MakeBackupPath(5), MakeBackupPath(6),  MakeBackupPath(7),
                                        MakeBackupPath(8),  MakeBackupPath(9), MakeBackupPath(15), MakeBackupPath(31),
                                        MakeBackupPath(63), MakeBackupPath(99)};
    std::set<fs::path> actual_kept;
    for (const auto& [time, path] : backups) {
        if (std::find(to_delete.begin(), to_delete.end(), path) == to_delete.end()) {
            actual_kept.insert(path);
        }
    }
    BOOST_CHECK(expected_kept == actual_kept);
}

BOOST_AUTO_TEST_CASE(hard_max_limit)
{
    std::multimap<std::chrono::system_clock::time_point, fs::path> backups;

    // Create 100 daily backups and set maxBackups=15
    for (int i = 0; i < 100; ++i) {
        backups.insert({MakeBackupTime(i), MakeBackupPath(i)});
    }

    auto to_delete = GetBackupsToDelete(backups, 10, 15);

    // Without maxBackups limit, we'd keep 14 backups (see Case 5 above)
    // With maxBackups=15, we still keep 14 (under the limit)
    BOOST_CHECK_EQUAL(to_delete.size(), 86);

    // Verify same backups kept as in Case 5
    std::set<fs::path> expected_kept_15 = {MakeBackupPath(0),  MakeBackupPath(1),  MakeBackupPath(2),
                                           MakeBackupPath(3),  MakeBackupPath(4),  MakeBackupPath(5),
                                           MakeBackupPath(6),  MakeBackupPath(7),  MakeBackupPath(8),
                                           MakeBackupPath(9),  MakeBackupPath(15), MakeBackupPath(31),
                                           MakeBackupPath(63), MakeBackupPath(99)};
    std::set<fs::path> actual_kept_15;
    for (const auto& [time, path] : backups) {
        if (std::find(to_delete.begin(), to_delete.end(), path) == to_delete.end()) {
            actual_kept_15.insert(path);
        }
    }
    BOOST_CHECK(expected_kept_15 == actual_kept_15);

    // Now test with maxBackups=12 (less than natural retention)
    to_delete = GetBackupsToDelete(backups, 10, 12);

    // Should cap at 12 backups: keep latest 10 + 2 oldest time ranges
    // Total: 12 kept, 88 deleted
    BOOST_CHECK_EQUAL(to_delete.size(), 88);

    // Verify exact backups kept when capped
    std::set<fs::path> expected_kept_12 = {MakeBackupPath(0), MakeBackupPath(1),  MakeBackupPath(2),
                                           MakeBackupPath(3), MakeBackupPath(4),  MakeBackupPath(5),
                                           MakeBackupPath(6), MakeBackupPath(7),  MakeBackupPath(8),
                                           MakeBackupPath(9), MakeBackupPath(15), MakeBackupPath(31)};
    std::set<fs::path> actual_kept_12;
    for (const auto& [time, path] : backups) {
        if (std::find(to_delete.begin(), to_delete.end(), path) == to_delete.end()) {
            actual_kept_12.insert(path);
        }
    }
    BOOST_CHECK(expected_kept_12 == actual_kept_12);
}

BOOST_AUTO_TEST_CASE(irregular_backup_schedule)
{
    std::multimap<std::chrono::system_clock::time_point, fs::path> backups;

    // Test irregular schedule: multiple backups some days, gaps on others
    // Day 0: 5 backups
    for (int i = 0; i < 5; ++i) {
        backups.insert({MakeBackupTime(0), MakeBackupPath(0, i)});
    }
    // Day 1: 3 backups
    for (int i = 5; i < 8; ++i) {
        backups.insert({MakeBackupTime(1), MakeBackupPath(1, i - 5)});
    }
    // Day 2: 2 backups
    for (int i = 8; i < 10; ++i) {
        backups.insert({MakeBackupTime(2), MakeBackupPath(2, i - 8)});
    }
    // Day 10: 1 backup (gap)
    backups.insert({MakeBackupTime(10), MakeBackupPath(10)});
    // Day 20: 1 backup (gap)
    backups.insert({MakeBackupTime(20), MakeBackupPath(20)});

    auto to_delete = GetBackupsToDelete(backups, 10, 50);

    // Should keep:
    // - Latest 10 (5 from day 0, 3 from day 1, 2 from day 2)
    // - Oldest in [8,16): day 10
    // - Oldest in [16,32): day 20
    // Total: 12 kept, 0 deleted
    BOOST_CHECK_EQUAL(to_delete.size(), 0);

    // Verify all backups are kept
    std::set<fs::path> expected_kept = {MakeBackupPath(0, 0), MakeBackupPath(0, 1), MakeBackupPath(0, 2),
                                        MakeBackupPath(0, 3), MakeBackupPath(0, 4), MakeBackupPath(1, 0),
                                        MakeBackupPath(1, 1), MakeBackupPath(1, 2), MakeBackupPath(2, 0),
                                        MakeBackupPath(2, 1), MakeBackupPath(10),   MakeBackupPath(20)};
    std::set<fs::path> actual_kept;
    for (const auto& [time, path] : backups) {
        actual_kept.insert(path);
    }
    BOOST_CHECK(expected_kept == actual_kept);
}

BOOST_AUTO_TEST_CASE(long_inactivity_period)
{
    std::multimap<std::chrono::system_clock::time_point, fs::path> backups;

    // 15 backups created 60 days ago, then nothing until today
    for (int i = 0; i < 15; ++i) {
        backups.insert({MakeBackupTime(60), MakeBackupPath(60, i)});
    }
    // New backup today
    backups.insert({MakeBackupTime(0), MakeBackupPath(0)});

    auto to_delete = GetBackupsToDelete(backups, 10, 50);

    // Should keep:
    // - Latest 10 (1 from today, 9 from 60 days ago)
    // - Oldest in [32,64): 6 backups from 60 days ago qualify, keep the oldest
    // Total: 11 kept, 5 deleted
    BOOST_CHECK_EQUAL(to_delete.size(), 5);

    // Verify exact backups kept
    // The current backup plus the nine latest at 60 days old are kept by count.
    // The earliest 60-day backup is retained from the [32,64) range.
    std::set<fs::path> expected_kept = {MakeBackupPath(0),     // newest
                                        MakeBackupPath(60, 0), // oldest in [32,64) range
                                        MakeBackupPath(60, 6),  MakeBackupPath(60, 7),  MakeBackupPath(60, 8),
                                        MakeBackupPath(60, 9),  MakeBackupPath(60, 10), MakeBackupPath(60, 11),
                                        MakeBackupPath(60, 12), MakeBackupPath(60, 13), MakeBackupPath(60, 14)};
    std::set<fs::path> actual_kept;
    for (const auto& [time, path] : backups) {
        if (std::find(to_delete.begin(), to_delete.end(), path) == to_delete.end()) {
            actual_kept.insert(path);
        }
    }
    BOOST_CHECK(expected_kept == actual_kept);
}

BOOST_AUTO_TEST_CASE(non_positive_max_backups)
{
    std::multimap<std::chrono::system_clock::time_point, fs::path> backups;
    for (int i = 0; i < 20; ++i) {
        backups.insert({MakeBackupTime(i), MakeBackupPath(i)});
    }

    // maxBackups <= 0 means "delete nothing", regardless of nWalletBackups
    BOOST_CHECK(GetBackupsToDelete(backups, 10, 0).empty());
    BOOST_CHECK(GetBackupsToDelete(backups, 10, -1).empty());
    BOOST_CHECK(GetBackupsToDelete(backups, 0, 0).empty());
}

BOOST_AUTO_TEST_CASE(count_window_boundaries)
{
    std::multimap<std::chrono::system_clock::time_point, fs::path> backups;
    for (int i = 0; i < 20; ++i) {
        backups.insert({MakeBackupTime(i), MakeBackupPath(i)});
    }

    // maxBackups == nWalletBackups leaves no room for time buckets, degrading to
    // the pre-exponential "keep the N most recent" policy.
    auto to_delete = GetBackupsToDelete(backups, 10, 10);
    BOOST_CHECK_EQUAL(to_delete.size(), 10);
    std::set<fs::path> expected_deleted;
    for (int i = 10; i < 20; ++i) {
        expected_deleted.insert(MakeBackupPath(i));
    }
    BOOST_CHECK(expected_deleted == std::set<fs::path>(to_delete.begin(), to_delete.end()));

    // An empty count window leaves retention to the time buckets: the newest backup is
    // kept as their anchor, then one per [1,2), [2,4), [4,8), [8,16), [16,32).
    to_delete = GetBackupsToDelete(backups, 0, 30);
    std::set<fs::path> expected_kept{MakeBackupPath(0), MakeBackupPath(1),  MakeBackupPath(3),
                                     MakeBackupPath(7), MakeBackupPath(15), MakeBackupPath(19)};
    std::set<fs::path> actual_kept;
    for (const auto& [time, path] : backups) {
        if (std::find(to_delete.begin(), to_delete.end(), path) == to_delete.end()) {
            actual_kept.insert(path);
        }
    }
    BOOST_CHECK(expected_kept == actual_kept);

    // Negative nWalletBackups is treated the same as an empty count window.
    BOOST_CHECK(GetBackupsToDelete(backups, -1, 30) == to_delete);
}

BOOST_AUTO_TEST_CASE(init_auto_backup_clamping)
{
    const int nWalletBackupsOrig = CWallet::nWalletBackups;
    const int nMaxWalletBackupsOrig = CWallet::nMaxWalletBackups;

    auto init_with_args = [](const std::vector<const char*>& argv) {
        ArgsManager args;
        args.AddArg("-createwalletbackups=<n>", "", ArgsManager::ALLOW_ANY, OptionsCategory::WALLET);
        args.AddArg("-maxwalletbackups=<n>", "", ArgsManager::ALLOW_ANY, OptionsCategory::WALLET);
        args.AddArg("-disablewallet", "", ArgsManager::ALLOW_ANY, OptionsCategory::WALLET);
        std::vector<const char*> argv_full{"ignored"};
        argv_full.insert(argv_full.end(), argv.begin(), argv.end());
        std::string error;
        BOOST_REQUIRE(args.ParseParameters(argv_full.size(), argv_full.data(), error));
        CWallet::InitAutoBackup(args);
    };

    // Defaults
    init_with_args({});
    BOOST_CHECK_EQUAL(CWallet::nWalletBackups, DEFAULT_N_WALLET_BACKUPS);
    BOOST_CHECK_EQUAL(CWallet::nMaxWalletBackups, DEFAULT_MAX_BACKUPS);

    // -createwalletbackups above MAX_N_WALLET_BACKUPS clamps to the max
    init_with_args({"-createwalletbackups=100"});
    BOOST_CHECK_EQUAL(CWallet::nWalletBackups, MAX_N_WALLET_BACKUPS);

    // Negative -createwalletbackups clamps to 0 (disabled)
    init_with_args({"-createwalletbackups=-5"});
    BOOST_CHECK_EQUAL(CWallet::nWalletBackups, 0);

    // -createwalletbackups is limited by -maxwalletbackups
    init_with_args({"-createwalletbackups=15", "-maxwalletbackups=12"});
    BOOST_CHECK_EQUAL(CWallet::nWalletBackups, 12);
    BOOST_CHECK_EQUAL(CWallet::nMaxWalletBackups, 12);

    // -maxwalletbackups=0 disables automatic backups entirely
    init_with_args({"-maxwalletbackups=0"});
    BOOST_CHECK_EQUAL(CWallet::nWalletBackups, 0);
    BOOST_CHECK_EQUAL(CWallet::nMaxWalletBackups, 0);

    CWallet::nWalletBackups = nWalletBackupsOrig;
    CWallet::nMaxWalletBackups = nMaxWalletBackupsOrig;
}

BOOST_AUTO_TEST_CASE(parse_backup_file_time)
{
    using namespace std::chrono;

    const auto parsed = ParseBackupFileTime(fs::u8path("wallet.dat.2026-08-02-14-30"));
    BOOST_REQUIRE(parsed.has_value());
    const auto expected = sys_days{year{2026} / 8 / 2} + hours{14} + minutes{30};
    BOOST_CHECK(*parsed == expected);

    // Wrong shape or invalid fields
    BOOST_CHECK(!ParseBackupFileTime(fs::u8path("wallet.dat")).has_value());
    BOOST_CHECK(!ParseBackupFileTime(fs::u8path("wallet.dat.bak")).has_value());
    BOOST_CHECK(!ParseBackupFileTime(fs::u8path("wallet.dat.2026-08-02")).has_value());
    BOOST_CHECK(!ParseBackupFileTime(fs::u8path("wallet.dat.2026-08-02-14-3x")).has_value());
    BOOST_CHECK(!ParseBackupFileTime(fs::u8path("wallet.dat.2026-13-02-14-30")).has_value());
    BOOST_CHECK(!ParseBackupFileTime(fs::u8path("wallet.dat.2026-02-30-14-30")).has_value());
    BOOST_CHECK(!ParseBackupFileTime(fs::u8path("wallet.dat.2026-08-02-24-30")).has_value());
    BOOST_CHECK(!ParseBackupFileTime(fs::u8path("wallet.dat.2026-08-02-14-60")).has_value());
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace wallet
