// Copyright (c) 2024-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <interfaces/coinjoin.h>

#include <coinjoin/client.h>
#include <coinjoin/options.h>
#include <coinjoin/walletman.h>
#include <node/context.h>
#include <util/check.h>
#include <walletinitinterface.h>

#include <memory>
#include <string>

using node::NodeContext;
using wallet::CWallet;

namespace coinjoin {
namespace {

class CoinJoinLoaderImpl : public interfaces::CoinJoin::Loader
{
private:
    CJWalletManager& manager()
    {
        return *Assert(m_node.cj_walletman);
    }

    interfaces::WalletLoader& wallet_loader()
    {
        return *Assert(m_node.wallet_loader);
    }

public:
    explicit CoinJoinLoaderImpl(NodeContext& node) :
        m_node(node)
    {
        // Enablement will be re-evaluated when a wallet is added or removed
        CCoinJoinClientOptions::SetEnabled(false);
    }

    void AddWallet(const std::shared_ptr<CWallet>& wallet) override
    {
        manager().addWallet(wallet);
        g_wallet_init_interface.InitCoinJoinSettings(*this, wallet_loader());
    }
    void RemoveWallet(const std::string& name) override
    {
        manager().removeWallet(name);
        g_wallet_init_interface.InitCoinJoinSettings(*this, wallet_loader());
    }
    void FlushWallet(const std::string& name) override
    {
        manager().flushWallet(name);
    }
    bool WithClient(const std::string& name, std::function<void(interfaces::CoinJoin::Client&)> func) override
    {
        return manager().doForClient(name, [&](CCoinJoinClientManager& mgr) { func(mgr); });
    }

    NodeContext& m_node;
};

} // namespace
} // namespace coinjoin

namespace interfaces {
std::unique_ptr<CoinJoin::Loader> MakeCoinJoinLoader(NodeContext& node) { return std::make_unique<coinjoin::CoinJoinLoaderImpl>(node); }
} // namespace interfaces
