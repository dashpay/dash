// Copyright (c) 2021 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_WALLET_TEST_UTIL_H
#define BITCOIN_WALLET_TEST_UTIL_H

#include <wallet/db.h>

#include <memory>

class ArgsManager;
class CChain;
class CKey;
namespace interfaces {
class Chain;
namespace CoinJoin {
class Loader;
} // namespace CoinJoin
} // namespace interfaces

namespace wallet {
class CWallet;

std::unique_ptr<CWallet> CreateSyncedWallet(interfaces::Chain& chain, interfaces::CoinJoin::Loader& coinjoin_loader, CChain& cchain, ArgsManager& args, const CKey& key);

class MockableBatch : public DatabaseBatch
{
private:
    using Record = std::map<SerializeData, SerializeData>;

    Record& m_records;
    bool m_pass;
    Record::iterator m_cursor_it;
    bool m_cursor_active;

    static bool DeserializeKey(CDataStream&& key, SerializeData& out);

public:
    MockableBatch(Record& records, bool pass);
    ~MockableBatch();

    bool ReadKey(CDataStream&& key, CDataStream& value) override;
    bool WriteKey(CDataStream&& key, CDataStream&& value, bool overwrite=true) override;
    bool EraseKey(CDataStream&& key) override;
    bool HasKey(CDataStream&& key) override;
    bool ErasePrefix(Span<const std::byte> prefix) override;

    void Flush() override;
    void Close() override;

    bool StartCursor() override;
    bool ReadAtCursor(CDataStream& ssKey, CDataStream& ssValue, bool& complete) override;
    void CloseCursor() override;

    bool TxnBegin() override;
    bool TxnCommit() override;
    bool TxnAbort() override;
};


/** A WalletDatabase whose contents and return values can be modified as needed for testing
 **/
class MockableDatabase : public WalletDatabase
{
public:
    std::map<SerializeData, SerializeData> m_records;
    bool m_pass{true};

    MockableDatabase(std::map<SerializeData, SerializeData> records = {}) : WalletDatabase(), m_records(records) {}
    ~MockableDatabase() {};

    void Open() override {}
    void AddRef() override {}
    void RemoveRef() override {}

    bool Rewrite(const char* pszSkip=nullptr) override { return m_pass; }
    bool Backup(const std::string& strDest) const override { return m_pass; }
    void Flush() override {}
    void Close() override {}
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
