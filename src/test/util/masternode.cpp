// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/masternode.h>

#include <bls/bls.h>
#include <evo/deterministicmns.h>
#include <evo/providertx.h>
#include <evo/specialtx.h>
#include <key.h>
#include <messagesigner.h>
#include <script/interpreter.h>
#include <script/sign.h>
#include <script/signingprovider.h>
#include <script/standard.h>
#include <sync.h>
#include <tinyformat.h>
#include <util/check.h>
#include <util/std23.h>
#include <validation.h>

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

CMutableTransaction CreateSpendTx(const ChainstateManager& chainman, SimpleUTXOMap& utxos,
                                  const CScript& script_payout, CAmount amount, const CKey& coinbase_key)
{
    CMutableTransaction tx;
    const auto spent = FundTransaction(chainman, tx, utxos, script_payout, amount);
    SignTransaction(tx, spent, coinbase_key);
    return tx;
}

COutPoint GetCollateralOutpoint(const CMutableTransaction& tx)
{
    for (size_t i = 0; i < tx.vout.size(); ++i) {
        if (tx.vout[i].nValue == dmn_types::Regular.collat_amount) {
            return COutPoint(tx.GetHash(), i);
        }
    }
    return COutPoint();
}

CMutableTransaction CreateProRegTxExternalCollateral(const ChainstateManager& chainman, SimpleUTXOMap& utxos, int port,
                                                     const COutPoint& collateral_outpoint, const CScript& script_payout,
                                                     const CKey& owner_key, const CBLSSecretKey& operator_key,
                                                     const CKey& collateral_key, const CKey& coinbase_key)
{
    CProRegTx pro_tx;
    pro_tx.nVersion = ProTxVersion::GetMax(!bls::bls_legacy_scheme, /*is_extended_addr=*/false);
    pro_tx.netInfo = NetInfoInterface::MakeNetInfo(pro_tx.nVersion);
    Assert(pro_tx.netInfo->AddEntry(NetInfoPurpose::CORE_P2P, strprintf("1.1.1.1:%d", port)) == NetInfoStatus::Success);
    pro_tx.collateralOutpoint = collateral_outpoint;
    pro_tx.keyIDOwner = owner_key.GetPubKey().GetID();
    pro_tx.pubKeyOperator.Set(operator_key.GetPublicKey(), bls::bls_legacy_scheme.load());
    pro_tx.keyIDVoting = owner_key.GetPubKey().GetID();
    pro_tx.scriptPayout = script_payout;

    CMutableTransaction tx;
    tx.nVersion = 3;
    tx.nType = TRANSACTION_PROVIDER_REGISTER;
    // The collateral is external, referenced through collateralOutpoint, so this transaction
    // only has to fund a fee.
    const auto spent = FundTransaction(chainman, tx, utxos, script_payout, /*amount=*/1 * COIN);
    pro_tx.inputsHash = CalcTxInputsHash(CTransaction(tx));
    CMessageSigner::SignMessage(pro_tx.MakeSignString(), pro_tx.vchSig, collateral_key);
    SetTxPayload(tx, pro_tx);
    SignTransaction(tx, spent, coinbase_key);

    return tx;
}

CMutableTransaction CreateProRegTx(const ChainstateManager& chainman, SimpleUTXOMap& utxos, int port,
                                   const CScript& script_payout, const CKey& coinbase_key, CKey& owner_key_ret,
                                   CBLSSecretKey& operator_key_ret)
{
    owner_key_ret.MakeNewKey(true);
    operator_key_ret.MakeNewKey();

    CProRegTx pro_tx;
    pro_tx.nVersion = ProTxVersion::GetMax(!bls::bls_legacy_scheme, /*is_extended_addr=*/false);
    pro_tx.netInfo = NetInfoInterface::MakeNetInfo(pro_tx.nVersion);
    pro_tx.collateralOutpoint.n = 0;
    Assert(pro_tx.netInfo->AddEntry(NetInfoPurpose::CORE_P2P, strprintf("1.1.1.1:%d", port)) == NetInfoStatus::Success);
    pro_tx.keyIDOwner = owner_key_ret.GetPubKey().GetID();
    pro_tx.pubKeyOperator.Set(operator_key_ret.GetPublicKey(), bls::bls_legacy_scheme.load());
    pro_tx.keyIDVoting = owner_key_ret.GetPubKey().GetID();
    pro_tx.scriptPayout = script_payout;

    CMutableTransaction tx;
    tx.nVersion = 3;
    tx.nType = TRANSACTION_PROVIDER_REGISTER;
    const auto spent = FundTransaction(chainman, tx, utxos, script_payout, dmn_types::Regular.collat_amount);
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
