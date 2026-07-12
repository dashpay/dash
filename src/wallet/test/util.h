// Copyright (c) 2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WALLET_TEST_UTIL_H
#define BITCOIN_WALLET_TEST_UTIL_H

#include <script/standard.h>
#include <wallet/db.h>

#include <map>
#include <memory>
#include <string>

class ArgsManager;
class ChainstateManager;
class CKey;
namespace interfaces {
class Chain;
namespace CoinJoin {
class Loader;
} // namespace CoinJoin
} // namespace interfaces

namespace wallet {
class CWallet;
class WalletDatabase;

extern const std::string ADDRESS_B58T_UNSPENDABLE;
extern const std::string ADDRESS_BCRT1_UNSPENDABLE;

std::unique_ptr<CWallet> CreateSyncedWallet(interfaces::Chain& chain, interfaces::CoinJoin::Loader& coinjoin_loader, ChainstateManager& chainman, ArgsManager& args, const CKey& key);

/** Returns a new encoded destination from the wallet */
std::string getnewaddress(CWallet& w);
/** Returns a new destination from the wallet. Dash only supports OutputType::LEGACY. */
CTxDestination getNewDestination(CWallet& w);

// Creates a copy of the provided database
std::unique_ptr<WalletDatabase> DuplicateMockDatabase(WalletDatabase& database);

class MockableBatch : public DatabaseBatch
{
private:
    std::map<SerializeData, SerializeData>& m_records;
    std::map<SerializeData, SerializeData>::const_iterator m_cursor;
    bool m_pass;

    bool ReadKey(CDataStream&& key, CDataStream& value) override;
    bool WriteKey(CDataStream&& key, CDataStream&& value, bool overwrite = true) override;
    bool EraseKey(CDataStream&& key) override;
    bool HasKey(CDataStream&& key) override;
    bool ErasePrefix(Span<const std::byte> prefix) override;

public:
    explicit MockableBatch(std::map<SerializeData, SerializeData>& records, bool pass) : m_records(records), m_cursor(m_records.cbegin()), m_pass(pass) {}
    void Flush() override {}
    void Close() override {}
    bool StartCursor() override;
    bool ReadAtCursor(CDataStream& ssKey, CDataStream& ssValue, bool& complete) override;
    void CloseCursor() override {}
    bool TxnBegin() override { return m_pass; }
    bool TxnCommit() override { return m_pass; }
    bool TxnAbort() override { return m_pass; }
};

class MockableDatabase : public WalletDatabase
{
public:
    std::map<SerializeData, SerializeData> m_records;
    bool m_pass{true};

    explicit MockableDatabase(std::map<SerializeData, SerializeData> records = {}) : WalletDatabase(), m_records(std::move(records)) {}

    void Open() override {}
    void AddRef() override {}
    void RemoveRef() override {}
    bool Rewrite(const char* pszSkip = nullptr) override { return m_pass; }
    bool Backup(const std::string& strDest) const override { return m_pass; }
    void Close() override {}
    void Flush() override {}
    bool PeriodicFlush() override { return m_pass; }
    void IncrementUpdateCounter() override {}
    void ReloadDbEnv() override {}
    std::string Filename() override { return "mockable"; }
    std::string Format() override { return "mock"; }
    std::unique_ptr<DatabaseBatch> MakeBatch(bool flush_on_close = true) override { return std::make_unique<MockableBatch>(m_records, m_pass); }
};

std::unique_ptr<WalletDatabase> CreateMockableWalletDatabase(std::map<SerializeData, SerializeData> records = {});
MockableDatabase& GetMockableDatabase(CWallet& wallet);
} // namespace wallet

#endif // BITCOIN_WALLET_TEST_UTIL_H
