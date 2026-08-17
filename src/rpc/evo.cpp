// Copyright (c) 2018-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <arith_uint256.h>
#include <bls/bls.h>
#include <chainparams.h>
#include <consensus/validation.h>
#include <core_io.h>
#include <deploymentstatus.h>
#include <evo/chainhelper.h>
#include <evo/deterministicmns.h>
#include <evo/dmn_types.h>
#include <evo/providertx.h>
#include <evo/providertx_service.h>
#include <evo/sharedcollateral.h>
#include <evo/smldiff.h>
#include <evo/specialtx.h>
#include <evo/specialtxman.h>
#include <index/txindex.h>
#include <interfaces/wallet.h>
#include <llmq/context.h>
#include <masternode/meta.h>
#include <node/context.h>
#include <node/transaction.h>
#include <rpc/server.h>
#include <rpc/server_util.h>
#include <rpc/util.h>
#include <util/check.h>
#include <util/strencodings.h>
#include <util/translation.h>
#include <validation.h>
#include <wallet/rpc/util.h>
#include <walletinitinterface.h>

#include <limits>
#include <optional>
#include <string_view>

#ifdef ENABLE_WALLET
#include <wallet/coincontrol.h>
#include <wallet/spend.h>
#include <wallet/wallet.h>

extern RPCHelpMan sendrawtransaction();
namespace wallet {
extern RPCHelpMan signrawtransactionwithwallet();
} // namespace wallet
#endif // ENABLE_WALLET

#ifndef ENABLE_WALLET
namespace wallet {
class CWallet;
} // namespace wallet
#endif // ENABLE_WALLET

using node::GetTransaction;
using node::NodeContext;
using wallet::CWallet;
#ifdef ENABLE_WALLET
using wallet::CCoinControl;
using wallet::CRecipient;
using wallet::DEFAULT_DISABLE_WALLET;
using wallet::GetWalletForJSONRPCRequest;
using wallet::HELP_REQUIRING_PASSPHRASE;
using wallet::isminetype;
using wallet::RANDOM_CHANGE_POSITION;
#endif // ENABLE_WALLET

// Defined here rather than with the other ToJson() in evo/core_write.cpp: dash-tx never prints
// a masternode entry, and the g_txindex lookup below needs libbitcoin_node anyway. Hosting it
// in evo/deterministicmns.cpp instead would create four new circular dependencies.
UniValue CDeterministicMN::ToJson() const
{
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("type", std::string(GetMnType(nType).description));
    obj.pushKV("proTxHash", proTxHash.ToString());
    obj.pushKV("collateralHash", collateralOutpoint.hash.ToString());
    obj.pushKV("collateralIndex", collateralOutpoint.n);

    if (g_txindex) {
        CTransactionRef collateralTx;
        uint256 nBlockHash;
        g_txindex->FindTx(collateralOutpoint.hash, nBlockHash, collateralTx);
        if (collateralTx) {
            CTxDestination dest;
            if (ExtractDestination(collateralTx->vout[collateralOutpoint.n].scriptPubKey, dest)) {
                obj.pushKV("collateralAddress", EncodeDestination(dest));
            }
        }
    }

    obj.pushKV("operatorReward", static_cast<double>(nOperatorReward) / 100);
    obj.pushKV("state", pdmnState->ToJson(nType));
    return obj;
}

static RPCArg GetRpcArg(const std::string& strParamName)
{
    static const std::map<std::string, RPCArg> mapParamHelp = {
        {"collateralAddress",
            {"collateralAddress", RPCArg::Type::STR, RPCArg::Optional::NO,
                "The Dash address to send the collateral to."}
        },
        {"collateralHash",
            {"collateralHash", RPCArg::Type::STR, RPCArg::Optional::NO,
                "The collateral transaction hash."}
        },
        {"collateralIndex",
            {"collateralIndex", RPCArg::Type::NUM, RPCArg::Optional::NO,
                "The collateral transaction output index."}
        },
        {"feeSourceAddress",
            {"feeSourceAddress", RPCArg::Type::STR, RPCArg::Default{""},
                "If specified wallet will only use coins from this address to fund ProTx.\n"
                "If not specified, payoutAddress is the one that is going to be used.\n"
                "The private key belonging to this address must be known in your wallet."}
        },
        {"fundAddress",
            {"fundAddress", RPCArg::Type::STR, RPCArg::Default{""},
                "If specified wallet will only use coins from this address to fund ProTx.\n"
                "If not specified, payoutAddress is the one that is going to be used.\n"
                "The private key belonging to this address must be known in your wallet."}
        },
        {"coreP2PAddrs",
            {"coreP2PAddrs", RPCArg::Type::ARR, RPCArg::Optional::NO,
                "Array of addresses in the form \"ADDR:PORT\". Must be unique on the network.\n"
                "A legacy ProTx can only store a single entry; storing multiple entries\n"
                "requires upgrading to a version 3 ProTx.\n"
                "Can be set to an empty string, which will require a ProUpServTx afterwards.",
                {
                    {"address", RPCArg::Type::STR, RPCArg::Optional::NO, ""},
                }}
        },
        {"coreP2PAddrs_update",
            {"coreP2PAddrs", RPCArg::Type::ARR, RPCArg::Optional::NO,
                "Array of addresses in the form \"ADDR:PORT\". Must be unique on the network.\n"
                "A legacy ProTx can only store a single entry; storing multiple entries\n"
                "requires upgrading to a version 3 ProTx.",
                {
                    {"address", RPCArg::Type::STR, RPCArg::Optional::NO, ""},
                }}
        },
        {"operatorKey",
            {"operatorKey", RPCArg::Type::STR, RPCArg::Optional::NO,
                "The operator BLS private key associated with the\n"
                "registered operator public key."}
        },
        {"operatorPayoutAddress",
            {"operatorPayoutAddress", RPCArg::Type::STR, RPCArg::Default{""},
                "The address used for operator reward payments.\n"
                "Only allowed when the ProRegTx had a non-zero operatorReward value.\n"
                "If set to an empty string, the currently active payout address is reused."}
        },
        {"operatorPubKey_register",
            {"operatorPubKey", RPCArg::Type::STR, RPCArg::Optional::NO,
                "The operator BLS public key. The BLS private key does not have to be known.\n"
                "It has to match the BLS private key which is later used when operating the masternode."}
        },
        {"operatorPubKey_register_legacy",
            {"operatorPubKey", RPCArg::Type::STR, RPCArg::Optional::NO,
                "The operator BLS public key in legacy scheme. The BLS private key does not have to be known.\n"
                "It has to match the BLS private key which is later used when operating the masternode.\n"}
        },
        {"operatorPubKey_update",
            {"operatorPubKey", RPCArg::Type::STR, RPCArg::Optional::NO,
                "The operator BLS public key. The BLS private key does not have to be known.\n"
                "It has to match the BLS private key which is later used when operating the masternode.\n"
                "If set to an empty string, the currently active operator BLS public key is reused."}
        },
        {"operatorPubKey_update_legacy",
            {"operatorPubKey", RPCArg::Type::STR, RPCArg::Optional::NO,
                "The operator BLS public key in legacy scheme. The BLS private key does not have to be known.\n"
                "It has to match the BLS private key which is later used when operating the masternode.\n"
                "If set to an empty string, the currently active operator BLS public key is reused."}
        },
        {"operatorReward",
            {"operatorReward", RPCArg::Type::STR, RPCArg::Optional::NO,
                "The fraction in %% to share with the operator.\n"
                "The value must be between 0 and 10000."}
        },
        {"ownerAddress",
            {"ownerAddress", RPCArg::Type::STR, RPCArg::Optional::NO,
                "The Dash address to use for payee updates and proposal voting.\n"
                "The corresponding private key does not have to be known by your wallet.\n"
                "The address must be unused and must differ from the collateralAddress."}
        },
        {"payoutAddress_register",
            {"payoutAddress", RPCArg::Type::ARR, RPCArg::Optional::NO,
                "The Dash address to use for masternode reward payments, or after v24 activation, "
                "an array of payout shares. Not compatible with legacy bls operator key.",
                {
                    {"", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
                        {
                            {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "The Dash payout address."},
                            {"reward", RPCArg::Type::NUM, RPCArg::Optional::NO, "The payout share in basis points."},
                        }},
                },
                RPCArgOptions{
                    .oneline_description={"\"payoutAddress\" | [{\"address\",\"reward\"},...] (string or array)"},
                    .type_str={"string or array", "string or array"},
                }}
        },
        {"payoutAddress_update",
            {"payoutAddress", RPCArg::Type::ARR, RPCArg::Optional::NO,
                "The Dash address to use for masternode reward payments, or after v24 activation, "
                "an array of payout shares. Not compatible with legacy bls operator key.\n"
                "If set to an empty string, the currently active payout address is reused.",
                {
                    {"", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
                        {
                            {"address", RPCArg::Type::STR, RPCArg::Optional::NO, "The Dash payout address."},
                            {"reward", RPCArg::Type::NUM, RPCArg::Optional::NO, "The payout share in basis points."},
                        }},
                },
                RPCArgOptions{
                    .oneline_description={"\"payoutAddress\" | [{\"address\",\"reward\"},...] (string or array)"},
                    .type_str={"string or array", "string or array"},
                }}
        },
        {"proTxHash",
            {"proTxHash", RPCArg::Type::STR, RPCArg::Optional::NO,
                "The hash of the initial ProRegTx."}
        },
        {"reason",
            {"reason", RPCArg::Type::NUM, RPCArg::DefaultHint{"Reason is not specified"},
                "The reason for masternode service revocation."}
        },
        {"submit",
            {"submit", RPCArg::Type::BOOL, RPCArg::Default{true},
                "If true, the resulting transaction is sent to the network."}
        },
        {"votingAddress_register",
            {"votingAddress", RPCArg::Type::STR, RPCArg::Optional::NO,
                "The voting key address. The private key does not have to be known by your wallet.\n"
                "It has to match the private key which is later used when voting on proposals.\n"
                "If set to an empty string, ownerAddress will be used."}
        },
        {"votingAddress_update",
            {"votingAddress", RPCArg::Type::STR, RPCArg::Optional::NO,
                "The voting key address. The private key does not have to be known by your wallet.\n"
                "It has to match the private key which is later used when voting on proposals.\n"
                "If set to an empty string, the currently active voting key address is reused."}
        },
        {"platformNodeID",
            {"platformNodeID", RPCArg::Type::STR, RPCArg::Optional::NO,
                "Platform P2P node ID, derived from P2P public key."}
        },
        {"platformP2PAddrs",
            {"platformP2PAddrs", RPCArg::Type::ARR, RPCArg::Optional::NO,
                "Array of addresses in the form \"ADDR:PORT\" used by Platform for peer-to-peer connection.\n"
                "For a legacy ProTx, pass a bare port number instead (e.g. 26656);\n"
                "the \"ADDR:PORT\" / address-array form requires upgrading to a version 3 ProTx.\n"
                "Must be unique on the network. Can be set to an empty string, which will require a ProUpServTx afterwards.",
                {
                    {"address", RPCArg::Type::STR, RPCArg::Optional::NO, ""},
                }}
        },
        {"platformP2PAddrs_update",
            {"platformP2PAddrs", RPCArg::Type::ARR, RPCArg::Optional::NO,
                "Array of addresses in the form \"ADDR:PORT\" used by Platform for peer-to-peer connection.\n"
                "For a legacy ProTx, pass a bare port number instead (e.g. 26656);\n"
                "the \"ADDR:PORT\" / address-array form requires upgrading to a version 3 ProTx.\n"
                "Must be unique on the network.",
                {
                    {"address", RPCArg::Type::STR, RPCArg::Optional::NO, ""},
                }}
        },
        {"platformHTTPSAddrs",
            {"platformHTTPSAddrs", RPCArg::Type::ARR, RPCArg::Optional::NO,
                "Array of addresses in the form \"ADDR:PORT\" used by Platform for their HTTPS API.\n"
                "For a legacy ProTx, pass a bare port number instead (e.g. 443);\n"
                "the \"ADDR:PORT\" / address-array form requires upgrading to a version 3 ProTx.\n"
                "Must be unique on the network. Can be set to an empty string, which will require a ProUpServTx afterwards.",
                {
                    {"address", RPCArg::Type::STR, RPCArg::Optional::NO, ""},
                }}
        },
        {"platformHTTPSAddrs_update",
            {"platformHTTPSAddrs", RPCArg::Type::ARR, RPCArg::Optional::NO,
                "Array of addresses in the form \"ADDR:PORT\" used by Platform for their HTTPS API.\n"
                "For a legacy ProTx, pass a bare port number instead (e.g. 443);\n"
                "the \"ADDR:PORT\" / address-array form requires upgrading to a version 3 ProTx.\n"
                "Must be unique on the network.",
                {
                    {"address", RPCArg::Type::STR, RPCArg::Optional::NO, ""},
                }}
        },
    };

    auto it = mapParamHelp.find(strParamName);
    if (it == mapParamHelp.end())
        throw std::runtime_error(strprintf("FIXME: WRONG PARAM NAME %s!", strParamName));

    return it->second;
}

static CBLSSecretKey ParseBLSSecretKey(const std::string& hexKey, const std::string& paramName)
{
    CBLSSecretKey secKey;

    // Actually, bool flag for bls::PrivateKey has other meaning (modOrder)
    if (!secKey.SetHexStr(hexKey, false)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("%s must be a valid BLS secret key", paramName));
    }
    return secKey;
}

#ifdef ENABLE_WALLET
// The shared-masternode helpers and RPCs below all feed into wallet-gated signing (protx
// shared_sign / dissolve), so they are only compiled with wallet support.

static CollateralShares ParseShares(const UniValue& value, const std::string& paramName)
{
    if (!value.isArray()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("%s must be an array", paramName));
    }
    const auto& arr = value.get_array();
    if (arr.size() < CProRegTx::MIN_SHARES || arr.size() > CProRegTx::MAX_SHARES) {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
                           strprintf("%s must contain between %d and %d entries", paramName, CProRegTx::MIN_SHARES,
                                     CProRegTx::MAX_SHARES));
    }
    CollateralShares shares;
    for (size_t i = 0; i < arr.size(); ++i) {
        const UniValue& entry = arr[i];
        if (!entry.isObject()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("%s entries must be objects", paramName));
        }
        const UniValue& amount_value = entry.find_value("amount");
        const UniValue& refund_value = entry.find_value("refundAddress");
        const UniValue& reward_value = entry.find_value("rewardAddress");
        const UniValue& owner_value = entry.find_value("ownerAddress");
        if (!amount_value.isNum() || !refund_value.isStr() || !owner_value.isStr()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                               strprintf("%s entries must include numeric amount, refundAddress and ownerAddress", paramName));
        }
        const CAmount amount = amount_value.getInt<int64_t>();
        CTxDestination refund_dest = DecodeDestination(refund_value.get_str());
        if (!IsValidDestination(refund_dest)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, strprintf("invalid refund address: %s", refund_value.get_str()));
        }
        CScript script_reward;
        if (reward_value.isStr() && !reward_value.get_str().empty()) {
            CTxDestination reward_dest = DecodeDestination(reward_value.get_str());
            if (!IsValidDestination(reward_dest)) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, strprintf("invalid reward address: %s", reward_value.get_str()));
            }
            script_reward = GetScriptForDestination(reward_dest);
        }
        CTxDestination owner_dest = DecodeDestination(owner_value.get_str());
        const PKHash* owner_pkhash = std::get_if<PKHash>(&owner_dest);
        if (!owner_pkhash) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, strprintf("invalid share owner address: %s", owner_value.get_str()));
        }
        shares.emplace_back(amount, GetScriptForDestination(refund_dest), script_reward, ToKeyID(*owner_pkhash));
    }
    return shares;
}

//! Builds an unsigned ProDisTx transaction paying each non-actor share its principal plus its
//! pro-rata slice of the penalty (sequential floor, remainder to the last non-actor entry, which
//! always satisfies the consensus minimums), with the actor absorbing penalty and fee
static CMutableTransaction BuildProDisTx(const CDeterministicMN& dmn, uint16_t actorIndex, CAmount penalty,
                                         CAmount fee, CProDisTx& ptxRet)
{
    const auto& shares = dmn.pdmnState->shares;
    if (actorIndex >= shares.size()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "actorIndex out of range");
    }
    if (fee <= 0) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "fee must be positive");
    }
    if (fee > CProDisTx::MAX_FEE) {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
                           strprintf("fee exceeds the consensus ceiling of %d duffs", CProDisTx::MAX_FEE));
    }
    const CAmount actor_output = shares[actorIndex].amount - penalty - fee;
    if (actor_output < 0) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "penalty and fee exceed the actor's share");
    }

    CMutableTransaction tx;
    tx.nVersion = 3;
    tx.nType = TRANSACTION_PROVIDER_DISSOLVE;
    tx.vin.emplace_back(dmn.collateralOutpoint);

    CAmount non_actor_total{0};
    size_t last_non_actor{0};
    for (size_t i = 0; i < shares.size(); i++) {
        if (i != actorIndex) {
            non_actor_total += shares[i].amount;
            last_non_actor = i;
        }
    }
    CAmount distributed{0};
    for (size_t i = 0; i < shares.size(); i++) {
        if (i == actorIndex) continue;
        CAmount bonus;
        if (i == last_non_actor) {
            bonus = penalty - distributed;
        } else {
            arith_uint256 v{static_cast<uint64_t>(penalty)};
            v *= arith_uint256{static_cast<uint64_t>(shares[i].amount)};
            v /= arith_uint256{static_cast<uint64_t>(non_actor_total)};
            bonus = static_cast<CAmount>(v.GetLow64());
        }
        distributed += bonus;
        tx.vout.emplace_back(shares[i].amount + bonus, shares[i].scriptRefund);
    }
    if (actor_output > 0) {
        tx.vout.emplace_back(actor_output, shares[actorIndex].scriptRefund);
    }

    ptxRet = CProDisTx();
    ptxRet.proTxHash = dmn.proTxHash;
    ptxRet.actorIndex = actorIndex;
    return tx;
}

template <typename SpecialTxPayload>
static void FundSpecialTx(CWallet& wallet, CMutableTransaction& tx, const SpecialTxPayload& payload,
                          const CTxDestination& fundDest) EXCLUSIVE_LOCKS_REQUIRED(!wallet.cs_wallet)
{
    // Make sure the results are valid at least up to the most recent block
    // the user could have gotten from another RPC command prior to now
    wallet.BlockUntilSyncedToCurrentChain();

    LOCK(wallet.cs_wallet);

    CTxDestination nodest = CNoDestination();
    if (fundDest == nodest) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "No source of funds specified");
    }

    CDataStream ds(SER_NETWORK, PROTOCOL_VERSION);
    ds << payload;
    tx.vExtraPayload.assign(UCharCast(ds.data()), UCharCast(ds.data() + ds.size()));

    static const CTxOut dummyTxOut(0, CScript() << OP_RETURN);
    std::vector<CRecipient> vecSend;
    bool dummyTxOutAdded = false;

    if (tx.vout.empty()) {
        // add dummy txout as CreateTransaction requires at least one recipient
        tx.vout.emplace_back(dummyTxOut);
        dummyTxOutAdded = true;
    }

    for (const auto& txOut : tx.vout) {
        CRecipient recipient = {txOut.scriptPubKey, txOut.nValue, false};
        vecSend.push_back(recipient);
    }

    CCoinControl coinControl;
    coinControl.destChange = fundDest;
    coinControl.fRequireAllInputs = false;

    for (const auto& out : AvailableCoinsListUnspent(wallet).all()) {
        CTxDestination txDest;
        if (ExtractDestination(out.txout.scriptPubKey, txDest) && txDest == fundDest) {
            coinControl.Select(out.outpoint);
        }
    }

    if (!coinControl.HasSelected()) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, strprintf("No funds at specified address %s", EncodeDestination(fundDest)));
    }

    auto res = CreateTransaction(wallet, vecSend, RANDOM_CHANGE_POSITION, coinControl, /*sign=*/true, tx.vExtraPayload.size());
    if (!res) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, util::ErrorString(res).original);
    }

    const CTransactionRef& newTx = res->tx;
    tx.vin = newTx->vin;
    tx.vout = newTx->vout;

    if (dummyTxOutAdded && tx.vout.size() > 1) {
        // CreateTransaction added a change output, so we don't need the dummy txout anymore.
        // Removing it results in slight overpayment of fees, but we ignore this for now (as it's a very low amount).
        auto it = std::find(tx.vout.begin(), tx.vout.end(), dummyTxOut);
        CHECK_NONFATAL(it != tx.vout.end());
        tx.vout.erase(it);
    }
}

template <typename SpecialTxPayload>
static void UpdateSpecialTxInputsHash(const CMutableTransaction& tx, SpecialTxPayload& payload)
{
    payload.inputsHash = CalcTxInputsHash(CTransaction(tx));
}

template <typename SpecialTxPayload>
static void SignSpecialTxPayloadByHash(const CMutableTransaction& tx, SpecialTxPayload& payload, const CKeyID& keyID, const CWallet& wallet)
{
    UpdateSpecialTxInputsHash(tx, payload);
    payload.vchSig.clear();

    const uint256 hash = ::SerializeHash(payload);
    if (!wallet.SignSpecialTxPayload(hash, keyID, payload.vchSig)) {
        throw JSONRPCError(RPC_INTERNAL_ERROR, "failed to sign special tx");
    }
}

static std::string SignAndSendSpecialTx(const JSONRPCRequest& request, CChainstateHelper& chain_helper, const ChainstateManager& chainman, const CMutableTransaction& tx, bool fSubmit)
{
    {
    LOCK(::cs_main);

    const CBlockIndex* tip{chainman.ActiveChain().Tip()};
    const Consensus::Params& consensus_params{chainman.GetConsensus()};
    if (!DeploymentActiveAfter(tip, consensus_params, Consensus::DEPLOYMENT_DIP0003)) {
        const int current_height{tip ? tip->nHeight : -1};
        const int next_block_height{current_height + 1};
        const int activation_height{consensus_params.DIP0003Height};
        const int blocks_to_mine{
            activation_height > next_block_height ? activation_height - next_block_height : 0
        };
        throw JSONRPCError(RPC_VERIFY_ERROR, strprintf(
            "DIP0003 is not active yet; ProTx transactions are valid starting at block height %d "
            "(current chain height %d, next block height %d). Mine %d more block%s or restart "
            "this regtest/devnet chain with DIP3 activation parameters that are already active.",
            activation_height, current_height, next_block_height, blocks_to_mine,
            blocks_to_mine == 1 ? "" : "s"));
    }

    TxValidationState state;
    if (!chain_helper.special_tx->CheckSpecialTx(CTransaction(tx), tip, chainman.ActiveChainstate().CoinsTip(), true, state)) {
        throw std::runtime_error(state.ToString());
    }
    } // cs_main

    CDataStream ds(SER_NETWORK, PROTOCOL_VERSION);
    ds << tx;

    JSONRPCRequest signRequest(request);
    signRequest.params.setArray();
    signRequest.params.push_back(HexStr(ds));
    UniValue signResult = wallet::signrawtransactionwithwallet().HandleRequest(signRequest);

    if (!fSubmit) {
        return signResult["hex"].get_str();
    }

    // Submitting a partially signed transaction would only fail later in the mempool with an
    // opaque script-verification error, so fail here with the actual cause. The typical way to
    // hit this is running "protx shared_combine" on a wallet other than the one whose coins fund
    // the transaction: combining changes the payload, which invalidates the fee-input signatures,
    // and only the funding wallet can re-sign them.
    if (!signResult["complete"].get_bool()) {
        std::string sign_errors;
        if (const UniValue& errors = signResult["errors"]; errors.isArray()) {
            for (const auto& err : errors.getValues()) {
                if (!sign_errors.empty()) sign_errors += ", ";
                sign_errors += err["error"].get_str();
            }
        }
        throw JSONRPCError(RPC_WALLET_ERROR,
                           strprintf("transaction inputs could not be fully signed by this wallet; run this "
                                     "command on the wallet that funded the transaction, or finish signing "
                                     "there with signrawtransactionwithwallet (%s)", sign_errors));
    }

    JSONRPCRequest sendRequest(request);
    sendRequest.params.setArray();
    sendRequest.params.push_back(signResult["hex"].get_str());
    return ::sendrawtransaction().HandleRequest(sendRequest).get_str();
}

static std::string SubmitSpecialTx(const JSONRPCRequest& request, CChainstateHelper& chain_helper,
                                   const ChainstateManager& chainman, const CMutableTransaction& tx)
{
    {
        LOCK(::cs_main);
        TxValidationState state;
        if (!chain_helper.special_tx->CheckSpecialTx(CTransaction(tx), chainman.ActiveChain().Tip(),
                                                     chainman.ActiveChainstate().CoinsTip(), true, state)) {
            throw std::runtime_error(state.ToString());
        }
    }
    JSONRPCRequest sendRequest(request);
    sendRequest.params.setArray();
    sendRequest.params.push_back(EncodeHexTx(CTransaction(tx)));
    return ::sendrawtransaction().HandleRequest(sendRequest).get_str();
}

static CKeyID ParsePubKeyIDFromAddress(const std::string& strAddress, const std::string& paramName)
{
    CTxDestination dest = DecodeDestination(strAddress);
    const PKHash *pkhash = std::get_if<PKHash>(&dest);
    if (!pkhash) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("%s must be a valid P2PKH address, not %s", paramName, strAddress));
    }
    return ToKeyID(*pkhash);
}

static CBLSPublicKey ParseBLSPubKey(const std::string& hexKey, const std::string& paramName, bool specific_legacy_bls_scheme)
{
    CBLSPublicKey pubKey;
    if (!pubKey.SetHexStr(hexKey, specific_legacy_bls_scheme)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("%s must be a valid BLS public key, not %s", paramName, hexKey));
    }
    return pubKey;
}

static std::vector<interfaces::ProviderPayout> ParsePayouts(const UniValue& value, const std::string& paramName)
{
    std::vector<interfaces::ProviderPayout> payouts;
    if (value.isArray()) {
        const auto& arr = value.get_array();
        if (arr.empty()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("%s must contain at least one entry", paramName));
        }
        if (arr.size() > 8) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("%s must not contain more than 8 entries", paramName));
        }
        for (size_t i = 0; i < arr.size(); ++i) {
            const UniValue& entry = arr[i];
            if (!entry.isObject()) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("%s entries must be objects", paramName));
            }
            const UniValue& address_value = entry.find_value("address");
            const UniValue& reward_value = entry.find_value("reward");
            if (!address_value.isStr() || !reward_value.isNum()) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("%s entries must include address and numeric reward", paramName));
            }
            CTxDestination dest = DecodeDestination(address_value.get_str());
            if (!IsValidDestination(dest)) {
                throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, strprintf("invalid payout address: %s", address_value.get_str()));
            }
            const int64_t reward = reward_value.getInt<int64_t>();
            if (reward < 0 || reward > std::numeric_limits<uint16_t>::max()) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "payout reward out of range");
            }
            payouts.push_back({dest, static_cast<uint16_t>(reward)});
        }
    } else {
        CTxDestination destination{DecodeDestination(value.get_str())};
        if (!IsValidDestination(destination)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, strprintf("invalid payout address: %s", value.get_str()));
        }
        payouts.push_back({destination, MasternodePayoutShare::MAX_REWARD});
    }
    return payouts;
}

static std::unique_ptr<interfaces::Wallet> MakeWalletInterface(const NodeContext& node,
                                                               const std::shared_ptr<CWallet>& wallet)
{
    auto* wallet_loader{CHECK_NONFATAL(node.wallet_loader)};
    return interfaces::MakeWallet(*CHECK_NONFATAL(wallet_loader->context()), wallet);
}

[[noreturn]] static void ThrowProviderTxError(const interfaces::ProviderTxError& provider_error)
{
    using interfaces::ProviderTxErrorCode;
    switch (provider_error.code) {
    case ProviderTxErrorCode::INVALID_PARAMETER:
        throw JSONRPCError(RPC_INVALID_PARAMETER, provider_error.message.original);
    case ProviderTxErrorCode::INVALID_ADDRESS_OR_KEY:
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, provider_error.message.original);
    case ProviderTxErrorCode::FUNDING_ERROR:
        // Preserve the historical protx RPC error code while exposing a useful typed category.
        throw JSONRPCError(RPC_INTERNAL_ERROR, provider_error.message.original);
    case ProviderTxErrorCode::WALLET_ERROR:
        throw JSONRPCError(RPC_WALLET_ERROR, provider_error.message.original);
    case ProviderTxErrorCode::WALLET_UNLOCK_NEEDED:
        throw JSONRPCError(RPC_WALLET_UNLOCK_NEEDED, provider_error.message.original);
    case ProviderTxErrorCode::INTERNAL_ERROR:
        throw JSONRPCError(RPC_INTERNAL_ERROR, provider_error.message.original);
    case ProviderTxErrorCode::VERIFY_ERROR:
        throw JSONRPCError(RPC_VERIFY_ERROR, provider_error.message.original);
    case ProviderTxErrorCode::CONSENSUS_REJECTED:
        throw std::runtime_error(provider_error.message.original);
    case ProviderTxErrorCode::BROADCAST_ERROR:
        throw JSONRPCTransactionError(*CHECK_NONFATAL(provider_error.transaction_error), provider_error.message.original);
    }
    NONFATAL_UNREACHABLE();
}

static std::string SubmissionToString(const interfaces::ProviderTxSubmission& submission)
{
    return submission.submitted ? submission.tx->GetHash().GetHex() : EncodeHexTx(*submission.tx);
}

template <typename T>
static T UnwrapOrThrow(interfaces::ProviderTxResult<T> result)
{
    if (const auto* error{std::get_if<interfaces::ProviderTxError>(&result)}) ThrowProviderTxError(*error);
    return std::get<T>(std::move(result));
}

static std::optional<CTxDestination> ParseFeeSource(const UniValue& param)
{
    if (param.isNull()) return std::nullopt;
    CTxDestination fee_source{DecodeDestination(param.get_str())};
    if (!IsValidDestination(fee_source)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid Dash address: ") + param.get_str());
    }
    return fee_source;
}

static std::vector<std::string> ParseCoreNetInfo(const UniValue& input, bool optional)
{
    if (input.isStr()) {
        if (input.get_str().empty()) {
            if (!optional) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid param for coreP2PAddrs[0], cannot be empty");
            }
            return {};
        }
        return {input.get_str()};
    }
    if (!input.isArray()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid param for coreP2PAddrs, must be string or array");
    }
    const auto& entries{input.get_array()};
    if (!optional && entries.empty()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "Invalid param for coreP2PAddrs, cannot be empty");
    }
    std::vector<std::string> result;
    result.reserve(entries.size());
    for (size_t index{0}; index < entries.size(); ++index) {
        if (!entries[index].isStr()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                               strprintf("Invalid param for coreP2PAddrs[%zu], must be string", index));
        }
        result.push_back(entries[index].get_str());
    }
    return result;
}

template <typename ProTx>
static void ProcessNetInfoCore(ProTx& ptx, const UniValue& input, const bool optional)
{
    CHECK_NONFATAL(ptx.netInfo);
    const auto entries{ParseCoreNetInfo(input, optional)};
    for (size_t idx{0}; idx < entries.size(); ++idx) {
        if (auto ret = ptx.netInfo->AddEntry(NetInfoPurpose::CORE_P2P, entries[idx]); ret != NetInfoStatus::Success) {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                               strprintf("Error setting coreP2PAddrs[%zu] to '%s' (%s)", idx, entries[idx], NISToString(ret)));
        }
    }
}

static interfaces::ProviderPlatformEndpoints ParsePlatformNetInfo(const UniValue& input, std::string_view field_name,
                                                                  bool extended_addresses)
{
    if (!input.isArray() && !input.isNum() && !input.isStr()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
                           strprintf("Invalid param for %s, must be array, number or string", field_name));
    }
    if (input.isArray()) {
        std::vector<std::string> result;
        const auto& entries{input.get_array()};
        if (!entries.empty() && !extended_addresses) {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                               strprintf("Invalid param for %s, this ProTx version only accepts a bare port number; "
                                         "the "
                                         "\"ADDR:PORT\" / address-array form requires upgrading to a version 3 ProTx",
                                         field_name));
        }
        result.reserve(entries.size());
        for (size_t index{0}; index < entries.size(); ++index) {
            if (!entries[index].isStr() || entries[index].get_str().find_first_not_of("0123456789") == std::string::npos) {
                throw JSONRPCError(RPC_INVALID_PARAMETER,
                                   strprintf("Invalid param for %s[%zu], must be string", field_name, index));
            }
            result.push_back(entries[index].get_str());
        }
        return result;
    }
    const std::string& value{input.getValStr()};
    if (value.empty()) return std::monostate{};
    int32_t port{0};
    if (ParseInt32(value, &port) && port >= 1 && port <= std::numeric_limits<uint16_t>::max()) {
        return static_cast<uint16_t>(port);
    }
    if (input.isStr() && value.find_first_not_of("0123456789") != std::string::npos)
        return std::vector<std::string>{value};
    throw JSONRPCError(RPC_INVALID_PARAMETER,
                       strprintf("Invalid param for %s, must be a valid port [1-65535]", field_name));
}

// forward declaration
namespace {
enum class ProTxRegisterAction
{
    External,
    Fund,
    Prepare,
};
} // anonymous namespace

static UniValue protx_register_common_wrapper(const JSONRPCRequest& request,
                                              const bool specific_legacy_bls_scheme,
                                              ProTxRegisterAction action,
                                              const MnType mnType);

static UniValue protx_update_service_common_wrapper(const JSONRPCRequest& request, const MnType mnType);


static RPCHelpMan protx_register_fund_wrapper(const bool legacy)
{
    std::string rpc_name = legacy ? "register_fund_legacy" : "register_fund";
    std::string rpc_full_name = std::string("protx ").append(rpc_name);
    std::string pubkey_operator = legacy ? "\"0532646990082f4fd639f90387b1551f2c7c39d37392cb9055a06a7e85c1d23692db8f87f827886310bccc1e29db9aee\"" : "\"8532646990082f4fd639f90387b1551f2c7c39d37392cb9055a06a7e85c1d23692db8f87f827886310bccc1e29db9aee\"";
    std::string rpc_example = rpc_name.append(" \"" + EXAMPLE_ADDRESS[0] + "\" \"1.2.3.4:1234\" \"" + EXAMPLE_ADDRESS[1] + "\" ").append(pubkey_operator).append(" \"" + EXAMPLE_ADDRESS[1] + "\" 0 \"" + EXAMPLE_ADDRESS[0] + "\"");
    return RPCHelpMan{rpc_full_name,
        "\nCreates, funds and sends a ProTx to the network. The resulting transaction will move 1000 Dash\n"
        "to the address specified by collateralAddress and will then function as the collateral of your\n"
        "masternode.\n"
        "A few of the limitations you see in the arguments are temporary and might be lifted after DIP3\n"
        "is fully deployed.\n"
        + std::string(legacy ? "\nDEPRECATED: May be removed in a future version, pass config option -deprecatedrpc=legacy_mn to use RPC\n" : "")
        + HELP_REQUIRING_PASSPHRASE,
        {
            GetRpcArg("collateralAddress"),
            GetRpcArg("coreP2PAddrs"),
            GetRpcArg("ownerAddress"),
            legacy ? GetRpcArg("operatorPubKey_register_legacy") : GetRpcArg("operatorPubKey_register"),
            GetRpcArg("votingAddress_register"),
            GetRpcArg("operatorReward"),
            GetRpcArg("payoutAddress_register"),
            GetRpcArg("fundAddress"),
            GetRpcArg("submit"),
        },
        {
            RPCResult{"if \"submit\" is not set or set to true",
                RPCResult::Type::STR_HEX, "txid", "The transaction id"},
            RPCResult{"if \"submit\" is set to false",
                RPCResult::Type::STR_HEX, "hex", "The serialized signed ProTx in hex format"},
        },
        RPCExamples{
            HelpExampleCli("protx",  rpc_example)
        },
        [legacy](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    if (legacy && !IsDeprecatedRPCEnabled("legacy_mn")) {
        throw std::runtime_error("DEPRECATED: Pass config option -deprecatedrpc=legacy_mn to enable this RPC");
    }
    return protx_register_common_wrapper(request, self.m_name == "protx register_fund_legacy", ProTxRegisterAction::Fund, MnType::Regular);
},
    };
}

static RPCHelpMan protx_register_fund() {
    return protx_register_fund_wrapper(false);
}

static RPCHelpMan protx_register_fund_legacy() {
    return protx_register_fund_wrapper(true);
}

static RPCHelpMan protx_register_wrapper(bool legacy)
{
    std::string rpc_name = legacy ? "register_legacy" : "register";
    std::string rpc_full_name = std::string("protx ").append(rpc_name);
    std::string pubkey_operator = legacy ? "\"0532646990082f4fd639f90387b1551f2c7c39d37392cb9055a06a7e85c1d23692db8f87f827886310bccc1e29db9aee\"" : "\"8532646990082f4fd639f90387b1551f2c7c39d37392cb9055a06a7e85c1d23692db8f87f827886310bccc1e29db9aee\"";
    std::string rpc_example = rpc_name.append(" \"0123456701234567012345670123456701234567012345670123456701234567\" 0 \"1.2.3.4:1234\" \"" + EXAMPLE_ADDRESS[1] + "\" ").append(pubkey_operator).append(" \"" + EXAMPLE_ADDRESS[1] + "\" 0 \"" + EXAMPLE_ADDRESS[0] + "\"");
    return RPCHelpMan{rpc_full_name,
        "\nSame as \"protx register_fund\", but with an externally referenced collateral.\n"
        "The collateral is specified through \"collateralHash\" and \"collateralIndex\" and must be an unspent\n"
        "transaction output spendable by this wallet. It must also not be used by any other masternode.\n"
        + std::string(legacy ? "\nDEPRECATED: May be removed in a future version, pass config option -deprecatedrpc=legacy_mn to use RPC\n" : "")
        + HELP_REQUIRING_PASSPHRASE,
        {
            GetRpcArg("collateralHash"),
            GetRpcArg("collateralIndex"),
            GetRpcArg("coreP2PAddrs"),
            GetRpcArg("ownerAddress"),
            legacy ? GetRpcArg("operatorPubKey_register_legacy") : GetRpcArg("operatorPubKey_register"),
            GetRpcArg("votingAddress_register"),
            GetRpcArg("operatorReward"),
            GetRpcArg("payoutAddress_register"),
            GetRpcArg("feeSourceAddress"),
            GetRpcArg("submit"),
        },
        {
            RPCResult{"if \"submit\" is not set or set to true",
                RPCResult::Type::STR_HEX, "txid", "The transaction id"},
            RPCResult{"if \"submit\" is set to false",
                RPCResult::Type::STR_HEX, "hex", "The serialized signed ProTx in hex format"},
        },
        RPCExamples{
            HelpExampleCli("protx", rpc_example),
        },
        [legacy](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    if (legacy && !IsDeprecatedRPCEnabled("legacy_mn")) {
        throw std::runtime_error("DEPRECATED: Pass config option -deprecatedrpc=legacy_mn to enable this RPC");
    }
    return protx_register_common_wrapper(request, self.m_name == "protx register_legacy", ProTxRegisterAction::External, MnType::Regular);
},
    };
}

static RPCHelpMan protx_register()
{
    return protx_register_wrapper(false);
}

static RPCHelpMan protx_register_legacy()
{
    return protx_register_wrapper(true);
}

static RPCHelpMan protx_register_prepare_wrapper(const bool legacy)
{
    std::string rpc_name = legacy ? "register_prepare_legacy" : "register_prepare";
    std::string rpc_full_name = std::string("protx ").append(rpc_name);
    std::string pubkey_operator = legacy ? "\"0532646990082f4fd639f90387b1551f2c7c39d37392cb9055a06a7e85c1d23692db8f87f827886310bccc1e29db9aee\"" : "\"8532646990082f4fd639f90387b1551f2c7c39d37392cb9055a06a7e85c1d23692db8f87f827886310bccc1e29db9aee\"";
    std::string rpc_example = rpc_name.append(" \"0123456701234567012345670123456701234567012345670123456701234567\" 0 \"1.2.3.4:1234\" \"" + EXAMPLE_ADDRESS[1] + "\" ").append(pubkey_operator).append(" \"" + EXAMPLE_ADDRESS[1] + "\" 0 \"" + EXAMPLE_ADDRESS[0] + "\"");
    return RPCHelpMan{rpc_full_name,
        "\nCreates an unsigned ProTx and a message that must be signed externally\n"
        "with the private key that corresponds to collateralAddress to prove collateral ownership.\n"
        "The prepared transaction will also contain inputs and outputs to cover fees.\n"
        + std::string(legacy ? "\nDEPRECATED: May be removed in a future version, pass config option -deprecatedrpc=legacy_mn to use RPC\n" : ""),
        {
            GetRpcArg("collateralHash"),
            GetRpcArg("collateralIndex"),
            GetRpcArg("coreP2PAddrs"),
            GetRpcArg("ownerAddress"),
            legacy ? GetRpcArg("operatorPubKey_register_legacy") : GetRpcArg("operatorPubKey_register"),
            GetRpcArg("votingAddress_register"),
            GetRpcArg("operatorReward"),
            GetRpcArg("payoutAddress_register"),
            GetRpcArg("feeSourceAddress"),
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::STR_HEX, "tx", "The serialized unsigned ProTx in hex format"},
                {RPCResult::Type::STR_HEX, "collateralAddress", "The collateral address"},
                {RPCResult::Type::STR_HEX, "signMessage", "The string message that needs to be signed with the collateral key"},
            }},
        RPCExamples{
            HelpExampleCli("protx", rpc_example)
        },
        [legacy](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    if (legacy && !IsDeprecatedRPCEnabled("legacy_mn")) {
        throw std::runtime_error("DEPRECATED: Pass config option -deprecatedrpc=legacy_mn to enable this RPC");
    }
    return protx_register_common_wrapper(request, self.m_name == "protx register_prepare_legacy", ProTxRegisterAction::Prepare, MnType::Regular);
},
    };
}

static RPCHelpMan protx_register_prepare()
{
    return protx_register_prepare_wrapper(false);
}

static RPCHelpMan protx_register_prepare_legacy()
{
    return protx_register_prepare_wrapper(true);
}

static RPCHelpMan protx_register_fund_evo()
{
    const std::string command_name{"protx register_fund_evo"};
    return RPCHelpMan{
        command_name,
        "\nCreates, funds and sends a ProTx to the network. The resulting transaction will move 4000 Dash\n"
        "to the address specified by collateralAddress and will then function as the collateral of your\n"
        "EvoNode.\n"
        "A few of the limitations you see in the arguments are temporary and might be lifted after DIP3\n"
        "is fully deployed.\n" +
            HELP_REQUIRING_PASSPHRASE,
        {
            GetRpcArg("collateralAddress"),
            GetRpcArg("coreP2PAddrs"),
            GetRpcArg("ownerAddress"),
            GetRpcArg("operatorPubKey_register"),
            GetRpcArg("votingAddress_register"),
            GetRpcArg("operatorReward"),
            GetRpcArg("payoutAddress_register"),
            GetRpcArg("platformNodeID"),
            GetRpcArg("platformP2PAddrs"),
            GetRpcArg("platformHTTPSAddrs"),
            GetRpcArg("fundAddress"),
            GetRpcArg("submit"),
        },
        {
            RPCResult{"if \"submit\" is not set or set to true",
                RPCResult::Type::STR_HEX, "txid", "The transaction id"},
            RPCResult{"if \"submit\" is set to false",
                RPCResult::Type::STR_HEX, "hex", "The serialized signed ProTx in hex format"},
        },
        RPCExamples{
            HelpExampleCli("protx", "register_fund_evo \"" + EXAMPLE_ADDRESS[0] + "\" \"1.2.3.4:1234\" \"" + EXAMPLE_ADDRESS[1] + "\" \"93746e8731c57f87f79b3620a7982924e2931717d49540a85864bd543de11c43fb868fd63e501a1db37e19ed59ae6db4\" \"" + EXAMPLE_ADDRESS[1] + "\" 0 \"" + EXAMPLE_ADDRESS[0] + "\" \"f2dbd9b0a1f541a7c44d34a58674d0262f5feca5\" 22821 22822")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    return protx_register_common_wrapper(request, false, ProTxRegisterAction::Fund, MnType::Evo);
},
    };
}

static RPCHelpMan protx_register_evo()
{
    const std::string command_name{"protx register_evo"};
    return RPCHelpMan{
        command_name,
        "\nSame as \"protx register_fund_evo\", but with an externally referenced collateral.\n"
        "The collateral is specified through \"collateralHash\" and \"collateralIndex\" and must be an unspent\n"
        "transaction output spendable by this wallet. It must also not be used by any other masternode.\n" +
            HELP_REQUIRING_PASSPHRASE,
        {
            GetRpcArg("collateralHash"),
            GetRpcArg("collateralIndex"),
            GetRpcArg("coreP2PAddrs"),
            GetRpcArg("ownerAddress"),
            GetRpcArg("operatorPubKey_register"),
            GetRpcArg("votingAddress_register"),
            GetRpcArg("operatorReward"),
            GetRpcArg("payoutAddress_register"),
            GetRpcArg("platformNodeID"),
            GetRpcArg("platformP2PAddrs"),
            GetRpcArg("platformHTTPSAddrs"),
            GetRpcArg("feeSourceAddress"),
            GetRpcArg("submit"),
        },
        {
            RPCResult{"if \"submit\" is not set or set to true",
                RPCResult::Type::STR_HEX, "txid", "The transaction id"},
            RPCResult{"if \"submit\" is set to false",
                RPCResult::Type::STR_HEX, "hex", "The serialized signed ProTx in hex format"},
        },
        RPCExamples{
            HelpExampleCli("protx", "register_evo \"0123456701234567012345670123456701234567012345670123456701234567\" 0 \"1.2.3.4:1234\" \"" + EXAMPLE_ADDRESS[1] + "\" \"93746e8731c57f87f79b3620a7982924e2931717d49540a85864bd543de11c43fb868fd63e501a1db37e19ed59ae6db4\" \"" + EXAMPLE_ADDRESS[1] + "\" 0 \"" + EXAMPLE_ADDRESS[0] + "\" \"f2dbd9b0a1f541a7c44d34a58674d0262f5feca5\" 22821 22822")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    return protx_register_common_wrapper(request, false, ProTxRegisterAction::External, MnType::Evo);
},
    };
}

static RPCHelpMan protx_register_prepare_evo()
{
    const std::string command_name{"protx register_prepare_evo"};
    return RPCHelpMan{
        command_name,
        "\nCreates an unsigned ProTx and a message that must be signed externally\n"
        "with the private key that corresponds to collateralAddress to prove collateral ownership.\n"
        "The prepared transaction will also contain inputs and outputs to cover fees.\n",
        {
            GetRpcArg("collateralHash"),
            GetRpcArg("collateralIndex"),
            GetRpcArg("coreP2PAddrs"),
            GetRpcArg("ownerAddress"),
            GetRpcArg("operatorPubKey_register"),
            GetRpcArg("votingAddress_register"),
            GetRpcArg("operatorReward"),
            GetRpcArg("payoutAddress_register"),
            GetRpcArg("platformNodeID"),
            GetRpcArg("platformP2PAddrs"),
            GetRpcArg("platformHTTPSAddrs"),
            GetRpcArg("feeSourceAddress"),
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "", {
                                              {RPCResult::Type::STR_HEX, "tx", "The serialized unsigned ProTx in hex format"},
                                              {RPCResult::Type::STR_HEX, "collateralAddress", "The collateral address"},
                                              {RPCResult::Type::STR_HEX, "signMessage", "The string message that needs to be signed with the collateral key"},
                                          }},
        RPCExamples{HelpExampleCli("protx", "register_prepare_evo \"0123456701234567012345670123456701234567012345670123456701234567\" 0 \"1.2.3.4:1234\" \"" + EXAMPLE_ADDRESS[1] + "\" \"93746e8731c57f87f79b3620a7982924e2931717d49540a85864bd543de11c43fb868fd63e501a1db37e19ed59ae6db4\" \"" + EXAMPLE_ADDRESS[1] + "\" 0 \"" + EXAMPLE_ADDRESS[0] + "\" \"f2dbd9b0a1f541a7c44d34a58674d0262f5feca5\" 22821 22822")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    return protx_register_common_wrapper(request, false, ProTxRegisterAction::Prepare, MnType::Evo);
},
    };
}

static UniValue protx_register_common_wrapper(const JSONRPCRequest& request,
                                              const bool specific_legacy_bls_scheme,
                                              const ProTxRegisterAction action,
                                              const MnType mnType)
{
    NodeContext& node{EnsureAnyNodeContext(request.context)};
    std::shared_ptr<CWallet> const pwallet{GetWalletForJSONRPCRequest(request)};
    if (!pwallet) return UniValue::VNULL;
    EnsureWalletIsUnlocked(*pwallet);

    interfaces::ProviderRegistrationRequest typed_request;
    typed_request.type = mnType;
    typed_request.use_legacy_bls_scheme = specific_legacy_bls_scheme;
    size_t paramIdx{0};
    if (action == ProTxRegisterAction::Fund) {
        CTxDestination collateral_destination{DecodeDestination(request.params[paramIdx].get_str())};
        if (!IsValidDestination(collateral_destination)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY,
                               strprintf("invalid collateral address: %s", request.params[paramIdx].get_str()));
        }
        typed_request.collateral = interfaces::FundProviderCollateral{collateral_destination};
        paramIdx++;
    } else {
        const uint256 collateral_hash{ParseHashV(request.params[paramIdx], "collateralHash")};
        const int32_t collateral_index{request.params[paramIdx + 1].getInt<int>()};
        if (collateral_hash.IsNull() || collateral_index < 0) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY,
                               strprintf("invalid hash or index: %s-%d", collateral_hash.ToString(), collateral_index));
        }
        typed_request.collateral = interfaces::ExistingProviderCollateral{
            COutPoint{collateral_hash, static_cast<uint32_t>(collateral_index)}};
        paramIdx += 2;
    }

    typed_request.net_info.core_p2p = ParseCoreNetInfo(request.params[paramIdx], /*optional=*/true);
    typed_request.owner_key = ParsePubKeyIDFromAddress(request.params[paramIdx + 1].get_str(), "owner address");
    typed_request.operator_key = ParseBLSPubKey(request.params[paramIdx + 2].get_str(), "operator BLS address",
                                                specific_legacy_bls_scheme);
    typed_request.voting_key = typed_request.owner_key;
    if (!request.params[paramIdx + 3].get_str().empty()) {
        typed_request.voting_key = ParsePubKeyIDFromAddress(request.params[paramIdx + 3].get_str(), "voting address");
    }

    int64_t operatorReward;
    if (!ParseFixedPoint(request.params[paramIdx + 4].getValStr(), 2, &operatorReward)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "operatorReward must be a number");
    }
    if (operatorReward < 0 || operatorReward > 10000) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "operatorReward must be between 0 and 10000");
    }
    typed_request.operator_reward = static_cast<uint16_t>(operatorReward);
    const auto capabilities{evo::provider::GetCapabilities(node, /*basic_override=*/!specific_legacy_bls_scheme)};
    typed_request.uses_extended_payouts = request.params[paramIdx + 5].isArray();
    if (typed_request.uses_extended_payouts && !capabilities.extended_addresses) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "payouts array requires provider transaction version 3");
    }
    typed_request.payouts = ParsePayouts(request.params[paramIdx + 5], "payouts");

    if (mnType == MnType::Evo) {
        if (!IsHex(request.params[paramIdx + 6].get_str())) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "platformNodeID must be hexadecimal string");
        }
        uint160 platform_node_id;
        platform_node_id.SetHex(request.params[paramIdx + 6].get_str());
        typed_request.platform_node_id = platform_node_id;
        typed_request.net_info.platform_p2p = ParsePlatformNetInfo(request.params[paramIdx + 7], "platformP2PAddrs",
                                                                   capabilities.extended_addresses);
        typed_request.net_info.platform_https = ParsePlatformNetInfo(request.params[paramIdx + 8], "platformHTTPSAddrs",
                                                                     capabilities.extended_addresses);
        paramIdx += 3;
    }

    typed_request.fee_source = ParseFeeSource(request.params[paramIdx + 6]);
    if ((action == ProTxRegisterAction::External || action == ProTxRegisterAction::Fund) &&
        !request.params[paramIdx + 7].isNull()) {
        typed_request.submit = ParseBoolV(request.params[paramIdx + 7], "submit");
    }

    auto wallet_interface{MakeWalletInterface(node, pwallet)};
    if (action == ProTxRegisterAction::Prepare) {
        const auto prepared{UnwrapOrThrow(evo::provider::PrepareRegistration(node, *wallet_interface, typed_request))};
        UniValue response{UniValue::VOBJ};
        response.pushKV("tx", EncodeHexTx(*prepared.tx));
        response.pushKV("collateralAddress", EncodeDestination(prepared.collateral_address));
        response.pushKV("signMessage", prepared.sign_message);
        return response;
    }
    return SubmissionToString(UnwrapOrThrow(evo::provider::Register(node, *wallet_interface, typed_request)));
}

static RPCHelpMan protx_register_submit()
{
    return RPCHelpMan{
        "protx register_submit",
        "\nCombines the unsigned ProTx and a signature of the signMessage, signs all inputs\n"
        "which were added to cover fees and submits the resulting transaction to the network.\n"
        "Note: See \"help protx register_prepare\" for more info about creating a ProTx and a message to sign.\n" +
            HELP_REQUIRING_PASSPHRASE,
        {
            {"tx", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The serialized unsigned ProTx in hex format."},
            {"sig", RPCArg::Type::STR, RPCArg::Optional::NO,
             "The signature signed with the collateral key. Must be in base64 format."},
        },
        RPCResult{RPCResult::Type::STR_HEX, "txid", "The transaction id"},
        RPCExamples{HelpExampleCli("protx", "register_submit \"tx\" \"sig\"")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue {
            NodeContext& node{EnsureAnyNodeContext(request.context)};
            const std::shared_ptr<CWallet> wallet{GetWalletForJSONRPCRequest(request)};
            if (!wallet) return UniValue::VNULL;
            EnsureWalletIsUnlocked(*wallet);

            CMutableTransaction tx;
            if (!DecodeHexTx(tx, request.params[0].get_str())) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "transaction not deserializable");
            }
            auto opt_vchSig{DecodeBase64(request.params[1].get_str())};
            if (!opt_vchSig.has_value()) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "malformed base64 encoding");
            }

            auto wallet_interface{MakeWalletInterface(node, wallet)};
            return SubmissionToString(UnwrapOrThrow(evo::provider::SubmitRegistration(
                node, *wallet_interface, MakeTransactionRef(std::move(tx)), *opt_vchSig)));
        },
    };
}

static RPCHelpMan protx_update_service()
{
    return RPCHelpMan{"protx update_service",
        "\nCreates and sends a ProUpServTx to the network. This will update the IP address\n"
        "of a masternode.\n"
        "If this is done for a masternode that got PoSe-banned, the ProUpServTx will also revive this masternode.\n"
        + HELP_REQUIRING_PASSPHRASE,
        {
            GetRpcArg("proTxHash"),
            GetRpcArg("coreP2PAddrs_update"),
            GetRpcArg("operatorKey"),
            GetRpcArg("operatorPayoutAddress"),
            GetRpcArg("feeSourceAddress"),
            GetRpcArg("submit"),
        },
        {
            RPCResult{"if \"submit\" is not set or set to true",
                RPCResult::Type::STR_HEX, "txid", "The transaction id"},
            RPCResult{"if \"submit\" is set to false",
                RPCResult::Type::STR_HEX, "hex", "The serialized signed ProTx in hex format"},
        },
        RPCExamples{
            HelpExampleCli("protx", "update_service \"0123456701234567012345670123456701234567012345670123456701234567\" \"1.2.3.4:1234\" 5a2e15982e62f1e0b7cf9783c64cf7e3af3f90a52d6c40f6f95d624c0b1621cd")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    return protx_update_service_common_wrapper(request, MnType::Regular);
},
    };
}

static RPCHelpMan protx_update_service_evo()
{
    const std::string command_name{"protx update_service_evo"};
    return RPCHelpMan{
        command_name,
        "\nCreates and sends a ProUpServTx to the network. This will update the IP address and the Platform fields\n"
        "of an EvoNode.\n"
        "If this is done for an EvoNode that got PoSe-banned, the ProUpServTx will also revive this EvoNode.\n" +
            HELP_REQUIRING_PASSPHRASE,
        {
            GetRpcArg("proTxHash"),
            GetRpcArg("coreP2PAddrs_update"),
            GetRpcArg("operatorKey"),
            GetRpcArg("platformNodeID"),
            GetRpcArg("platformP2PAddrs_update"),
            GetRpcArg("platformHTTPSAddrs_update"),
            GetRpcArg("operatorPayoutAddress"),
            GetRpcArg("feeSourceAddress"),
            GetRpcArg("submit"),
        },
        {
            RPCResult{"if \"submit\" is not set or set to true",
                RPCResult::Type::STR_HEX, "txid", "The transaction id"},
            RPCResult{"if \"submit\" is set to false",
                RPCResult::Type::STR_HEX, "hex", "The serialized signed ProTx in hex format"},
        },
        RPCExamples{
            HelpExampleCli("protx", "update_service_evo \"0123456701234567012345670123456701234567012345670123456701234567\" \"1.2.3.4:1234\" \"5a2e15982e62f1e0b7cf9783c64cf7e3af3f90a52d6c40f6f95d624c0b1621cd\" \"f2dbd9b0a1f541a7c44d34a58674d0262f5feca5\" 22821 22822")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    return protx_update_service_common_wrapper(request, MnType::Evo);
},
    };
}

static UniValue protx_update_service_common_wrapper(const JSONRPCRequest& request, const MnType mnType)
{
    NodeContext& node{EnsureAnyNodeContext(request.context)};
    std::shared_ptr<CWallet> const wallet{GetWalletForJSONRPCRequest(request)};
    if (!wallet) return UniValue::VNULL;
    EnsureWalletIsUnlocked(*wallet);

    interfaces::ProviderUpdateServiceRequest typed_request;
    typed_request.type = mnType;
    typed_request.pro_tx_hash = ParseHashV(request.params[0], "proTxHash");
    typed_request.net_info.core_p2p = ParseCoreNetInfo(request.params[1], /*optional=*/false);
    typed_request.operator_key = ParseBLSSecretKey(request.params[2].get_str(), "operatorKey");

    size_t paramIdx{3};
    if (mnType == MnType::Evo) {
        const auto capabilities{evo::provider::GetCapabilities(node)};
        if (!IsHex(request.params[paramIdx].get_str())) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "platformNodeID must be hexadecimal string");
        }
        uint160 platform_node_id;
        platform_node_id.SetHex(request.params[paramIdx].get_str());
        typed_request.platform_node_id = platform_node_id;
        typed_request.net_info.platform_p2p = ParsePlatformNetInfo(request.params[paramIdx + 1], "platformP2PAddrs",
                                                                   capabilities.extended_addresses);
        typed_request.net_info.platform_https = ParsePlatformNetInfo(request.params[paramIdx + 2], "platformHTTPSAddrs",
                                                                     capabilities.extended_addresses);
        paramIdx += 3;
    }

    if (!request.params[paramIdx].isNull() && !request.params[paramIdx].get_str().empty()) {
        CTxDestination payout_destination{DecodeDestination(request.params[paramIdx].get_str())};
        if (!IsValidDestination(payout_destination)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY,
                               strprintf("invalid operator payout address: %s", request.params[paramIdx].get_str()));
        }
        typed_request.operator_payout = payout_destination;
    }
    typed_request.fee_source = ParseFeeSource(request.params[paramIdx + 1]);
    if (!request.params[paramIdx + 2].isNull()) {
        typed_request.submit = ParseBoolV(request.params[paramIdx + 2], "submit");
    }

    auto wallet_interface{MakeWalletInterface(node, wallet)};
    return SubmissionToString(UnwrapOrThrow(evo::provider::UpdateService(node, *wallet_interface, typed_request)));
}

static RPCHelpMan protx_update_registrar_wrapper(const bool specific_legacy_bls_scheme)
{
    std::string rpc_name = specific_legacy_bls_scheme ? "update_registrar_legacy" : "update_registrar";
    std::string rpc_full_name = std::string("protx ").append(rpc_name);
    std::string pubkey_operator = specific_legacy_bls_scheme ? "\"0532646990082f4fd639f90387b1551f2c7c39d37392cb9055a06a7e85c1d23692db8f87f827886310bccc1e29db9aee\"" : "\"8532646990082f4fd639f90387b1551f2c7c39d37392cb9055a06a7e85c1d23692db8f87f827886310bccc1e29db9aee\"";
    std::string rpc_example = rpc_name.append(" \"0123456701234567012345670123456701234567012345670123456701234567\" ").append(pubkey_operator).append(" \"" + EXAMPLE_ADDRESS[1] + "\"");
    return RPCHelpMan{
        rpc_full_name,
        "\nCreates and sends a ProUpRegTx to the network. This will update the operator key, voting key and payout\n"
        "address of the masternode specified by \"proTxHash\".\n"
        "The owner key of the masternode must be known to your wallet.\n" +
            std::string(specific_legacy_bls_scheme ? "\nDEPRECATED: May be removed in a future version, pass config "
                                                     "option -deprecatedrpc=legacy_mn to use RPC\n"
                                                   : "") +
            HELP_REQUIRING_PASSPHRASE,
        {
            GetRpcArg("proTxHash"),
            specific_legacy_bls_scheme ? GetRpcArg("operatorPubKey_update_legacy") : GetRpcArg("operatorPubKey_update"),
            GetRpcArg("votingAddress_update"),
            GetRpcArg("payoutAddress_update"),
            GetRpcArg("feeSourceAddress"),
            GetRpcArg("submit"),
        },
        {
            RPCResult{"if \"submit\" is not set or set to true", RPCResult::Type::STR_HEX, "txid", "The transaction id"},
            RPCResult{"if \"submit\" is set to false", RPCResult::Type::STR_HEX, "hex",
                      "The serialized signed ProTx in hex format"},
        },
        RPCExamples{HelpExampleCli("protx", rpc_example)},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue {
            const bool use_legacy{self.m_name == "protx update_registrar_legacy"};
            if (use_legacy && !IsDeprecatedRPCEnabled("legacy_mn")) {
                throw std::runtime_error("DEPRECATED: Pass config option -deprecatedrpc=legacy_mn to enable this RPC");
            }

            NodeContext& node{EnsureAnyNodeContext(request.context)};
            std::shared_ptr<CWallet> const wallet{GetWalletForJSONRPCRequest(request)};
            if (!wallet) return UniValue::VNULL;
            EnsureWalletIsUnlocked(*wallet);

            interfaces::ProviderUpdateRegistrarRequest typed_request;
            typed_request.pro_tx_hash = ParseHashV(request.params[0], "proTxHash");
            typed_request.use_legacy_bls_scheme = use_legacy;
            if (!request.params[1].get_str().empty()) {
                typed_request.operator_key = ParseBLSPubKey(request.params[1].get_str(), "operator BLS address",
                                                            use_legacy);
            }
            if (!request.params[2].get_str().empty()) {
                typed_request.voting_key = ParsePubKeyIDFromAddress(request.params[2].get_str(), "voting address");
            }
            if (request.params[3].isArray() || !request.params[3].get_str().empty()) {
                typed_request.uses_extended_payouts = request.params[3].isArray();
                if (typed_request.uses_extended_payouts &&
                    !evo::provider::GetCapabilities(node, /*basic_override=*/!use_legacy).extended_addresses) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, "payouts array requires provider transaction version 3");
                }
                typed_request.payouts = ParsePayouts(request.params[3], "payouts");
            }
            typed_request.fee_source = ParseFeeSource(request.params[4]);
            if (!request.params[5].isNull()) {
                typed_request.submit = ParseBoolV(request.params[5], "submit");
            }

            auto wallet_interface{MakeWalletInterface(node, wallet)};
            return SubmissionToString(
                UnwrapOrThrow(evo::provider::UpdateRegistrar(node, *wallet_interface, typed_request)));
        },
    };
}

static RPCHelpMan protx_update_registrar()
{
    return protx_update_registrar_wrapper(false);
}

static RPCHelpMan protx_update_registrar_legacy()
{
    return protx_update_registrar_wrapper(true);
}

static RPCHelpMan protx_revoke()
{
    return RPCHelpMan{
        "protx revoke",
        "\nCreates and sends a ProUpRevTx to the network. This will revoke the operator key of the masternode and\n"
        "put it into the PoSe-banned state. It will also set the service field of the masternode\n"
        "to zero. Use this in case your operator key got compromised or you want to stop providing your service\n"
        "to the masternode owner.\n" +
            HELP_REQUIRING_PASSPHRASE,
        {
            GetRpcArg("proTxHash"),
            GetRpcArg("operatorKey"),
            GetRpcArg("reason"),
            GetRpcArg("feeSourceAddress"),
            GetRpcArg("submit"),
        },
        {
            RPCResult{"if \"submit\" is not set or set to true", RPCResult::Type::STR_HEX, "txid", "The transaction id"},
            RPCResult{"if \"submit\" is set to false", RPCResult::Type::STR_HEX, "hex",
                      "The serialized signed ProTx in hex format"},
        },
        RPCExamples{HelpExampleCli("protx",
                                   "revoke \"0123456701234567012345670123456701234567012345670123456701234567\" "
                                   "\"072f36a77261cdd5d64c32d97bac417540eddca1d5612f416feb07ff75a8e240\"")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue {
            NodeContext& node{EnsureAnyNodeContext(request.context)};
            std::shared_ptr<CWallet> const pwallet{GetWalletForJSONRPCRequest(request)};
            if (!pwallet) return UniValue::VNULL;
            EnsureWalletIsUnlocked(*pwallet);

            interfaces::ProviderRevokeRequest typed_request;
            typed_request.pro_tx_hash = ParseHashV(request.params[0], "proTxHash");
            typed_request.operator_key = ParseBLSSecretKey(request.params[1].get_str(), "operatorKey");
            if (!request.params[2].isNull()) {
                const int32_t nReason{request.params[2].getInt<int>()};
                if (nReason < 0 || nReason > CProUpRevTx::REASON_LAST) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("invalid reason %d, must be between 0 and %d",
                                                                        nReason, CProUpRevTx::REASON_LAST));
                }
                typed_request.reason = static_cast<uint16_t>(nReason);
            }
            typed_request.fee_source = ParseFeeSource(request.params[3]);
            if (!request.params[4].isNull()) {
                typed_request.submit = ParseBoolV(request.params[4], "submit");
            }

            auto wallet_interface{MakeWalletInterface(node, pwallet)};
            return SubmissionToString(UnwrapOrThrow(evo::provider::Revoke(node, *wallet_interface, typed_request)));
        },
    };
}

static RPCHelpMan protx_shared_sign()
{
    return RPCHelpMan{"protx shared_sign",
        "\nSigns a shared masternode transaction (shared ProRegTx, unanimous ProDisTx or ProUpSharedRegTx)\n"
        "with every share owner key this wallet holds and returns the produced signatures. The transaction\n"
        "itself is not modified; pass the signatures to \"protx shared_combine\".\n"
        "Only the multi-party flows pass through here: a unilateral dissolution is built, signed and\n"
        "submitted in one step by \"protx dissolve\" and never needs shared_sign.\n"
        + HELP_REQUIRING_PASSPHRASE,
        {
            {"tx", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The serialized transaction in hex format."},
            {"allowTimeLocks", RPCArg::Type::BOOL, RPCArg::Default{false}, "Sign a dissolution carrying a lock time or non-final input sequence. The signed digest commits to these fields, so a lock a co-signer failed to notice delays when the dissolution can confirm."},
        },
        RPCResult{RPCResult::Type::ARR, "", "",
        {
            {RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::NUM, "shareIndex", "Index into the share table"},
                {RPCResult::Type::STR, "signature", "Base64-encoded signature by this share's owner key"},
            }},
        }},
        RPCExamples{HelpExampleCli("protx", "shared_sign \"tx\"")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const NodeContext& node = EnsureAnyNodeContext(request.context);
    CDeterministicMNManager& dmnman = *CHECK_NONFATAL(node.dmnman);

    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return UniValue::VNULL;
    EnsureWalletIsUnlocked(*pwallet);

    CMutableTransaction tx;
    if (!DecodeHexTx(tx, request.params[0].get_str())) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "transaction not deserializable");
    }

    // Resolve the share table and the digest to sign from the transaction type
    CollateralShares shares;
    uint256 sign_hash;
    if (tx.nType == TRANSACTION_PROVIDER_REGISTER) {
        const auto opt_ptx = GetTxPayload<CProRegTx>(tx);
        if (!opt_ptx || !opt_ptx->IsShared()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "transaction is not a shared masternode registration");
        }
        shares = opt_ptx->shares;
        sign_hash = opt_ptx->MakeSharedRegConsentHash(CTransaction(tx));
    } else if (tx.nType == TRANSACTION_PROVIDER_DISSOLVE) {
        const auto opt_ptx = GetTxPayload<CProDisTx>(tx);
        if (!opt_ptx) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "transaction payload not deserializable");
        }
        // shared_sign only ever signs unanimous dissolutions (built unsigned by dissolve_prepare);
        // the signing digest commits to the signature count, so signatures produced here would
        // never verify on a transaction carrying a different count. A unilateral dissolution is
        // built, signed and submitted in one step by "protx dissolve" and must not pass through
        // here at all.
        if (!opt_ptx->vchSigs.empty()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                               "transaction already carries dissolution signatures; a unilateral dissolution "
                               "is created fully signed by \"protx dissolve\" and needs no shared_sign step");
        }
        const auto dmn = dmnman.GetListAtChainTip().GetMN(opt_ptx->proTxHash);
        if (!dmn || !dmn->pdmnState->IsShared()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "shared masternode not found");
        }
        // Consensus permits time-locked dissolutions and the digest commits to the lock fields, so
        // a lock the signer failed to notice would be silently baked into their signature
        const bool allow_time_locks{request.params[1].isNull() ? false
                                                               : ParseBoolV(request.params[1], "allowTimeLocks")};
        bool has_time_lock{tx.nLockTime != 0};
        for (const auto& txin : tx.vin) {
            has_time_lock |= txin.nSequence != CTxIn::SEQUENCE_FINAL;
        }
        if (has_time_lock && !allow_time_locks) {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                               "dissolution carries a lock time or non-final input sequence, which delays when "
                               "it can confirm; pass allowTimeLocks=true to sign it anyway");
        }
        shares = dmn->pdmnState->shares;
        sign_hash = opt_ptx->MakeSignHash(CTransaction(tx), static_cast<uint8_t>(shares.size()));
    } else if (tx.nType == TRANSACTION_PROVIDER_UPDATE_SHARED_REGISTRAR) {
        const auto opt_ptx = GetTxPayload<CProUpSharedRegTx>(tx);
        if (!opt_ptx) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "transaction payload not deserializable");
        }
        const auto dmn = dmnman.GetListAtChainTip().GetMN(opt_ptx->proTxHash);
        if (!dmn || !dmn->pdmnState->IsShared()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "shared masternode not found");
        }
        shares = dmn->pdmnState->shares;
        sign_hash = ::SerializeHash(*opt_ptx);
    } else {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "transaction is not a shared masternode transaction");
    }

    UniValue ret(UniValue::VARR);
    for (size_t i = 0; i < shares.size(); i++) {
        std::vector<unsigned char> vchSig;
        if (!pwallet->SignSpecialTxPayload(sign_hash, shares[i].keyIDOwner, vchSig)) {
            continue; // this wallet does not hold this share's owner key
        }
        UniValue entry(UniValue::VOBJ);
        entry.pushKV("shareIndex", static_cast<uint64_t>(i));
        entry.pushKV("signature", EncodeBase64(vchSig));
        ret.push_back(entry);
    }
    if (ret.empty()) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "none of the share owner keys were found in this wallet");
    }
    return ret;
},
    };
}

static RPCHelpMan protx_dissolve()
{
    return RPCHelpMan{"protx dissolve",
        "\nCreates, signs and optionally submits a unilateral ProDisTx that dissolves a shared masternode,\n"
        "refunding every participant's principal to its immutable refund script. During the early period a\n"
        "unilateral dissolution pays the configured penalty, redistributed pro-rata to the other shares.\n"
        "The wallet must hold the actor share's owner key. With submit=false the signed transaction hex is\n"
        "returned instead, which can be stored offline as a standby dissolution: ProDisTx validity is\n"
        "monotone, so a transaction that is valid now stays valid forever.\n"
        + HELP_REQUIRING_PASSPHRASE,
        {
            {"proTxHash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The hash of the initial ProRegTx."},
            {"actorIndex", RPCArg::Type::NUM, RPCArg::Optional::NO, "Index into the share table of the dissolving participant."},
            {"fee", RPCArg::Type::NUM, RPCArg::Default{100000}, "Transaction fee in duffs, paid from the actor's share. At most 1000000 duffs (consensus ceiling)."},
            {"submit", RPCArg::Type::BOOL, RPCArg::Default{true}, "Submit the transaction to the network."},
            {"payPenalty", RPCArg::Type::BOOL, RPCArg::DefaultHint{"determined by the current height"}, "Pay the early-period penalty. Pass true to build a standby valid at any height, false for one valid only after the early period ends."},
        },
        RPCResult{RPCResult::Type::STR_HEX, "result", "The transaction id if submitted, otherwise the signed transaction hex"},
        RPCExamples{HelpExampleCli("protx", "dissolve \"proTxHash\" 0")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const NodeContext& node = EnsureAnyNodeContext(request.context);
    const ChainstateManager& chainman = EnsureChainman(node);
    CDeterministicMNManager& dmnman = *CHECK_NONFATAL(node.dmnman);
    CChainstateHelper& chain_helper = *CHECK_NONFATAL(node.chain_helper);

    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return UniValue::VNULL;
    EnsureWalletIsUnlocked(*pwallet);

    const uint256 proTxHash(ParseHashV(request.params[0], "proTxHash"));
    const int actorIndex{request.params[1].getInt<int>()};
    if (actorIndex < 0 || actorIndex > std::numeric_limits<uint16_t>::max()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "actorIndex out of range");
    }
    const CAmount fee{request.params[2].isNull() ? 100000 : request.params[2].getInt<int64_t>()};
    const bool fSubmit{request.params[3].isNull() ? true : ParseBoolV(request.params[3], "submit")};

    const auto dmn = dmnman.GetListAtChainTip().GetMN(proTxHash);
    if (!dmn || !dmn->pdmnState->IsShared()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "shared masternode not found");
    }

    const bool early{[&]() {
        const int next_height{WITH_LOCK(::cs_main, return chainman.ActiveChain().Height()) + 1};
        return next_height - dmn->pdmnState->nRegisteredHeight <
               static_cast<int64_t>(dmn->pdmnState->nEarlyPeriodBlocks);
    }()};
    const bool pay_penalty{request.params[4].isNull() ? early : ParseBoolV(request.params[4], "payPenalty")};
    if (early && !pay_penalty) {
        // Deliberate: a zero-penalty standby signed during the early period becomes valid at the
        // boundary. Warn via error only when it would also be submitted now.
        if (fSubmit) {
            throw JSONRPCError(RPC_INVALID_PARAMETER,
                               "a unilateral dissolution during the early period must pay the penalty; "
                               "pass submit=false to build a standby for later");
        }
    }

    CProDisTx ptx;
    CMutableTransaction tx = BuildProDisTx(*dmn, static_cast<uint16_t>(actorIndex),
                                           pay_penalty ? dmn->pdmnState->nEarlyPenalty : 0, fee, ptx);

    std::vector<unsigned char> vchSig;
    if (!pwallet->SignSpecialTxPayload(ptx.MakeSignHash(CTransaction(tx), /*sig_count=*/1),
                                       dmn->pdmnState->shares[actorIndex].keyIDOwner, vchSig)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY,
                           "private key for the actor share's owner address not found in this wallet");
    }
    ptx.vchSigs = {vchSig};
    SetTxPayload(tx, ptx);

    if (!fSubmit) {
        return EncodeHexTx(CTransaction(tx));
    }
    return SubmitSpecialTx(request, chain_helper, chainman, tx);
},
    };
}

static RPCHelpMan protx_update_share()
{
    return RPCHelpMan{"protx update_share",
        "\nCreates and sends a ProUpShareTx updating one collateral share's reward script. Only the share's\n"
        "owner key (which this wallet must hold) can update it; all other share fields are immutable.\n"
        + HELP_REQUIRING_PASSPHRASE,
        {
            {"proTxHash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The hash of the initial ProRegTx."},
            {"shareIndex", RPCArg::Type::NUM, RPCArg::Optional::NO, "Index into the share table of the share to update."},
            {"rewardAddress", RPCArg::Type::STR, RPCArg::Optional::NO, "The new reward address, or an empty string to pay rewards to the refund script again."},
            {"feeSourceAddress", RPCArg::Type::STR, RPCArg::Optional::NO, "Wallet address to pay the transaction fee from."},
            {"submit", RPCArg::Type::BOOL, RPCArg::Default{true}, "Submit the transaction to the network."},
        },
        {
            RPCResult{"if \"submit\" is not set or set to true",
                RPCResult::Type::STR_HEX, "txid", "The transaction id"},
            RPCResult{"if \"submit\" is set to false",
                RPCResult::Type::STR_HEX, "hex", "The serialized signed ProUpShareTx in hex format"},
        },
        RPCExamples{HelpExampleCli("protx", "update_share \"proTxHash\" 0 \"" + EXAMPLE_ADDRESS[1] + "\" \"" + EXAMPLE_ADDRESS[0] + "\"")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const NodeContext& node = EnsureAnyNodeContext(request.context);
    const ChainstateManager& chainman = EnsureChainman(node);
    CDeterministicMNManager& dmnman = *CHECK_NONFATAL(node.dmnman);
    CChainstateHelper& chain_helper = *CHECK_NONFATAL(node.chain_helper);

    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return UniValue::VNULL;
    EnsureWalletIsUnlocked(*pwallet);

    CProUpShareTx ptx;
    ptx.proTxHash = ParseHashV(request.params[0], "proTxHash");
    const int shareIndex{request.params[1].getInt<int>()};
    if (shareIndex < 0 || shareIndex > std::numeric_limits<uint16_t>::max()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "shareIndex out of range");
    }
    ptx.shareIndex = static_cast<uint16_t>(shareIndex);

    const auto dmn = dmnman.GetListAtChainTip().GetMN(ptx.proTxHash);
    if (!dmn || !dmn->pdmnState->IsShared()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "shared masternode not found");
    }
    if (ptx.shareIndex >= dmn->pdmnState->shares.size()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "shareIndex out of range");
    }

    if (!request.params[2].get_str().empty()) {
        CTxDestination rewardDest = DecodeDestination(request.params[2].get_str());
        if (!IsValidDestination(rewardDest)) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, strprintf("invalid reward address: %s", request.params[2].get_str()));
        }
        ptx.scriptReward = GetScriptForDestination(rewardDest);
    }

    CTxDestination feeSourceDest = DecodeDestination(request.params[3].get_str());
    if (!IsValidDestination(feeSourceDest)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid Dash address: ") + request.params[3].get_str());
    }

    bool fSubmit{true};
    if (!request.params[4].isNull()) {
        fSubmit = ParseBoolV(request.params[4], "submit");
    }

    CMutableTransaction tx;
    tx.nVersion = 3;
    tx.nType = TRANSACTION_PROVIDER_UPDATE_SHARE;

    // make sure we get enough fees added
    ptx.vchSig.resize(65);

    FundSpecialTx(*pwallet, tx, ptx, feeSourceDest);
    SignSpecialTxPayloadByHash(tx, ptx, dmn->pdmnState->shares[ptx.shareIndex].keyIDOwner, *pwallet);
    SetTxPayload(tx, ptx);

    return SignAndSendSpecialTx(request, chain_helper, chainman, tx, fSubmit);
},
    };
}

static RPCHelpMan protx_update_shared_registrar_prepare()
{
    return RPCHelpMan{"protx update_shared_registrar_prepare",
        "\nCreates an unsigned ProUpSharedRegTx updating a shared masternode's operator key and/or voting\n"
        "key. Fee inputs from this wallet are added and signed. Every share owner must then sign the\n"
        "returned transaction with \"protx shared_sign\"; combine the signatures with \"protx shared_combine\"\n"
        "on this same wallet (combining invalidates the fee-input signatures, which only this wallet can\n"
        "re-sign).\n"
        + HELP_REQUIRING_PASSPHRASE,
        {
            {"proTxHash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The hash of the initial ProRegTx."},
            {"operatorPubKey", RPCArg::Type::STR, RPCArg::Optional::NO, "The new operator BLS public key, or an empty string to keep the current key."},
            {"votingAddress", RPCArg::Type::STR, RPCArg::Optional::NO, "The new voting address, or an empty string to keep the current address."},
            {"feeSourceAddress", RPCArg::Type::STR, RPCArg::Optional::NO, "Wallet address to pay the transaction fee from."},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_HEX, "tx", "The serialized unsigned ProUpSharedRegTx"},
            {RPCResult::Type::STR_HEX, "signHash", "The payload hash every share owner must sign"},
        }},
        RPCExamples{HelpExampleCli("protx", "update_shared_registrar_prepare \"proTxHash\" \"operatorPubKey\" \"\" \"" + EXAMPLE_ADDRESS[0] + "\"")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const NodeContext& node = EnsureAnyNodeContext(request.context);
    CDeterministicMNManager& dmnman = *CHECK_NONFATAL(node.dmnman);

    std::shared_ptr<CWallet> const pwallet = GetWalletForJSONRPCRequest(request);
    if (!pwallet) return UniValue::VNULL;
    EnsureWalletIsUnlocked(*pwallet);

    CProUpSharedRegTx ptx;
    ptx.proTxHash = ParseHashV(request.params[0], "proTxHash");

    const auto dmn = dmnman.GetListAtChainTip().GetMN(ptx.proTxHash);
    if (!dmn || !dmn->pdmnState->IsShared()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "shared masternode not found");
    }

    if (!request.params[1].get_str().empty()) {
        ptx.pubKeyOperator.Set(ParseBLSPubKey(request.params[1].get_str(), "operator BLS address", /*specific_legacy_bls_scheme=*/false),
                               /*specificLegacyScheme=*/false);
    } else {
        ptx.pubKeyOperator = dmn->pdmnState->pubKeyOperator;
    }
    if (!request.params[2].get_str().empty()) {
        ptx.keyIDVoting = ParsePubKeyIDFromAddress(request.params[2].get_str(), "voting address");
    } else {
        ptx.keyIDVoting = dmn->pdmnState->keyIDVoting;
    }

    CTxDestination feeSourceDest = DecodeDestination(request.params[3].get_str());
    if (!IsValidDestination(feeSourceDest)) {
        throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, std::string("Invalid Dash address: ") + request.params[3].get_str());
    }

    CMutableTransaction tx;
    tx.nVersion = 3;
    tx.nType = TRANSACTION_PROVIDER_UPDATE_SHARED_REGISTRAR;

    // make sure we get enough fees added: one signature per share
    ptx.vchSigs.assign(dmn->pdmnState->shares.size(), std::vector<unsigned char>(CPubKey::COMPACT_SIGNATURE_SIZE, 0));

    FundSpecialTx(*pwallet, tx, ptx, feeSourceDest);
    UpdateSpecialTxInputsHash(tx, ptx);
    SetTxPayload(tx, ptx);

    UniValue ret(UniValue::VOBJ);
    ret.pushKV("tx", EncodeHexTx(CTransaction(tx)));
    ret.pushKV("signHash", ::SerializeHash(ptx).ToString());
    return ret;
},
    };
}

static RPCHelpMan protx_shared_combine()
{
    return RPCHelpMan{"protx shared_combine",
        "\nCombines share owner signatures produced by \"protx shared_sign\" into a shared masternode\n"
        "transaction. For a shared ProRegTx the completed transaction hex is returned and the funding\n"
        "inputs still have to be signed (e.g. by passing the result around signrawtransactionwithwallet).\n"
        "For a ProDisTx or ProUpSharedRegTx the transaction can be submitted directly. Submit a\n"
        "ProUpSharedRegTx from the wallet that ran \"protx update_shared_registrar_prepare\": combining\n"
        "re-signs its fee inputs, which only that wallet can do (with submit=false the returned hex can\n"
        "be finished there with signrawtransactionwithwallet).\n",
        {
            {"tx", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The serialized transaction in hex format."},
            {"signatures", RPCArg::Type::ARR, RPCArg::Optional::NO, "Signature entries collected from \"protx shared_sign\".",
            {
                {"", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
                {
                    {"shareIndex", RPCArg::Type::NUM, RPCArg::Optional::NO, "Index into the share table"},
                    {"signature", RPCArg::Type::STR, RPCArg::Optional::NO, "Base64-encoded signature"},
                }},
            }},
            {"submit", RPCArg::Type::BOOL, RPCArg::Default{false}, "Submit the transaction to the network (not available for registrations, whose funding inputs still need signing)."},
        },
        RPCResult{RPCResult::Type::STR_HEX, "result", "The transaction id if submitted, otherwise the combined transaction hex"},
        RPCExamples{HelpExampleCli("protx", "shared_combine \"tx\" \"[{\\\"shareIndex\\\":0,\\\"signature\\\":\\\"...\\\"}]\"")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const NodeContext& node = EnsureAnyNodeContext(request.context);
    const ChainstateManager& chainman = EnsureChainman(node);
    CDeterministicMNManager& dmnman = *CHECK_NONFATAL(node.dmnman);
    CChainstateHelper& chain_helper = *CHECK_NONFATAL(node.chain_helper);

    CMutableTransaction tx;
    if (!DecodeHexTx(tx, request.params[0].get_str())) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "transaction not deserializable");
    }

    // Collect (shareIndex, signature) pairs
    std::map<size_t, std::vector<unsigned char>> sigs;
    for (const auto& entry : request.params[1].get_array().getValues()) {
        const int64_t index{entry.find_value("shareIndex").getInt<int64_t>()};
        auto opt_sig = DecodeBase64(entry.find_value("signature").get_str());
        if (index < 0 || !opt_sig.has_value() || opt_sig->size() != CPubKey::COMPACT_SIGNATURE_SIZE) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "invalid signature entry");
        }
        if (!sigs.emplace(static_cast<size_t>(index), *opt_sig).second) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "duplicate shareIndex");
        }
    }

    const bool fSubmit{request.params[2].isNull() ? false : ParseBoolV(request.params[2], "submit")};

    if (tx.nType == TRANSACTION_PROVIDER_REGISTER) {
        auto opt_ptx = GetTxPayload<CProRegTx>(tx);
        if (!opt_ptx || !opt_ptx->IsShared()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "transaction is not a shared masternode registration");
        }
        if (fSubmit) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "a combined registration still needs its funding inputs signed; submit it with sendrawtransaction afterwards");
        }
        if (sigs.size() != opt_ptx->shares.size()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "a registration requires a signature from every share");
        }
        for (const auto& [index, sig] : sigs) {
            if (index >= opt_ptx->shares.size()) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "shareIndex out of range");
            }
            opt_ptx->vchJoinSigs[index] = sig;
        }
        SetTxPayload(tx, *opt_ptx);
        return EncodeHexTx(CTransaction(tx));
    } else if (tx.nType == TRANSACTION_PROVIDER_DISSOLVE) {
        auto opt_ptx = GetTxPayload<CProDisTx>(tx);
        if (!opt_ptx) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "transaction payload not deserializable");
        }
        const auto dmn = dmnman.GetListAtChainTip().GetMN(opt_ptx->proTxHash);
        if (!dmn || !dmn->pdmnState->IsShared()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "shared masternode not found");
        }
        const size_t share_count{dmn->pdmnState->shares.size()};
        opt_ptx->vchSigs.clear();
        if (sigs.size() == 1) {
            // unilateral: the single signature must be the actor's
            if (sigs.begin()->first != opt_ptx->actorIndex) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "a single signature must be by the actor share");
            }
            opt_ptx->vchSigs.push_back(sigs.begin()->second);
        } else if (sigs.size() == share_count) {
            for (size_t i = 0; i < share_count; i++) {
                const auto it = sigs.find(i);
                if (it == sigs.end()) {
                    throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("missing signature for share %d", i));
                }
                opt_ptx->vchSigs.push_back(it->second);
            }
        } else {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "a dissolution requires exactly one signature or one per share");
        }
        SetTxPayload(tx, *opt_ptx);
        if (!fSubmit) {
            return EncodeHexTx(CTransaction(tx));
        }
        return SubmitSpecialTx(request, chain_helper, chainman, tx);
    } else if (tx.nType == TRANSACTION_PROVIDER_UPDATE_SHARED_REGISTRAR) {
        auto opt_ptx = GetTxPayload<CProUpSharedRegTx>(tx);
        if (!opt_ptx) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "transaction payload not deserializable");
        }
        const auto dmn = dmnman.GetListAtChainTip().GetMN(opt_ptx->proTxHash);
        if (!dmn || !dmn->pdmnState->IsShared()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "shared masternode not found");
        }
        const size_t share_count{dmn->pdmnState->shares.size()};
        if (sigs.size() != share_count) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "a shared registrar update requires a signature from every share");
        }
        opt_ptx->vchSigs.clear();
        for (size_t i = 0; i < share_count; i++) {
            const auto it = sigs.find(i);
            if (it == sigs.end()) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("missing signature for share %d", i));
            }
            opt_ptx->vchSigs.push_back(it->second);
        }
        SetTxPayload(tx, *opt_ptx);
        // Inserting the signatures changed the payload, so the fee inputs signed at prepare time
        // are stale; SignAndSendSpecialTx re-signs them with this wallet before submitting
        return SignAndSendSpecialTx(request, chain_helper, chainman, tx, fSubmit);
    }
    throw JSONRPCError(RPC_INVALID_PARAMETER, "transaction is not a shared masternode transaction");
},
    };
}

#endif//ENABLE_WALLET

#ifdef ENABLE_WALLET
static bool CheckWalletOwnsScript(const CWallet* const pwallet, const CScript& script) {
    if (!pwallet) {
        return false;
    }
    return WITH_LOCK(pwallet->cs_wallet, return pwallet->IsMine(script)) == isminetype::ISMINE_SPENDABLE;
}

static bool CheckWalletOwnsAnyPayout(const CWallet* const pwallet, const CDeterministicMNState& state)
{
    for (const auto& script : state.GetOwnerRewardScripts()) {
        if (CheckWalletOwnsScript(pwallet, script)) return true;
    }
    return false;
}

static bool CheckWalletOwnsKey(const CWallet* const pwallet, const CKeyID& keyID) {
    return CheckWalletOwnsScript(pwallet, GetScriptForDestination(PKHash(keyID)));
}

static bool CheckWalletOwnsAnyOwnerKey(const CWallet* const pwallet, const CDeterministicMNState& state)
{
    // A shared masternode has a null keyIDOwner; the share owner keys take its place
    if (state.IsShared()) {
        for (const auto& share : state.shares) {
            if (CheckWalletOwnsKey(pwallet, share.keyIDOwner)) return true;
        }
        return false;
    }
    return CheckWalletOwnsKey(pwallet, state.keyIDOwner);
}

static bool CheckWalletOwnsAnyShareRefund(const CWallet* const pwallet, const CDeterministicMNState& state)
{
    // A dissolution returns each share's collateral to its refund script. The non-shared
    // equivalent (the collateral UTXO) is matched via ListProTxCoins, which cannot see the
    // shared covenant output, so match the refund destinations directly.
    for (const auto& share : state.shares) {
        if (CheckWalletOwnsScript(pwallet, share.scriptRefund)) return true;
    }
    return false;
}
#endif

static UniValue BuildDMNListEntry(const CWallet* const pwallet, const CDeterministicMN& dmn, const CMasternodeMetaMan& mn_metaman, bool detailed, const ChainstateManager& chainman, const CBlockIndex* pindex = nullptr)
{
    if (!detailed) {
        return dmn.proTxHash.ToString();
    }

    UniValue o = dmn.ToJson();

    CTransactionRef collateralTx{nullptr};
    int confirmations = GetUTXOConfirmations(chainman.ActiveChainstate(), dmn.collateralOutpoint);

    if (pindex != nullptr) {
        if (confirmations > -1) {
            confirmations -= WITH_LOCK(::cs_main, return chainman.ActiveChain().Height()) - pindex->nHeight;
        } else {
            uint256 minedBlockHash;
            collateralTx = GetTransaction(/* pindex */ nullptr, /* mempool */ nullptr, dmn.collateralOutpoint.hash, Params().GetConsensus(), minedBlockHash);
            const CBlockIndex* const pindexMined = WITH_LOCK(::cs_main, return chainman.m_blockman.LookupBlockIndex(minedBlockHash));
            CHECK_NONFATAL(pindexMined != nullptr);
            CHECK_NONFATAL(pindex->GetAncestor(pindexMined->nHeight) == pindexMined);
            confirmations = pindex->nHeight - pindexMined->nHeight + 1;
        }
    }
    o.pushKV("confirmations", confirmations);

#ifdef ENABLE_WALLET
    bool hasOwnerKey = CheckWalletOwnsAnyOwnerKey(pwallet, *dmn.pdmnState);
    bool hasVotingKey = CheckWalletOwnsKey(pwallet, dmn.pdmnState->keyIDVoting);

    bool ownsCollateral = false;
    if (Coin coin; GetUTXOCoin(chainman.ActiveChainstate(), dmn.collateralOutpoint, coin)) {
        ownsCollateral = CheckWalletOwnsScript(pwallet, coin.out.scriptPubKey);
    } else if (collateralTx != nullptr) {
        ownsCollateral = CheckWalletOwnsScript(pwallet, collateralTx->vout[dmn.collateralOutpoint.n].scriptPubKey);
    }

    if (pwallet) {
        UniValue walletObj(UniValue::VOBJ);
        walletObj.pushKV("hasOwnerKey", hasOwnerKey);
        walletObj.pushKV("hasOperatorKey", false);
        walletObj.pushKV("hasVotingKey", hasVotingKey);
        walletObj.pushKV("ownsCollateral", ownsCollateral);
        walletObj.pushKV("ownsPayeeScript", CheckWalletOwnsAnyPayout(pwallet, *dmn.pdmnState));
        walletObj.pushKV("ownsOperatorRewardScript", CheckWalletOwnsScript(pwallet, dmn.pdmnState->scriptOperatorPayout));
        o.pushKV("wallet", walletObj);
    }
#endif

    o.pushKV("metaInfo", mn_metaman.GetInfo(dmn.proTxHash).ToJson());

    return o;
}

static RPCHelpMan protx_list()
{
    return RPCHelpMan{"protx list",
        "\nLists all ProTxs in your wallet or on-chain, depending on the given type.\n",
        {
            {"type", RPCArg::Type::STR, RPCArg::Default{"registered"},
                "\nAvailable types:\n"
                "  registered   - List all ProTx which are registered at the given chain height.\n"
                "                 This will also include ProTx which failed PoSe verification.\n"
                "  valid        - List only ProTx which are active/valid at the given chain height.\n"
                "  evo          - List only ProTx corresponding to EvoNodes at the given chain height.\n"
#ifdef ENABLE_WALLET
                "  wallet       - List only ProTx which are found in your wallet at the given chain height.\n"
                "                 This will also include ProTx which failed PoSe verification.\n"
#endif
            },
            {"detailed", RPCArg::Type::BOOL, RPCArg::Default{false}, "If not specified, only the hashes of the ProTx will be returned."},
            {"height", RPCArg::Type::NUM, RPCArg::DefaultHint{"current chain-tip"}, ""},
        },
        RPCResult{
            RPCResult::Type::ARR, "", "List of masternodes",
            {
                // Array elements are matched against this doc by index, so the two shapes
                // cannot be listed as alternatives here - the first entry would be applied
                // to element 0 only and the second to every element after it.
                // TODO: document fields of the detailed entry
                {RPCResult::Type::ANY, "", "The ProTx hash when detailed=false, otherwise an object describing the masternode"},
            }},
        RPCExamples{""},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const NodeContext& node = EnsureAnyNodeContext(request.context);
    const ChainstateManager& chainman = EnsureChainman(node);

    CDeterministicMNManager& dmnman = *CHECK_NONFATAL(node.dmnman);
    const CMasternodeMetaMan& mn_metaman = *CHECK_NONFATAL(node.mn_metaman);

    std::shared_ptr<CWallet> wallet{nullptr};
#ifdef ENABLE_WALLET
    try {
        wallet = GetWalletForJSONRPCRequest(request);
    } catch (...) {
    }
#endif

    std::string type = "registered";
    if (!request.params[0].isNull()) {
        type = request.params[0].get_str();
    }

    UniValue ret(UniValue::VARR);

    if (g_txindex) {
        g_txindex->BlockUntilSyncedToCurrentChain();
    }

    if (type == "wallet") {
        if (!wallet) {
            throw std::runtime_error("\"protx list wallet\" not supported when wallet is disabled");
        }
#ifdef ENABLE_WALLET

        if (request.params.size() > 4) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Too many arguments");
        }

        bool detailed = !request.params[1].isNull() ? ParseBoolV(request.params[1], "detailed") : false;

        LOCK2(wallet->cs_wallet, ::cs_main);
        int height = !request.params[2].isNull() ? request.params[2].getInt<int>() : chainman.ActiveChain().Height();
        if (height < 1 || height > chainman.ActiveChain().Height()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "invalid height specified");
        }

        std::set<COutPoint> setOutpts;
        for (const auto& outpt : wallet->ListProTxCoins()) {
            setOutpts.emplace(outpt);
        }

        CDeterministicMNList mnList = dmnman.GetListForBlock(chainman.ActiveChain()[height]);
        mnList.ForEachMN(/*onlyValid=*/false, [&](const auto& dmn) {
            if (setOutpts.count(dmn.collateralOutpoint) ||
                CheckWalletOwnsAnyOwnerKey(wallet.get(), *dmn.pdmnState) ||
                CheckWalletOwnsKey(wallet.get(), dmn.pdmnState->keyIDVoting) ||
                CheckWalletOwnsAnyPayout(wallet.get(), *dmn.pdmnState) ||
                CheckWalletOwnsAnyShareRefund(wallet.get(), *dmn.pdmnState) ||
                CheckWalletOwnsScript(wallet.get(), dmn.pdmnState->scriptOperatorPayout)) {
                ret.push_back(BuildDMNListEntry(wallet.get(), dmn, mn_metaman, detailed, chainman));
            }
        });
#endif
    } else if (type == "valid" || type == "registered" || type == "evo") {
        if (request.params.size() > 3) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "Too many arguments");
        }

        bool detailed = !request.params[1].isNull() ? ParseBoolV(request.params[1], "detailed") : false;

#ifdef ENABLE_WALLET
        LOCK2(wallet ? wallet->cs_wallet : ::cs_main, ::cs_main);
#else
        LOCK(::cs_main);
#endif
        int height = !request.params[2].isNull() ? request.params[2].getInt<int>() : chainman.ActiveChain().Height();
        if (height < 1 || height > chainman.ActiveChain().Height()) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, "invalid height specified");
        }

        CDeterministicMNList mnList = dmnman.GetListForBlock(chainman.ActiveChain()[height]);
        bool onlyValid = type == "valid";
        bool onlyEvoNodes = type == "evo";
        mnList.ForEachMN(onlyValid, [&](const auto& dmn) {
            if (onlyEvoNodes && dmn.nType != MnType::Evo) return;
            ret.push_back(BuildDMNListEntry(wallet.get(), dmn, mn_metaman, detailed, chainman));
        });
    } else {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "invalid type specified");
    }

    return ret;
},
    };
}

static RPCHelpMan protx_info()
{
    return RPCHelpMan{"protx info",
        "\nReturns detailed information about a deterministic masternode.\n",
        {
            GetRpcArg("proTxHash"),
            {"blockHash", RPCArg::Type::STR_HEX, RPCArg::DefaultHint{"(chain tip)"}, "The hash of the block to get deterministic masternode state at"},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "Details about a specific deterministic masternode",
            {
                // TODO: implement proper doc for protx info
                {RPCResult::Type::ELISION, "", ""}
            }
        },
        RPCExamples{
            HelpExampleCli("protx", "info \"0123456701234567012345670123456701234567012345670123456701234567\"")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const NodeContext& node = EnsureAnyNodeContext(request.context);
    const ChainstateManager& chainman = EnsureChainman(node);

    CDeterministicMNManager& dmnman = *CHECK_NONFATAL(node.dmnman);
    const CMasternodeMetaMan& mn_metaman = *CHECK_NONFATAL(node.mn_metaman);

    std::shared_ptr<CWallet> wallet{nullptr};
#ifdef ENABLE_WALLET
    try {
        wallet = GetWalletForJSONRPCRequest(request);
    } catch (...) {
    }
#endif

    if (g_txindex) {
        g_txindex->BlockUntilSyncedToCurrentChain();
    }

    const CBlockIndex* pindex{nullptr};

    uint256 proTxHash(ParseHashV(request.params[0], "proTxHash"));

    if (request.params[1].isNull()) {
        LOCK(::cs_main);
        pindex = chainman.ActiveChain().Tip();
    } else {
        LOCK(::cs_main);
        uint256 blockHash(ParseHashV(request.params[1], "blockHash"));
        pindex = chainman.m_blockman.LookupBlockIndex(blockHash);
        if (pindex == nullptr) {
            throw JSONRPCError(RPC_INVALID_ADDRESS_OR_KEY, "Block not found");
        }
    }

    auto mnList = dmnman.GetListForBlock(pindex);
    auto dmn = mnList.GetMN(proTxHash);
    if (!dmn) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("%s not found", proTxHash.ToString()));
    }
    return BuildDMNListEntry(wallet.get(), *dmn, mn_metaman, true, chainman, pindex);
},
    };
}

static uint256 ParseBlock(const UniValue& v, const ChainstateManager& chainman, const std::string& strName) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    AssertLockHeld(::cs_main);

    try {
        return ParseHashV(v, strName);
    } catch (...) {
        bool fail{false}; int32_t h{0};
        if (v.isNum()) {
            h = v.getInt<int>();
        } else if (!ParseInt32(v.get_str(), &h)) {
            fail = true;
        }
        if (fail || h < 1 || h > chainman.ActiveChain().Height()) {
            throw std::runtime_error(strprintf("%s must be a block hash or chain height and not %s", strName, v.getValStr()));
        }
        return *chainman.ActiveChain()[h]->phashBlock;
    }
}

static const CBlockIndex* ParseBlockIndex(const UniValue& v, const ChainstateManager& chainman, const std::string& strName) EXCLUSIVE_LOCKS_REQUIRED(::cs_main)
{
    AssertLockHeld(::cs_main);

    try {
        const auto hash{ParseBlock(v, chainman, strName)};
        const CBlockIndex* pindex = chainman.m_blockman.LookupBlockIndex(hash);
        if (!pindex) {
            throw std::runtime_error(strprintf("Block %s with hash %s not found", strName, v.getValStr()));
        }
        return pindex;
    } catch (...) {
        // Same phrasing as ParseBlock() as it can parse heights
        throw std::runtime_error(strprintf("%s must be a block hash or chain height and not %s", strName, v.getValStr()));
    }
}

static RPCHelpMan protx_diff()
{
    return RPCHelpMan{"protx diff",
        "\nCalculates a diff between two deterministic masternode lists. The result also contains proof data.\n",
        {
            {"baseBlock", RPCArg::Type::STR, RPCArg::Optional::NO, "The starting block hash or height."},
            {"block", RPCArg::Type::STR, RPCArg::Optional::NO, "The ending block hash or height."},
            {"extended", RPCArg::Type::BOOL, RPCArg::Optional::OMITTED, "Show additional fields."},
        },
        CSimplifiedMNListDiff::GetJsonHelp(/*key=*/"", /*optional=*/false),
        RPCExamples{""},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const NodeContext& node = EnsureAnyNodeContext(request.context);
    const ChainstateManager& chainman = EnsureChainman(node);

    CDeterministicMNManager& dmnman = *CHECK_NONFATAL(node.dmnman);
    const LLMQContext& llmq_ctx = *CHECK_NONFATAL(node.llmq_ctx);

    LOCK(::cs_main);
    uint256 baseBlockHash = ParseBlock(request.params[0], chainman, "baseBlock");
    uint256 blockHash = ParseBlock(request.params[1], chainman, "block");
    bool extended = false;
    if (!request.params[2].isNull()) {
        extended = ParseBoolV(request.params[2], "extended");
    }

    CSimplifiedMNListDiff mnListDiff;
    std::string strError;

    if (!BuildSimplifiedMNListDiff(dmnman, chainman, *llmq_ctx.quorum_block_processor, *llmq_ctx.qman, baseBlockHash,
                                   blockHash, mnListDiff, strError, extended))
    {
        throw std::runtime_error(strError);
    }

    return mnListDiff.ToJson(extended);
},
    };
}

static RPCHelpMan protx_listdiff()
{
    return RPCHelpMan{"protx listdiff",
               "\nCalculate a full MN list diff between two masternode lists.\n",
               {
                       {"baseBlock", RPCArg::Type::STR, RPCArg::Optional::NO, "The starting block hash or height."},
                       {"block", RPCArg::Type::STR, RPCArg::Optional::NO, "The ending block hash or height."},
               },
                RPCResult {
                    RPCResult::Type::OBJ, "", "",
                    {
                        {RPCResult::Type::NUM, "baseHeight", "Height of base (starting) block"},
                        {RPCResult::Type::NUM, "blockHeight", "Height of target (ending) block"},
                        {RPCResult::Type::ARR, "addedMNs", "Added masternodes",
                            {CDeterministicMN::GetJsonHelp(/*key=*/"", /*optional=*/false)}},
                        {RPCResult::Type::ARR, "removedMNs", "Removed masternodes",
                            {{RPCResult::Type::STR_HEX, "protx", "ProTx of removed masternode"}}},
                        {RPCResult::Type::ARR, "updatedMNs", "Updated masternodes",
                            {{RPCResult::Type::OBJ_DYN, "", "json object with ProTx hash as keys",
                                {CDeterministicMNStateDiff::GetJsonHelp(/*key=*/"<protx_hash>", /*optional=*/false)}}}},
                    },
                },
                RPCExamples{""},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const NodeContext& node = EnsureAnyNodeContext(request.context);
    const ChainstateManager& chainman = EnsureChainman(node);

    CDeterministicMNManager& dmnman = *CHECK_NONFATAL(node.dmnman);

    LOCK(::cs_main);
    UniValue ret(UniValue::VOBJ);

    const CBlockIndex* pBaseBlockIndex = ParseBlockIndex(request.params[0], chainman, "baseBlock");
    const CBlockIndex* pTargetBlockIndex = ParseBlockIndex(request.params[1], chainman, "block");

    ret.pushKV("baseHeight", pBaseBlockIndex->nHeight);
    ret.pushKV("blockHeight", pTargetBlockIndex->nHeight);

    auto baseBlockMNList = dmnman.GetListForBlock(pBaseBlockIndex);
    auto blockMNList = dmnman.GetListForBlock(pTargetBlockIndex);

    auto mnDiff = baseBlockMNList.BuildDiff(blockMNList);

    UniValue jaddedMNs(UniValue::VARR);
    for(const auto& mn : mnDiff.addedMNs) {
        jaddedMNs.push_back(mn->ToJson());
    }
    ret.pushKV("addedMNs", jaddedMNs);

    UniValue jremovedMNs(UniValue::VARR);
    for(const auto& internal_id : mnDiff.removedMns) {
        auto dmn = baseBlockMNList.GetMNByInternalId(internal_id);
        // BuildDiff will construct itself with MNs that we already have knowledge
        // of, meaning that fetch operations should never fail.
        CHECK_NONFATAL(dmn);
        jremovedMNs.push_back(dmn->proTxHash.ToString());
    }
    ret.pushKV("removedMNs", jremovedMNs);

    UniValue jupdatedMNs(UniValue::VARR);
    for(const auto& [internal_id, stateDiff] : mnDiff.updatedMNs) {
        auto dmn = baseBlockMNList.GetMNByInternalId(internal_id);
        // BuildDiff will construct itself with MNs that we already have knowledge
        // of, meaning that fetch operations should never fail.
        CHECK_NONFATAL(dmn);
        UniValue obj(UniValue::VOBJ);
        obj.pushKV(dmn->proTxHash.ToString(), stateDiff.ToJson(dmn->nType));
        jupdatedMNs.push_back(obj);
    }
    ret.pushKV("updatedMNs", jupdatedMNs);

    return ret;
},
    };
}

// Helper function for evodb verify/repair commands
static UniValue evodb_verify_or_repair_impl(const JSONRPCRequest& request, bool repair)
{
    const NodeContext& node = EnsureAnyNodeContext(request.context);
    ChainstateManager& chainman = EnsureChainman(node);
    CDeterministicMNManager& dmnman = *CHECK_NONFATAL(node.dmnman);
    CChainstateHelper& chain_helper = *CHECK_NONFATAL(node.chain_helper);

    const CBlockIndex* start_index;
    const CBlockIndex* stop_index;

    {
        LOCK(::cs_main);
        // Default to DIP0003 activation height if startBlock not specified
        if (request.params[0].isNull()) {
            const auto& consensus_params = Params().GetConsensus();
            start_index = chainman.ActiveChain()[consensus_params.DIP0003Height];
            if (!start_index) {
                throw JSONRPCError(RPC_INTERNAL_ERROR, "Cannot find DIP0003 activation block");
            }
        } else {
            uint256 start_block_hash = ParseBlock(request.params[0], chainman, "startBlock");
            start_index = chainman.m_blockman.LookupBlockIndex(start_block_hash);
            if (!start_index) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Start block not found");
            }
        }

        // Default to chain tip if stopBlock not specified
        if (request.params[1].isNull()) {
            stop_index = chainman.ActiveChain().Tip();
            if (!stop_index) {
                throw JSONRPCError(RPC_INTERNAL_ERROR, "Cannot find chain tip");
            }
        } else {
            uint256 stop_block_hash = ParseBlock(request.params[1], chainman, "stopBlock");
            stop_index = chainman.m_blockman.LookupBlockIndex(stop_block_hash);
            if (!stop_index) {
                throw JSONRPCError(RPC_INVALID_PARAMETER, "Stop block not found");
            }
        }
    }

    int start_height = start_index->nHeight;
    int stop_height = stop_index->nHeight;

    // Validation
    if (stop_height < start_height) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "stopBlock must be >= startBlock");
    }

    // Create a callback that wraps CSpecialTxProcessor::RebuildListFromBlock
    auto build_list_func = [&chain_helper](const CBlock& block, const CBlockIndex* const pindexPrev,
                                           const CDeterministicMNList& prevList, const CCoinsViewCache& view,
                                           bool debugLogs, BlockValidationState& state,
                                           CDeterministicMNList& mnListRet) -> bool {
        return chain_helper.special_tx->RebuildListFromBlock(block, pindexPrev, prevList, view, debugLogs, state, mnListRet);
    };

    // Call the dmnman method to do the work
    auto recalc_result = dmnman.RecalculateAndRepairDiffs(start_index, stop_index, build_list_func, repair);

    // Convert result to UniValue
    UniValue result(UniValue::VOBJ);
    UniValue verification_errors(UniValue::VARR);

    for (const auto& verification_error : recalc_result.verification_errors) {
        verification_errors.push_back(verification_error);
    }

    result.pushKV("startHeight", recalc_result.start_height);
    result.pushKV("stopHeight", recalc_result.stop_height);
    result.pushKV("diffsRecalculated", recalc_result.diffs_recalculated);
    result.pushKV("snapshotsVerified", recalc_result.snapshots_verified);
    result.pushKV("verificationErrors", verification_errors);

    // Only include repair errors if we're in repair mode
    if (repair) {
        UniValue repair_errors(UniValue::VARR);
        for (const auto& repair_error : recalc_result.repair_errors) {
            repair_errors.push_back(repair_error);
        }
        result.pushKV("repairErrors", repair_errors);
    }

    return result;
}

static RPCHelpMan evodb_verify()
{
    return RPCHelpMan{"evodb verify",
        "\nVerifies evodb diff records between specified block heights.\n"
        "Checks that all diffs applied between snapshots in the range match the saved snapshots in evodb.\n"
        "This is a read-only operation that does not modify the database.\n"
        "If no heights are specified, defaults to the full range from DIP0003 activation to chain tip.\n",
        {
            {"startBlock", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "The starting block hash or height (defaults to DIP0003 activation height)."},
            {"stopBlock", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "The ending block hash or height (defaults to current chain tip)."},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::NUM, "startHeight", "Actual starting block height (may differ from input if clamped to DIP0003 activation)"},
                {RPCResult::Type::NUM, "stopHeight", "Ending block height"},
                {RPCResult::Type::NUM, "diffsRecalculated", "Number of diffs recalculated (always 0 for verify-only mode)"},
                {RPCResult::Type::NUM, "snapshotsVerified", "Number of snapshot pairs that passed verification"},
                {RPCResult::Type::ARR, "verificationErrors", "List of verification errors (empty if verification passed)",
                    {
                        {RPCResult::Type::STR, "", "Error message"},
                    }
                },
            }
        },
        RPCExamples{
            HelpExampleCli("evodb verify", "")
            + HelpExampleCli("evodb verify", "1000 2000")
            + HelpExampleRpc("evodb", "\"verify\"")
            + HelpExampleRpc("evodb", "\"verify\", 1000, 2000")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    return evodb_verify_or_repair_impl(request, false);
},
    };
}

static RPCHelpMan evodb_repair()
{
    return RPCHelpMan{"evodb repair",
        "\nRepairs corrupted evodb diff records between specified block heights.\n"
        "First verifies all diffs applied between snapshots in the range.\n"
        "If verification fails, recalculates diffs from blockchain data and replaces corrupted records.\n"
        "If no heights are specified, defaults to the full range from DIP0003 activation to chain tip.\n",
        {
            {"startBlock", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "The starting block hash or height (defaults to DIP0003 activation height)."},
            {"stopBlock", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "The ending block hash or height (defaults to current chain tip)."},
        },
        RPCResult{
            RPCResult::Type::OBJ, "", "",
            {
                {RPCResult::Type::NUM, "startHeight", "Actual starting block height (may differ from input if clamped to DIP0003 activation)"},
                {RPCResult::Type::NUM, "stopHeight", "Ending block height"},
                {RPCResult::Type::NUM, "diffsRecalculated", "Number of diffs successfully recalculated and written to database"},
                {RPCResult::Type::NUM, "snapshotsVerified", "Number of snapshot pairs that passed verification"},
                {RPCResult::Type::ARR, "verificationErrors", "Errors encountered during verification phase (empty if verification passed)",
                    {
                        {RPCResult::Type::STR, "", "Error message"},
                    }
                },
                {RPCResult::Type::ARR, "repairErrors", "Critical errors encountered during repair phase (non-empty means full reindex required)",
                    {
                        {RPCResult::Type::STR, "", "Error message"},
                    }
                },
            }
        },
        RPCExamples{
            HelpExampleCli("evodb repair", "")
            + HelpExampleCli("evodb repair", "1000 2000")
            + HelpExampleRpc("evodb", "\"repair\"")
            + HelpExampleRpc("evodb", "\"repair\", 1000, 2000")
        },
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    return evodb_verify_or_repair_impl(request, true);
},
    };
}

#ifdef ENABLE_WALLET
static RPCHelpMan protx_register_shared_prepare()
{
    return RPCHelpMan{"protx register_shared_prepare",
        "\nCreates an unsigned shared masternode registration (a version 3 ProRegTx with a collateral share\n"
        "table) by appending the shared-collateral output and payload to a caller-supplied funding\n"
        "transaction. The funding transaction must already contain every participant's contribution inputs\n"
        "and any change outputs; the consent digest binds all of them. Every share owner must sign the\n"
        "returned consent hash via \"protx shared_sign\"; combine with \"protx shared_combine\", then have the\n"
        "funding inputs signed (signrawtransactionwithwallet) and broadcast with sendrawtransaction.\n"
        "\nIMPORTANT: once the registration confirms, every participant should create a zero-penalty\n"
        "standby dissolution (\"protx dissolve <proTxHash> <shareIndex> <fee> false false\") and store\n"
        "the returned hex with their refund-key backup, separately from the share owner key. It becomes\n"
        "valid when the early period ends (or immediately when no early period applies), then never expires;\n"
        "broadcasting it after that point recovers the participant's principal without cooperation.\n",
        {
            {"fundingTx", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The serialized funding transaction with all participants' inputs and change outputs."},
            {"shares", RPCArg::Type::ARR, RPCArg::Optional::NO, "The collateral share table, in consensus-significant order. Amounts must sum to the collateral.",
            {
                {"", RPCArg::Type::OBJ, RPCArg::Optional::OMITTED, "",
                {
                    {"amount", RPCArg::Type::NUM, RPCArg::Optional::NO, "Collateral contribution in duffs (at least 100 DASH)"},
                    {"refundAddress", RPCArg::Type::STR, RPCArg::Optional::NO, "Immutable address the principal is refunded to at dissolution"},
                    {"rewardAddress", RPCArg::Type::STR, RPCArg::Optional::OMITTED, "Address this share's owner rewards are paid to (defaults to the refund address)"},
                    {"ownerAddress", RPCArg::Type::STR, RPCArg::Optional::NO, "P2PKH address of the immutable share owner key"},
                }},
            }},
            {"coreP2PAddrs", RPCArg::Type::STR, RPCArg::Optional::NO, "IP address and port of the masternode, leave empty to bank on a later ProUpServTx."},
            {"operatorPubKey", RPCArg::Type::STR, RPCArg::Optional::NO, "The operator BLS public key."},
            {"votingAddress", RPCArg::Type::STR, RPCArg::Optional::NO, "The voting key address."},
            {"operatorReward", RPCArg::Type::STR, RPCArg::Optional::NO, "The fraction in %% to share with the operator (0.00 to 100.00)."},
            {"earlyPeriodBlocks", RPCArg::Type::NUM, RPCArg::Optional::NO, "Length in blocks of the early period during which unilateral dissolution is penalized (up to 420480)."},
            {"earlyPenalty", RPCArg::Type::NUM, RPCArg::Optional::NO, "Penalty in duffs for unilateral dissolution during the early period (must be below the smallest share)."},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_HEX, "tx", "The serialized unsigned shared ProRegTx"},
            {RPCResult::Type::NUM, "collateralIndex", "Output index of the shared collateral"},
            {RPCResult::Type::STR_HEX, "consentHash", "The consent digest every share owner must sign"},
        }},
        RPCExamples{HelpExampleCli("protx", "register_shared_prepare \"fundingTx\" \"[...]\" \"1.2.3.4:1234\" \"operatorPubKey\" \"" + EXAMPLE_ADDRESS[1] + "\" 0 10000 5000000000")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const NodeContext& node = EnsureAnyNodeContext(request.context);
    const ChainstateManager& chainman = EnsureChainman(node);

    CMutableTransaction tx;
    if (!DecodeHexTx(tx, request.params[0].get_str())) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "funding transaction not deserializable");
    }
    tx.nVersion = 3;
    tx.nType = TRANSACTION_PROVIDER_REGISTER;

    CProRegTx ptx;
    ptx.nType = MnType::Regular;
    ptx.nVersion = DeploymentToProtxVersion(WITH_LOCK(::cs_main, return chainman.ActiveChain().Tip()), chainman);
    if (ptx.nVersion < ProTxVersion::ExtAddr) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "shared masternode registration requires provider transaction version 3");
    }
    ptx.netInfo = NetInfoInterface::MakeNetInfo(ptx.nVersion);

    ptx.shares = ParseShares(request.params[1], "shares");

    ProcessNetInfoCore(ptx, request.params[2], /*optional=*/true);

    ptx.pubKeyOperator.Set(ParseBLSPubKey(request.params[3].get_str(), "operator BLS address", /*specific_legacy_bls_scheme=*/false),
                           /*specificLegacyScheme=*/false);

    {
        CTxDestination voting_dest = DecodeDestination(request.params[4].get_str());
        const PKHash* voting_pkhash = std::get_if<PKHash>(&voting_dest);
        if (!voting_pkhash) {
            throw JSONRPCError(RPC_INVALID_PARAMETER, strprintf("voting address must be a valid P2PKH address, not %s", request.params[4].get_str()));
        }
        ptx.keyIDVoting = ToKeyID(*voting_pkhash);
    }

    int64_t operatorReward;
    if (!ParseFixedPoint(request.params[5].getValStr(), 2, &operatorReward)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "operatorReward must be a number");
    }
    if (operatorReward < 0 || operatorReward > 10000) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "operatorReward must be between 0 and 10000");
    }
    ptx.nOperatorReward = operatorReward;

    const int64_t earlyPeriodBlocks{request.params[6].getInt<int64_t>()};
    if (earlyPeriodBlocks < 0 || earlyPeriodBlocks > CProRegTx::MAX_EARLY_PERIOD_BLOCKS) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "earlyPeriodBlocks out of range");
    }
    ptx.nEarlyPeriodBlocks = static_cast<uint32_t>(earlyPeriodBlocks);
    ptx.nEarlyPenalty = request.params[7].getInt<int64_t>();

    // Append the shared collateral output; the collateral is always internal
    tx.vout.emplace_back(GetMnType(ptx.nType).collat_amount, sharedcollateral::SharedCollateralScript());
    ptx.collateralOutpoint = COutPoint(uint256(), static_cast<uint32_t>(tx.vout.size() - 1));

    // Placeholder consent signatures; filled in by "protx shared_combine"
    ptx.vchJoinSigs.assign(ptx.shares.size(), std::vector<unsigned char>(CPubKey::COMPACT_SIGNATURE_SIZE, 0));

    UpdateSpecialTxInputsHash(tx, ptx);

    // Preflight the payload with the same stateless rules consensus applies, so consensus-invalid
    // terms (bad share sums, penalty bounds, payee reuse, ...) fail here with a clear error
    // instead of after every participant has signed and the funding inputs are finalized
    if (TxValidationState state; !ptx.IsTriviallyValid(state)) {
        throw JSONRPCError(RPC_INVALID_PARAMETER,
                           strprintf("invalid shared registration terms: %s", state.GetRejectReason()));
    }

    SetTxPayload(tx, ptx);

    UniValue ret(UniValue::VOBJ);
    ret.pushKV("tx", EncodeHexTx(CTransaction(tx)));
    ret.pushKV("collateralIndex", static_cast<uint64_t>(ptx.collateralOutpoint.n));
    ret.pushKV("consentHash", ptx.MakeSharedRegConsentHash(CTransaction(tx)).ToString());
    return ret;
},
    };
}

static RPCHelpMan protx_dissolve_prepare()
{
    return RPCHelpMan{"protx dissolve_prepare",
        "\nCreates an unsigned unanimous ProDisTx dissolving a shared masternode without penalty at any\n"
        "height. Every share owner must sign it via \"protx shared_sign\"; combine and submit the result\n"
        "with \"protx shared_combine\". For a unilateral dissolution use \"protx dissolve\" instead.\n",
        {
            {"proTxHash", RPCArg::Type::STR_HEX, RPCArg::Optional::NO, "The hash of the initial ProRegTx."},
            {"actorIndex", RPCArg::Type::NUM, RPCArg::Optional::NO, "Index into the share table of the participant paying the transaction fee."},
            {"fee", RPCArg::Type::NUM, RPCArg::Default{100000}, "Transaction fee in duffs, paid from the actor's share. At most 1000000 duffs (consensus ceiling)."},
        },
        RPCResult{RPCResult::Type::OBJ, "", "",
        {
            {RPCResult::Type::STR_HEX, "tx", "The serialized unsigned ProDisTx"},
            {RPCResult::Type::STR_HEX, "signHash", "The digest every share owner must sign"},
        }},
        RPCExamples{HelpExampleCli("protx", "dissolve_prepare \"proTxHash\" 0")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    const NodeContext& node = EnsureAnyNodeContext(request.context);
    CDeterministicMNManager& dmnman = *CHECK_NONFATAL(node.dmnman);

    const uint256 proTxHash(ParseHashV(request.params[0], "proTxHash"));
    const int actorIndex{request.params[1].getInt<int>()};
    if (actorIndex < 0 || actorIndex > std::numeric_limits<uint16_t>::max()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "actorIndex out of range");
    }
    const CAmount fee{request.params[2].isNull() ? 100000 : request.params[2].getInt<int64_t>()};

    const auto dmn = dmnman.GetListAtChainTip().GetMN(proTxHash);
    if (!dmn || !dmn->pdmnState->IsShared()) {
        throw JSONRPCError(RPC_INVALID_PARAMETER, "shared masternode not found");
    }

    CProDisTx ptx;
    CMutableTransaction tx = BuildProDisTx(*dmn, static_cast<uint16_t>(actorIndex), /*penalty=*/0, fee, ptx);
    SetTxPayload(tx, ptx);

    UniValue ret(UniValue::VOBJ);
    ret.pushKV("tx", EncodeHexTx(CTransaction(tx)));
    // Unanimous dissolution: every share owner signs, so the count is the share count
    ret.pushKV("signHash",
               ptx.MakeSignHash(CTransaction(tx), static_cast<uint8_t>(dmn->pdmnState->shares.size())).ToString());
    return ret;
},
    };
}
#endif // ENABLE_WALLET

static RPCHelpMan protx_help()
{
    return RPCHelpMan{
        "protx",
        "Set of commands to execute ProTx related actions.\n"
        "To get help on individual commands, use \"help protx command\".\n"
        "\nAvailable commands:\n"
#ifdef ENABLE_WALLET
        "  register                 - Create and send ProTx to network\n"
        "  register_fund            - Fund, create and send ProTx to network\n"
        "  register_prepare         - Create an unsigned ProTx\n"
        "  register_evo             - Create and send ProTx to network for an EvoNode\n"
        "  register_fund_evo        - Fund, create and send ProTx to network for an EvoNode\n"
        "  register_prepare_evo     - Create an unsigned ProTx for an EvoNode\n"
        "  register_legacy          - (DEPRECATED) Create a ProTx by parsing BLS using the legacy scheme and send it to network\n"
        "  register_fund_legacy     - (DEPRECATED) Fund and create a ProTx by parsing BLS using the legacy scheme, then send it to network\n"
        "  register_prepare_legacy  - (DEPRECATED) Create an unsigned ProTx by parsing BLS using the legacy scheme\n"
        "  register_submit          - Sign and submit a ProTx\n"
        "  register_shared_prepare  - Create an unsigned shared masternode ProTx\n"
#endif
        "  list                     - List ProTxs\n"
        "  info                     - Return information about a ProTx\n"
#ifdef ENABLE_WALLET
        "  update_service           - Create and send ProUpServTx to network\n"
        "  update_service_evo       - Create and send ProUpServTx to network for an EvoNode\n"
        "  update_registrar         - Create and send ProUpRegTx to network\n"
        "  update_registrar_legacy  - (DEPRECATED) Create ProUpRegTx by parsing BLS using the legacy scheme, then send it to network\n"
        "  revoke                   - Create and send ProUpRevTx to network\n"
        "  shared_sign              - Sign a shared masternode transaction with this wallet's share owner keys\n"
        "  shared_combine           - Combine share owner signatures into a shared masternode transaction\n"
        "  dissolve                 - Create, sign and send a unilateral ProDisTx\n"
        "  update_share             - Create and send a ProUpShareTx updating one share's reward address\n"
        "  update_shared_registrar_prepare - Create an unsigned ProUpSharedRegTx\n"
        "  dissolve_prepare         - Create an unsigned unanimous ProDisTx\n"
#endif
        "  diff                     - Calculate a diff and a proof between two masternode lists\n"
        "  listdiff                 - Calculate a full MN list diff between two masternode lists\n",
        {
            {"command", RPCArg::Type::STR, RPCArg::Optional::NO, "The command to execute"},
        },
        RPCResult{RPCResult::Type::NONE, "", ""},
        RPCExamples{""},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    throw JSONRPCError(RPC_INVALID_PARAMETER, "Must be a valid command");
},
    };
}

static RPCHelpMan bls_generate()
{
    return RPCHelpMan{
        "bls generate",
        "\nReturns a BLS secret/public key pair.\n",
        {
            {"legacy", RPCArg::Type::BOOL, RPCArg::Default{false}, "(DEPRECATED, can be set if -deprecatedrpc=legacy_mn is passed) Set true to use legacy BLS scheme"},
        },
        RPCResult{RPCResult::Type::OBJ,
                  "",
                  "",
                  {{RPCResult::Type::STR_HEX, "secret", "BLS secret key"},
                   {RPCResult::Type::STR_HEX, "public", "BLS public key"},
                   {RPCResult::Type::STR_HEX, "scheme", "BLS scheme (valid schemes: legacy, basic)"}}},
        RPCExamples{HelpExampleCli("bls generate", "")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue {
            CBLSSecretKey sk;
            sk.MakeNewKey();
            bool bls_legacy_scheme{false};
            if (!request.params[0].isNull()) {
                if (!IsDeprecatedRPCEnabled("legacy_mn")) {
                    throw std::runtime_error("DEPRECATED: Pass config option -deprecatedrpc=legacy_mn to set this argument");
                }
                bls_legacy_scheme = ParseBoolV(request.params[0], "bls_legacy_scheme");
            }
            UniValue ret(UniValue::VOBJ);
            ret.pushKV("secret", sk.ToString());
            ret.pushKV("public", sk.GetPublicKey().ToString(bls_legacy_scheme));
            std::string bls_scheme_str = bls_legacy_scheme ? "legacy" : "basic";
            ret.pushKV("scheme", bls_scheme_str);
            return ret;
        },
    };
}

static RPCHelpMan bls_fromsecret()
{
    return RPCHelpMan{
        "bls fromsecret",
        "\nParses a BLS secret key and returns the secret/public key pair.\n",
        {
            {"secret", RPCArg::Type::STR, RPCArg::Optional::NO, "The BLS secret key"},
            {"legacy", RPCArg::Type::BOOL, RPCArg::Default{false}, "Pass true if you need in legacy scheme"},
        },
        RPCResult{RPCResult::Type::OBJ,
                  "",
                  "",
                  {
                      {RPCResult::Type::STR_HEX, "secret", "BLS secret key"},
                      {RPCResult::Type::STR_HEX, "public", "BLS public key"},
                      {RPCResult::Type::STR_HEX, "scheme", "BLS scheme (valid schemes: legacy, basic)"},
                  }},
        RPCExamples{
            HelpExampleCli("bls fromsecret", "000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f")},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue {
            bool bls_legacy_scheme{false};
            if (!request.params[1].isNull()) {
                bls_legacy_scheme = ParseBoolV(request.params[1], "bls_legacy_scheme");
            }
            CBLSSecretKey sk = ParseBLSSecretKey(request.params[0].get_str(), "secretKey");
            UniValue ret(UniValue::VOBJ);
            ret.pushKV("secret", sk.ToString());
            ret.pushKV("public", sk.GetPublicKey().ToString(bls_legacy_scheme));
            std::string bls_scheme_str = bls_legacy_scheme ? "legacy" : "basic";
            ret.pushKV("scheme", bls_scheme_str);
            return ret;
        },
    };
}

static RPCHelpMan bls_help()
{
    return RPCHelpMan{"bls",
        "Set of commands to execute BLS related actions.\n"
        "To get help on individual commands, use \"help bls command\".\n"
        "\nAvailable commands:\n"
        "  generate          - Create a BLS secret/public key pair\n"
        "  fromsecret        - Parse a BLS secret key and return the secret/public key pair\n",
        {
            {"command", RPCArg::Type::STR, RPCArg::Optional::NO, "The command to execute"},
        },
        RPCResult{RPCResult::Type::NONE, "", ""},
        RPCExamples{""},
        [&](const RPCHelpMan& self, const JSONRPCRequest& request) -> UniValue
{
    throw JSONRPCError(RPC_INVALID_PARAMETER, "Must be a valid command");
},
    };
}

#ifdef ENABLE_WALLET
Span<const CRPCCommand> GetWalletEvoRPCCommands()
{
    static const CRPCCommand commands[]{
        {"evo", &protx_list},
        {"evo", &protx_info},
        {"evo", &protx_register},
        {"evo", &protx_register_evo},
        {"evo", &protx_register_fund},
        {"evo", &protx_register_fund_evo},
        {"evo", &protx_register_prepare},
        {"evo", &protx_register_prepare_evo},
        {"evo", &protx_update_service},
        {"evo", &protx_update_service_evo},
        {"evo", &protx_register_submit},
        {"evo", &protx_update_registrar},
        {"evo", &protx_revoke},
        {"evo", &protx_register_shared_prepare},
        {"evo", &protx_shared_sign},
        {"evo", &protx_shared_combine},
        {"evo", &protx_dissolve},
        {"evo", &protx_dissolve_prepare},
        {"evo", &protx_update_share},
        {"evo", &protx_update_shared_registrar_prepare},
        {"hidden", &protx_register_legacy},
        {"hidden", &protx_register_fund_legacy},
        {"hidden", &protx_register_prepare_legacy},
        {"hidden", &protx_update_registrar_legacy},
    };
    return commands;
}
#endif // ENABLE_WALLET

void RegisterEvoRPCCommands(CRPCTable& t)
{
    static const CRPCCommand commands[]{
        {"evo", &bls_help},
        {"evo", &bls_generate},
        {"evo", &bls_fromsecret},
        {"evo", &protx_help},
        {"evo", &protx_diff},
        {"evo", &protx_listdiff},
        {"hidden", &evodb_verify},
        {"hidden", &evodb_repair},
    };
    static const CRPCCommand commands_wallet[]{
        {"evo", &protx_list},
        {"evo", &protx_info},
    };
    for (const auto& command : commands) {
        t.appendCommand(command.name, &command);
    }
    // If we aren't compiling with wallet support, we still need to register RPCs that are
    // capable of working without wallet support. We have to do this even if wallet support
    // is compiled in but is disabled at runtime because runtime disablement prohibits
    // registering wallet RPCs. We still want the reduced functionality RPC to be registered.
    // TODO: Spin off these hybrid RPCs into dedicated wallet-only and/or wallet-free RPCs
    //       and get rid of this workaround.
    if (!g_wallet_init_interface.HasWalletSupport()
#ifdef ENABLE_WALLET
        || gArgs.GetBoolArg("-disablewallet", DEFAULT_DISABLE_WALLET)
#endif // ENABLE_WALLET
    ) {
        for (const auto& command : commands_wallet) {
            t.appendCommand(command.name, &command);
        }
    }
}
