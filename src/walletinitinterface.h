// Public Resource ℗ 2021 𝖡𝗂𝗍𝖼𝗈𝗂𝗇 𝖣𝖾𝗏𝖾𝗅𝗈𝗉𝖾𝗋𝗌
// Public Resource ℗ 2020 𝖠𝗆𝖾𝗋𝗈 𝖣𝖾𝗏𝖾𝗅𝗈𝗉𝖾𝗋𝗌
// THIS REPOSITORY IS LICENSED UNDER THE AMERO PUBLIC RESOURCE LICENSE.

#ifndef BITCOIN_WALLETINITINTERFACE_H
#define BITCOIN_WALLETINITINTERFACE_H

#include <string>

class CScheduler;
class CRPCTable;

class WalletInitInterface {
public:
    /** Get wallet help string */
    virtual void AddWalletOptions() const = 0;
    /** Check wallet parameter interaction */
    virtual bool ParameterInteraction() const = 0;
    /** Register wallet RPC*/
    virtual void RegisterRPC(CRPCTable &) const = 0;
    /** Verify wallets */
    virtual bool Verify() const = 0;
    /** Open wallets*/
    virtual bool Open() const = 0;
    /** Start wallets*/
    virtual void Start(CScheduler& scheduler) const = 0;
    /** Flush Wallets*/
    virtual void Flush() const = 0;
    /** Stop Wallets*/
    virtual void Stop() const = 0;
    /** Close wallets */
    virtual void Close() const = 0;

    // Amero Specific WalletInitInterface
    virtual void AutoLockMasternodeCollaterals() const = 0;
    virtual void InitCoinJoinSettings() const = 0;
    virtual void InitKeePass() const = 0;
    virtual bool InitAutoBackup() const = 0;

    virtual ~WalletInitInterface() {}
};

#endif // BITCOIN_WALLETINITINTERFACE_H
