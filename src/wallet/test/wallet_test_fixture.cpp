// Copyright (c) 2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "wallet/test/wallet_test_fixture.h"

#include "rpc/server.h"
#include "wallet/db.h"
#include "wallet/wallet.h"

WalletTestingSetup::WalletTestingSetup(const std::string& chainName):
    TestingSetup(chainName)
{
    LogPrintf("WalletTestingSetup 1\n");

    bitdb.MakeMock();
    LogPrintf("WalletTestingSetup 2\n");

    bool fFirstRun;
    pwalletMain = new CWallet("wallet_test.dat");
    pwalletMain->LoadWallet(fFirstRun);
    LogPrintf("WalletTestingSetup 3\n");
    RegisterValidationInterface(pwalletMain);

    RegisterWalletRPCCommands(tableRPC);
    LogPrintf("WalletTestingSetup 4\n");

}

WalletTestingSetup::~WalletTestingSetup()
{
    LogPrintf("~WalletTestingSetup 1\n");
    UnregisterValidationInterface(pwalletMain);
    LogPrintf("~WalletTestingSetup 2\n");
    delete pwalletMain;
    LogPrintf("~WalletTestingSetup 3\n");
    pwalletMain = NULL;

    bitdb.Flush(true);
    LogPrintf("~WalletTestingSetup 4\n");
    bitdb.Reset();
    LogPrintf("~WalletTestingSetup 5\n");
}
