// Copyright (c) 2009-2016 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef BITCOIN_RPC_REGISTER_H
#define BITCOIN_RPC_REGISTER_H

/** These are in one header file to avoid creating tons of single-function
 * headers for everything under src/rpc/ */
class CRPCCommand;
class CRPCTable;

template <typename T>
class Span;

void RegisterBlockchainRPCCommands(CRPCTable &tableRPC);
void RegisterFeeRPCCommands(CRPCTable&);
void RegisterMempoolRPCCommands(CRPCTable&);
void RegisterNetRPCCommands(CRPCTable &tableRPC);
void RegisterMiscRPCCommands(CRPCTable &tableRPC);
void RegisterMiningRPCCommands(CRPCTable &tableRPC);
void RegisterRawTransactionRPCCommands(CRPCTable &tableRPC);
void RegisterSignMessageRPCCommands(CRPCTable&);
/** Register raw transaction RPC commands */
void RegisterSignerRPCCommands(CRPCTable &tableRPC);
void RegisterTxoutProofRPCCommands(CRPCTable&);
void RegisterMasternodeRPCCommands(CRPCTable &tableRPC);
void RegisterCoinJoinRPCCommands(CRPCTable &tableRPC);
void RegisterGovernanceRPCCommands(CRPCTable &tableRPC);
void RegisterEvoRPCCommands(CRPCTable &tableRPC);
void RegisterQuorumsRPCCommands(CRPCTable &tableRPC);

#ifdef ENABLE_WALLET
// Dash-specific wallet-only RPC commands
Span<const CRPCCommand> GetWalletCoinJoinRPCCommands();
Span<const CRPCCommand> GetWalletEvoRPCCommands();
Span<const CRPCCommand> GetWalletGovernanceRPCCommands();
Span<const CRPCCommand> GetWalletMasternodeRPCCommands();
#endif // ENABLE_WALLET

static inline void RegisterAllCoreRPCCommands(CRPCTable &t)
{
    RegisterBlockchainRPCCommands(t);
    RegisterFeeRPCCommands(t);
    RegisterMempoolRPCCommands(t);
    RegisterNetRPCCommands(t);
    RegisterMiscRPCCommands(t);
    RegisterMiningRPCCommands(t);
    RegisterRawTransactionRPCCommands(t);
    RegisterSignMessageRPCCommands(t);
#ifdef ENABLE_EXTERNAL_SIGNER
    RegisterSignerRPCCommands(t);
#endif // ENABLE_EXTERNAL_SIGNER
    RegisterTxoutProofRPCCommands(t);
    RegisterMasternodeRPCCommands(t);
    RegisterCoinJoinRPCCommands(t);
    RegisterGovernanceRPCCommands(t);
    RegisterEvoRPCCommands(t);
    RegisterQuorumsRPCCommands(t);
}

#endif // BITCOIN_RPC_REGISTER_H
