// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <common/args.h>
#include <common/system.h>
#include <test/util/setup_common.h>
#include <util/fs.h>
#include <wallet/wallet.h>

#include <boost/test/unit_test.hpp>

#include <algorithm>
#include <chrono>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace wallet {

namespace {
using BackupTime = std::chrono::system_clock::time_point;
using Backups = std::multimap<BackupTime, fs::path>;

//! Fixtures are anchored on a fixed date so that age-range boundaries are exact
//! rather than dependent on when the test happens to run.
constexpr std::chrono::sys_days BACKUP_ANCHOR{std::chrono::year{2026} / 8 / 4};

//! Filename AutoBackupWallet() writes for a backup taken at `time`.
fs::path BackupPath(BackupTime time)
{
    return fs::u8path("wallet.dat" + FormatBackupFileTime(std::chrono::floor<std::chrono::seconds>(time)));
}

//! A backup taken `days_ago` days before the anchor; `minute` tells apart several taken that day.
Backups::value_type Backup(int days_ago, int minute = 0)
{
    const BackupTime time{BACKUP_ANCHOR - std::chrono::days{days_ago} + std::chrono::minutes{minute}};
    return {time, BackupPath(time)};
}

Backups DailyBackups(int count)
{
    Backups backups;
    for (int i = 0; i < count; ++i)
        backups.insert(Backup(i));
    return backups;
}

std::set<fs::path> Paths(const Backups& backups)
{
    std::set<fs::path> paths;
    for (const auto& [time, path] : backups)
        paths.insert(path);
    return paths;
}

//! Names of the backups that survive pruning with `backup_time` as the backup just written.
std::set<fs::path> Kept(const Backups& backups, BackupTime backup_time, int keep_recent, int max_backups)
{
    std::set<fs::path> kept{Paths(backups)};
    for (const auto& path : GetBackupsToDelete(backups, backup_time, keep_recent, max_backups)) {
        BOOST_CHECK_EQUAL(kept.erase(path), 1U);
    }
    return kept;
}
} // namespace

BOOST_FIXTURE_TEST_SUITE(backup_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(retention_by_count_and_age)
{
    // Nothing is deleted while the count window isn't full.
    BOOST_CHECK(GetBackupsToDelete(DailyBackups(10), BACKUP_ANCHOR, 10, 50).empty());

    // Backups less than an hour older than the newest fall in no age range, so beyond
    // the count window they are dropped.
    Backups backups;
    for (int minute = 0; minute < 11; ++minute)
        backups.insert(Backup(0, minute));
    auto expected{Paths(backups)};
    expected.erase(Backup(0, 0).second);
    BOOST_CHECK(Kept(backups, backups.rbegin()->first, 10, 50) == expected);

    // 100 daily backups: the last 10 by count, then days 10, 21, 42, 85 and 99 as the
    // oldest in [128,256), [256,512), [512,1024), [1024,2048) and [2048,4096) hours.
    backups = DailyBackups(100);
    expected = Paths({Backup(0), Backup(1), Backup(2), Backup(3), Backup(4), Backup(5), Backup(6), Backup(7), Backup(8),
                      Backup(9), Backup(10), Backup(21), Backup(42), Backup(85), Backup(99)});
    BOOST_CHECK(Kept(backups, BACKUP_ANCHOR, 10, 50) == expected);

    // -maxwalletbackups below that drops the widest ranges first, and equal to the count
    // window degrades to keeping the most recent backups only.
    expected.erase(Backup(42).second);
    expected.erase(Backup(85).second);
    expected.erase(Backup(99).second);
    BOOST_CHECK(Kept(backups, BACKUP_ANCHOR, 10, 12) == expected);
    BOOST_CHECK(Kept(backups, BACKUP_ANCHOR, 10, 10) == Paths(DailyBackups(10)));

    // The count window is positional: after 60 idle days the new backup pushes out only
    // one of the old ones, and of the rest the oldest stays for the [1024,2048) hour range.
    backups.clear();
    for (int minute = 0; minute < 15; ++minute)
        backups.insert(Backup(60, minute));
    backups.insert(Backup(0));
    expected = Paths({Backup(0), Backup(60, 0)});
    for (int minute = 6; minute < 15; ++minute)
        expected.insert(Backup(60, minute).second);
    BOOST_CHECK(Kept(backups, BACKUP_ANCHOR, 10, 50) == expected);

    // Disabled (0) or failed (negative) automatic backups delete nothing.
    for (int keep_recent : {0, -1, -2}) {
        BOOST_CHECK(GetBackupsToDelete(backups, BACKUP_ANCHOR, keep_recent, 30).empty());
    }
    BOOST_CHECK(GetBackupsToDelete(backups, BACKUP_ANCHOR, 10, 0).empty());
}

BOOST_AUTO_TEST_CASE(clock_moved_backward)
{
    // 40 daily backups written with a correct clock, then the clock jumps back 20 days
    // and a new backup is written half an hour after the existing day-20 one.
    Backups backups{DailyBackups(40)};
    const auto just_written{Backup(20, 30)};
    backups.insert(just_written);

    // Ranked by filename time alone, the new backup would fall behind the 20 later-dated
    // files, out of the count window, and be deleted as a non-oldest member of its range.
    // Instead those 20 are left alone and the rest is pruned as if the new backup were the
    // newest: it and days 20-28 fill the count window, and days 30 and 39 are the oldest
    // in the [128,256) and [256,512) hour ranges before it.
    auto expected{Paths(backups)};
    for (int day : {29, 31, 32, 33, 34, 35, 36, 37, 38})
        expected.erase(Backup(day).second);
    BOOST_CHECK(Kept(backups, just_written.first, 10, 50) == expected);
}

BOOST_AUTO_TEST_CASE(frequent_backups_keep_history)
{
    // A wallet that mixes all day can write more backups a day than the count window
    // holds. Replay the pruning AutoBackupWallet() does after each backup over 60 days of
    // one backup every 72 minutes (20 a day).
    Backups backups;
    const BackupTime first_backup{BACKUP_ANCHOR - std::chrono::days{60}};
    auto backup_time{first_backup};
    for (int i = 0; i < 20 * 60; ++i, backup_time += std::chrono::minutes{72}) {
        backups.insert({backup_time, BackupPath(backup_time)});
        for (const auto& path : GetBackupsToDelete(backups, backup_time, 10, 30)) {
            std::erase_if(backups, [&](const auto& entry) { return entry.second == path; });
        }
        BOOST_REQUIRE_LE(backups.size(), 30U);
    }

    // Restore points older than the last 10 backups (12 hours) survive, back to the first.
    const auto newest{backups.rbegin()->first};
    const auto older_than{[&](std::chrono::hours age) {
        return std::count_if(backups.begin(), backups.end(),
                             [&](const auto& entry) { return newest - entry.first >= age; });
    }};
    BOOST_CHECK_GE(older_than(std::chrono::days{1}), 4);
    BOOST_CHECK_GE(older_than(std::chrono::days{7}), 3);
    BOOST_CHECK(backups.begin()->first == first_backup);
}

BOOST_AUTO_TEST_CASE(init_auto_backup_clamping)
{
    const int orig_backups{nWalletBackups};
    const int orig_max_backups{CWallet::nMaxWalletBackups};

    const auto check{[](const std::vector<const char*>& argv, int expected_backups, int expected_max_backups) {
        ArgsManager args;
        args.AddArg("-createwalletbackups=<n>", "", ArgsManager::ALLOW_ANY, OptionsCategory::WALLET);
        args.AddArg("-maxwalletbackups=<n>", "", ArgsManager::ALLOW_ANY, OptionsCategory::WALLET);
        std::vector<const char*> argv_full{"ignored"};
        argv_full.insert(argv_full.end(), argv.begin(), argv.end());
        std::string error;
        BOOST_REQUIRE(args.ParseParameters(argv_full.size(), argv_full.data(), error));
        CWallet::InitAutoBackup(args);
        BOOST_CHECK_EQUAL(nWalletBackups, expected_backups);
        BOOST_CHECK_EQUAL(CWallet::nMaxWalletBackups, expected_max_backups);
    }};

    check({}, DEFAULT_N_WALLET_BACKUPS, DEFAULT_MAX_WALLET_BACKUPS);
    check({"-createwalletbackups=100"}, MAX_N_WALLET_BACKUPS, DEFAULT_MAX_WALLET_BACKUPS);
    check({"-createwalletbackups=-5"}, 0, DEFAULT_MAX_WALLET_BACKUPS);
    check({"-createwalletbackups=15", "-maxwalletbackups=12"}, 12, 12);
    check({"-maxwalletbackups=0"}, 0, 0);
    check({"-maxwalletbackups=-1"}, 0, 0);

    nWalletBackups = orig_backups;
    CWallet::nMaxWalletBackups = orig_max_backups;
}

BOOST_AUTO_TEST_CASE(parse_backup_file_time)
{
    using namespace std::chrono;

    const auto parsed{ParseBackupFileTime(fs::u8path("wallet.dat.2026-08-02-14-30"))};
    BOOST_REQUIRE(parsed.has_value());
    const sys_seconds time{sys_days{year{2026} / 8 / 2} + hours{14} + minutes{30}};
    BOOST_CHECK(*parsed == time);
    // What AutoBackupWallet() writes, seconds dropped, is what the parser accepts.
    BOOST_CHECK_EQUAL(FormatBackupFileTime(time + seconds{59}), ".2026-08-02-14-30");

    for (const char* name : {"wallet.dat", "wallet.dat.bak", "wallet.dat.2026-08-02", "wallet.dat.2026-08-02-14-3x",
                             "wallet.dat.2026-13-02-14-30", "wallet.dat.2026-02-30-14-30", "wallet.dat.2026-08-02-24-30",
                             "wallet.dat.2026-08-02-14-60", "wallet.dat.2500-01-01-00-00"}) {
        BOOST_CHECK_MESSAGE(!ParseBackupFileTime(fs::u8path(name)).has_value(), name);
    }
}

BOOST_AUTO_TEST_SUITE_END()

} // namespace wallet
