// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/masternode.h>

#include <bls/bls.h>
#include <evo/deterministicmns.h>
#include <evo/providertx.h>
#include <evo/specialtx.h>
#include <key.h>
#include <script/interpreter.h>
#include <script/sign.h>
#include <script/signingprovider.h>
#include <script/standard.h>
#include <sync.h>
#include <tinyformat.h>
#include <util/check.h>
#include <util/std23.h>
#include <validation.h>

#include <utility>

namespace {

SimpleUTXOMap SelectUTXOs(const CChain& active_chain, SimpleUTXOMap& utxos, CAmount amount, CAmount& change_ret)
{
    change_ret = 0;

    SimpleUTXOMap selected_utxos;
    CAmount selected_amount{0};
    while (!utxos.empty()) {
        bool found{false};
        for (auto it = utxos.begin(); it != utxos.end(); ++it) {
            if (active_chain.Height() - it->second.nHeight < 101) {
                continue;
            }

            found = true;
            selected_amount += it->second.out.nValue;
            selected_utxos.emplace(it->first, it->second);
            utxos.erase(it);
            break;
        }
        Assert(found);
        if (selected_amount >= amount) {
            change_ret = selected_amount - amount;
            break;
        }
    }

    return selected_utxos;
}

} // namespace

SimpleUTXOMap BuildSimpleUtxoMap(const std::vector<CTransactionRef>& txs)
{
    SimpleUTXOMap utxos;
    for (auto [i, tx] : std23::views::enumerate(txs)) {
        for (auto [j, output] : std23::views::enumerate(tx->vout)) {
            if (output.scriptPubKey.IsUnspendable()) continue;
            utxos.try_emplace(COutPoint(tx->GetHash(), j), Coin(output, static_cast<int>(i) + 1, /*fCoinBaseIn=*/false));
        }
    }
    return utxos;
}

SimpleUTXOMap FundTransaction(const ChainstateManager& chainman, CMutableTransaction& tx, SimpleUTXOMap& utxos,
                              const CScript& script_payout, CAmount amount)
{
    return FundTransaction(chainman, tx, utxos, script_payout, amount, /*script_change=*/script_payout);
}

SimpleUTXOMap FundTransaction(const ChainstateManager& chainman, CMutableTransaction& tx, SimpleUTXOMap& utxos,
                              const CScript& script_payout, CAmount amount, const CScript& script_change)
{
    CAmount change;
    auto inputs = WITH_LOCK(::cs_main, return SelectUTXOs(chainman.ActiveChain(), utxos, amount, change));
    for (const auto& input : inputs) {
        tx.vin.emplace_back(input.first);
    }
    tx.vout.emplace_back(amount, script_payout);
    if (change != 0) {
        tx.vout.emplace_back(change, script_change);
    }
    return inputs;
}

void SignTransaction(CMutableTransaction& tx, const SimpleUTXOMap& coins, const CKey& coinbase_key)
{
    FillableSigningProvider keystore;
    keystore.AddKeyPubKey(coinbase_key, coinbase_key.GetPubKey());

    std::map<int, bilingual_str> input_errors;
    Assert(::SignTransaction(tx, &keystore, coins, SIGHASH_ALL, input_errors));
}

CMutableTransaction CreateProRegTx(const ChainstateManager& chainman, SimpleUTXOMap& utxos, int port,
                                   const CScript& script_payout, const CKey& coinbase_key, CKey& owner_key_ret,
                                   CBLSSecretKey& operator_key_ret)
{
    owner_key_ret.MakeNewKey(true);
    operator_key_ret.MakeNewKey();
    return CreateProRegTx(chainman, utxos, port, script_payout, coinbase_key, std::as_const(owner_key_ret),
                          std::as_const(operator_key_ret));
}

CMutableTransaction CreateProRegTx(const ChainstateManager& chainman, SimpleUTXOMap& utxos, int port,
                                   const CScript& script_payout, const CKey& coinbase_key, const CKey& owner_key,
                                   const CBLSSecretKey& operator_key, MnType type)
{
    CProRegTx pro_tx;
    pro_tx.nVersion = ProTxVersion::GetMax(!bls::bls_legacy_scheme, /*is_extended_addr=*/false);
    pro_tx.nType = type;
    if (type == MnType::Evo) {
        pro_tx.platformNodeID = uint160{Span{SerializeHash(std::make_pair(std::string{"platform"}, port)).begin(), 20}};
        pro_tx.platformP2PPort = 20000 + 2 * port;
        pro_tx.platformHTTPPort = 20001 + 2 * port;
    }
    pro_tx.netInfo = NetInfoInterface::MakeNetInfo(pro_tx.nVersion);
    pro_tx.collateralOutpoint.n = 0;
    Assert(pro_tx.netInfo->AddEntry(NetInfoPurpose::CORE_P2P, strprintf("1.1.1.1:%d", port)) == NetInfoStatus::Success);
    pro_tx.keyIDOwner = owner_key.GetPubKey().GetID();
    pro_tx.pubKeyOperator.Set(operator_key.GetPublicKey(), bls::bls_legacy_scheme.load());
    pro_tx.keyIDVoting = owner_key.GetPubKey().GetID();
    pro_tx.scriptPayout = script_payout;

    CMutableTransaction tx;
    tx.nVersion = 3;
    tx.nType = TRANSACTION_PROVIDER_REGISTER;
    const auto spent = FundTransaction(chainman, tx, utxos, script_payout, GetMnType(type).collat_amount);
    pro_tx.inputsHash = CalcTxInputsHash(CTransaction(tx));
    SetTxPayload(tx, pro_tx);
    SignTransaction(tx, spent, coinbase_key);

    return tx;
}

CScript GenerateRandomAddress()
{
    CKey key;
    key.MakeNewKey(false);
    return GetScriptForDestination(PKHash(key.GetPubKey()));
}
