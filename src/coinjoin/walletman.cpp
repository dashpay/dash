// Copyright (c) 2023-2025 The Dash Core developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#if defined(HAVE_CONFIG_H)
#include <config/bitcoin-config.h>
#endif

#include <coinjoin/walletman.h>
#include <evo/deterministicmns.h>
#include <net.h>
#include <scheduler.h>
#include <streams.h>

#ifdef ENABLE_WALLET
#include <coinjoin/client.h>
#endif // ENABLE_WALLET

#include <memory>
#include <ranges>

#ifdef ENABLE_WALLET
class CJWalletManagerImpl final : public CJWalletManager, public CoinJoinQueueNotify
{
public:
    CJWalletManagerImpl(ChainstateManager& chainman, CDeterministicMNManager& dmnman, CMasternodeMetaMan& mn_metaman,
                        CTxMemPool& mempool, const CMasternodeSync& mn_sync, const llmq::CInstantSendManager& isman,
                        bool relay_txes);
    ~CJWalletManagerImpl() override;

public:
    void Schedule(CConnman& connman, CScheduler& scheduler) override;

public:
    bool hasQueue(const uint256& hash) const override;
    CCoinJoinClientManager* getClient(const std::string& name) override;
    MessageProcessingResult processMessage(CNode& peer, CChainState& chainstate, CConnman& connman, CTxMemPool& mempool,
                                           std::string_view msg_type, CDataStream& vRecv) override;
    std::optional<CCoinJoinQueue> getQueueFromHash(const uint256& hash) const override;
    std::optional<int> getQueueSize() const override;
    std::vector<CDeterministicMNCPtr> getMixingMasternodes() override;
    void addWallet(const std::shared_ptr<wallet::CWallet>& wallet) override;
    void removeWallet(const std::string& name) override;
    void flushWallet(const std::string& name) override;

    // CoinJoinQueueNotify
    bool TrySubmitDenominate(const uint256& proTxHash, CConnman& connman) override;
    void MarkAlreadyJoinedQueueAsTried(CCoinJoinQueue& dsq) override;

protected:
    // CValidationInterface
    void UpdatedBlockTip(const CBlockIndex* pindexNew, const CBlockIndex* pindexFork, bool fInitialDownload) override;

private:
    const bool m_relay_txes;
    ChainstateManager& m_chainman;
    CDeterministicMNManager& m_dmnman;
    CMasternodeMetaMan& m_mn_metaman;
    CTxMemPool& m_mempool;
    const CMasternodeSync& m_mn_sync;
    const llmq::CInstantSendManager& m_isman;

    // queueman is declared before the wallet map so that it is destroyed
    // after all CCoinJoinClientManager instances (which hold a raw pointer to it).
    const std::unique_ptr<CCoinJoinClientQueueManager> m_queueman;

    mutable Mutex cs_wallet_manager_map;
    std::map<const std::string, std::unique_ptr<CCoinJoinClientManager>> m_wallet_manager_map GUARDED_BY(cs_wallet_manager_map);

    void DoMaintenance(CConnman& connman) EXCLUSIVE_LOCKS_REQUIRED(!cs_wallet_manager_map);

    template <typename Callable>
    void ForEachCJClientMan(Callable&& func) EXCLUSIVE_LOCKS_REQUIRED(!cs_wallet_manager_map)
    {
        LOCK(cs_wallet_manager_map);
        for (auto& [_, clientman] : m_wallet_manager_map) {
            func(*clientman);
        }
    }

    template <typename Callable>
    bool ForAnyCJClientMan(Callable&& func) EXCLUSIVE_LOCKS_REQUIRED(!cs_wallet_manager_map)
    {
        LOCK(cs_wallet_manager_map);
        return std::ranges::any_of(m_wallet_manager_map, [&](auto& pair) { return func(*pair.second); });
    }
};

CJWalletManagerImpl::CJWalletManagerImpl(ChainstateManager& chainman, CDeterministicMNManager& dmnman,
                                         CMasternodeMetaMan& mn_metaman, CTxMemPool& mempool,
                                         const CMasternodeSync& mn_sync, const llmq::CInstantSendManager& isman,
                                         bool relay_txes) :
    m_relay_txes{relay_txes},
    m_chainman{chainman},
    m_dmnman{dmnman},
    m_mn_metaman{mn_metaman},
    m_mempool{mempool},
    m_mn_sync{mn_sync},
    m_isman{isman},
    m_queueman{m_relay_txes ? std::make_unique<CCoinJoinClientQueueManager>(*this, dmnman, mn_metaman, mn_sync) : nullptr}
{
}

CJWalletManagerImpl::~CJWalletManagerImpl()
{
    LOCK(cs_wallet_manager_map);
    for (auto& [_, clientman] : m_wallet_manager_map) {
        clientman.reset();
    }
}

void CJWalletManagerImpl::Schedule(CConnman& connman, CScheduler& scheduler)
{
    if (!m_relay_txes) return;
    scheduler.scheduleEvery(std::bind(&CCoinJoinClientQueueManager::DoMaintenance, std::ref(*m_queueman)),
                            std::chrono::seconds{1});
    scheduler.scheduleEvery(std::bind(&CJWalletManagerImpl::DoMaintenance, this, std::ref(connman)),
                            std::chrono::seconds{1});
}

void CJWalletManagerImpl::UpdatedBlockTip(const CBlockIndex* pindexNew, const CBlockIndex* pindexFork, bool fInitialDownload)
{
    if (fInitialDownload || pindexNew == pindexFork) // In IBD or blocks were disconnected without any new ones
        return;

    ForEachCJClientMan([&pindexNew](CCoinJoinClientManager& clientman) { clientman.UpdatedBlockTip(pindexNew); });
}

bool CJWalletManagerImpl::hasQueue(const uint256& hash) const
{
    if (m_queueman) {
        return m_queueman->HasQueue(hash);
    }
    return false;
}

CCoinJoinClientManager* CJWalletManagerImpl::getClient(const std::string& name)
{
    LOCK(cs_wallet_manager_map);
    auto it = m_wallet_manager_map.find(name);
    return (it != m_wallet_manager_map.end()) ? it->second.get() : nullptr;
}

MessageProcessingResult CJWalletManagerImpl::processMessage(CNode& pfrom, CChainState& chainstate, CConnman& connman,
                                                            CTxMemPool& mempool, std::string_view msg_type,
                                                            CDataStream& vRecv)
{
    ForEachCJClientMan([&](CCoinJoinClientManager& clientman) {
        clientman.ProcessMessage(pfrom, chainstate, connman, mempool, msg_type, vRecv);
    });
    if (m_queueman) {
        return m_queueman->ProcessMessage(pfrom.GetId(), connman, msg_type, vRecv);
    }
    return {};
}

std::optional<CCoinJoinQueue> CJWalletManagerImpl::getQueueFromHash(const uint256& hash) const
{
    if (m_queueman) {
        return m_queueman->GetQueueFromHash(hash);
    }
    return std::nullopt;
}

std::optional<int> CJWalletManagerImpl::getQueueSize() const
{
    if (m_queueman) {
        return m_queueman->GetQueueSize();
    }
    return std::nullopt;
}

std::vector<CDeterministicMNCPtr> CJWalletManagerImpl::getMixingMasternodes()
{
    std::vector<CDeterministicMNCPtr> ret{};
    ForEachCJClientMan([&](const CCoinJoinClientManager& clientman) { clientman.GetMixingMasternodesInfo(ret); });
    return ret;
}

void CJWalletManagerImpl::addWallet(const std::shared_ptr<wallet::CWallet>& wallet)
{
    LOCK(cs_wallet_manager_map);
    m_wallet_manager_map.try_emplace(wallet->GetName(),
                                     std::make_unique<CCoinJoinClientManager>(wallet, m_dmnman, m_mn_metaman, m_mn_sync,
                                                                              m_isman, m_queueman.get()));
}

void CJWalletManagerImpl::flushWallet(const std::string& name)
{
    auto* clientman = Assert(getClient(name));
    clientman->ResetPool();
    clientman->StopMixing();
}

void CJWalletManagerImpl::removeWallet(const std::string& name)
{
    LOCK(cs_wallet_manager_map);
    m_wallet_manager_map.erase(name);
}

void CJWalletManagerImpl::DoMaintenance(CConnman& connman)
{
    LOCK(cs_wallet_manager_map);
    for (auto& [_, clientman] : m_wallet_manager_map) {
        clientman->DoMaintenance(m_chainman, connman, m_mempool);
    }
}

bool CJWalletManagerImpl::TrySubmitDenominate(const uint256& proTxHash, CConnman& connman)
{
    return ForAnyCJClientMan(
        [&](CCoinJoinClientManager& clientman) { return clientman.TrySubmitDenominate(proTxHash, connman); });
}

void CJWalletManagerImpl::MarkAlreadyJoinedQueueAsTried(CCoinJoinQueue& dsq)
{
    ForAnyCJClientMan([&](CCoinJoinClientManager& clientman) { return clientman.MarkAlreadyJoinedQueueAsTried(dsq); });
}
#endif // ENABLE_WALLET

std::unique_ptr<CJWalletManager> CJWalletManager::make(ChainstateManager& chainman, CDeterministicMNManager& dmnman,
                                                       CMasternodeMetaMan& mn_metaman, CTxMemPool& mempool,
                                                       const CMasternodeSync& mn_sync,
                                                       const llmq::CInstantSendManager& isman, bool relay_txes)
{
#ifdef ENABLE_WALLET
    return std::make_unique<CJWalletManagerImpl>(chainman, dmnman, mn_metaman, mempool, mn_sync, isman, relay_txes);
#else
    // Cannot be constructed if wallet support isn't built
    return nullptr;
#endif // ENABLE_WALLET
}
