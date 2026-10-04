// Copyright (c) 2021-2022 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WALLET_TEST_UTIL_H
#define BITCOIN_WALLET_TEST_UTIL_H

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <script/standard.h>
#include <wallet/db.h>

#include <memory>

class ArgsManager;
class ChainstateManager;
class CKey;
enum class OutputType;
namespace interfaces {
class Chain;
namespace CoinJoin {
class Loader;
} // namespace CoinJoin
} // namespace interfaces

namespace wallet {
class CWallet;
struct DatabaseOptions;
class WalletDatabase;

std::unique_ptr<CWallet> CreateSyncedWallet(interfaces::Chain& chain, interfaces::CoinJoin::Loader& coinjoin_loader, ChainstateManager& chainman, ArgsManager& args, const CKey& key);

// Creates a copy of the provided database
std::unique_ptr<WalletDatabase> DuplicateMockDatabase(WalletDatabase& database, DatabaseOptions& options);

/** Returns a new encoded destination from the wallet */
std::string getnewaddress(wallet::CWallet& w);
/** Returns a new destination, of an specific type, from the wallet */
CTxDestination getNewDestination(wallet::CWallet& w, OutputType output_type);

class FailCursor : public DatabaseCursor
{
private:
    bool m_pass{true};

public:
    explicit FailCursor(bool pass) : m_pass(pass) {}
    Status Next(DataStream& key, DataStream& value) override { return m_pass ? Status::DONE : Status::FAIL; }
};

/** RAII class that provides access to a FailDatabase. Which fails if needed. */
class FailBatch : public DatabaseBatch
{
private:
    bool m_pass{true};
    bool ReadKey(DataStream&&, DataStream&) override { return m_pass; }
    bool WriteKey(DataStream&&, DataStream&&, bool) override { return m_pass; }
    bool EraseKey(DataStream&&) override { return m_pass; }
    bool HasKey(DataStream&&) override { return m_pass; }
    bool ErasePrefix(Span<const std::byte>) override { return m_pass; }

public:
    explicit FailBatch(bool pass) : m_pass(pass) {}
    void Flush() override {}
    void Close() override {}

    std::unique_ptr<DatabaseCursor> GetNewCursor() override { return std::make_unique<FailCursor>(m_pass); }
    bool TxnBegin() override { return m_pass; }
    bool TxnCommit() override { return m_pass; }
    bool TxnAbort() override { return m_pass; }
};

/** A dummy WalletDatabase that does nothing, only fails if needed.**/
class FailDatabase : public WalletDatabase
{
public:
    bool m_pass{true}; // false when this db should fail

    void Open() override {}
    void AddRef() override {}
    void RemoveRef() override {}
    bool Rewrite(const char* = nullptr) override { return true; }
    bool Backup(const std::string&) const override { return true; }
    void Close() override {}
    void Flush() override {}
    bool PeriodicFlush() override { return true; }
    void IncrementUpdateCounter() override { ++nUpdateCounter; }
    void ReloadDbEnv() override {}
    std::string Filename() override { return "faildb"; }
    std::string Format() override { return "faildb"; }
    std::unique_ptr<DatabaseBatch> MakeBatch(bool = true) override { return std::make_unique<FailBatch>(m_pass); }
};

} // namespace wallet

#endif // BITCOIN_WALLET_TEST_UTIL_H
