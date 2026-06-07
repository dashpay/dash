// Copyright (c) 2021-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/data/trivially_invalid.json.h>
#include <test/data/trivially_valid.json.h>
#include <test/util/json.h>
#include <test/util/setup_common.h>

#include <chain.h>
#include <chainparams.h>
#include <deploymentstatus.h>
#include <evo/providertx.h>
#include <key.h>
#include <primitives/transaction.h>
#include <script/script.h>
#include <script/standard.h>
#include <validation.h>

#include <boost/test/unit_test.hpp>

#include <univalue.h>

struct TestChain100V19Activation : public TestChain100Setup {
    TestChain100V19Activation()
        : TestChain100Setup{CBaseChainParams::REGTEST, {"-testactivationheight=v19@100"}} {}
};

BOOST_FIXTURE_TEST_SUITE(evo_trivialvalidation, TestChain100V19Activation)

template <class T>
void TestTxHelper(const CMutableTransaction& tx, gsl::not_null<const CBlockIndex*> pindexPrev,
                  const std::optional<std::string>& expected_error, const ChainstateManager& chainman)
{
    const bool payload_to_fail = expected_error.has_value() && expected_error.value() == "gettxpayload-fail";
    const auto opt_payload = GetTxPayload<T>(tx, false);
    BOOST_CHECK_EQUAL(opt_payload.has_value(), !payload_to_fail);

    // No need to check anything else if GetTxPayload() expected to fail
    if (payload_to_fail) return;

    TxValidationState dummy_state;
    BOOST_CHECK_EQUAL(opt_payload->IsTriviallyValid(pindexPrev, chainman, dummy_state), !expected_error.has_value());
    if (expected_error.has_value()) {
        BOOST_CHECK_EQUAL(dummy_state.GetRejectReason(), expected_error.value());
    }
}

void trivialvalidation_runner(const ChainstateManager& chainman, const std::string& json)
{
    const UniValue vectors = read_json(json);

    for (size_t idx = 1; idx < vectors.size(); idx++) {
        const UniValue& test = vectors[idx];
        uint256 txHash;
        std::string txType;
        CMutableTransaction tx;
        std::optional<std::string> expected_err;
        try {
            // Additional data
            txHash = uint256S(test[0].get_str());
            BOOST_TEST_MESSAGE("tx: " << test[0].get_str());
            txType = test[1].get_str();
            BOOST_CHECK(test[2].get_str() == "basic" || test[2].get_str() == "legacy");
            // Determine which pindexPrev to supply based on whether we want to validate legacy or basic
            // TODO: Introduce trivial validation test vectors for extended addresses
            const CBlockIndex* pindexPrev{(test[2].get_str() == "basic") ? chainman.ActiveChain()[100]
                                                                         : chainman.ActiveChain()[98]};
            assert(pindexPrev);
            // Raw transaction
            CDataStream stream(ParseHex(test[3].get_str()), SER_NETWORK, PROTOCOL_VERSION);
            stream >> tx;
            expected_err = (test.size() > 4) ? std::make_optional(test[4].get_str()) : std::nullopt;
            // Sanity check
            BOOST_CHECK_EQUAL(tx.nVersion, 3);
            BOOST_CHECK_EQUAL(tx.GetHash(), txHash);
            // Deserialization based on transaction nType
            TxValidationState dummy_state;
            switch (tx.nType) {
            case TRANSACTION_PROVIDER_REGISTER: {
                BOOST_CHECK_EQUAL(txType, "proregtx");

                TestTxHelper<CProRegTx>(tx, pindexPrev, expected_err, chainman);
                break;
            }
            case TRANSACTION_PROVIDER_UPDATE_SERVICE: {
                BOOST_CHECK_EQUAL(txType, "proupservtx");

                TestTxHelper<CProUpServTx>(tx, pindexPrev, expected_err, chainman);
                break;
            }
            case TRANSACTION_PROVIDER_UPDATE_REGISTRAR: {
                BOOST_CHECK_EQUAL(txType, "proupregtx");

                TestTxHelper<CProUpRegTx>(tx, pindexPrev, expected_err, chainman);
                break;
            }
            case TRANSACTION_PROVIDER_UPDATE_REVOKE: {
                BOOST_CHECK_EQUAL(txType, "prouprevtx");

                TestTxHelper<CProUpRevTx>(tx, pindexPrev, expected_err, chainman);
                break;
            }
            default:
                // TRANSACTION_COINBASE and TRANSACTION_NORMAL
                // are not subject to trivial validation checks
                BOOST_CHECK(false);
            }
        } catch (...) {
            BOOST_ERROR("Bad test, couldn't deserialize data: " << test.write());
            continue;
        }
    }
}

BOOST_AUTO_TEST_CASE(trivialvalidation_valid)
{
    const std::string json(json_tests::trivially_valid, json_tests::trivially_valid + sizeof(json_tests::trivially_valid));

    bls::bls_legacy_scheme.store(true);
    trivialvalidation_runner(*m_node.chainman, json);

    bls::bls_legacy_scheme.store(false);
    trivialvalidation_runner(*m_node.chainman, json);
}

BOOST_AUTO_TEST_CASE(trivialvalidation_invalid)
{
    const std::string json(json_tests::trivially_invalid, json_tests::trivially_invalid + sizeof(json_tests::trivially_invalid));

    bls::bls_legacy_scheme.store(true);
    trivialvalidation_runner(*m_node.chainman, json);

    bls::bls_legacy_scheme.store(false);
    trivialvalidation_runner(*m_node.chainman, json);
}

static CScript NewP2PKHScript(CKey& key)
{
    key.MakeNewKey(true);
    return GetScriptForDestination(PKHash(key.GetPubKey()));
}

static void CheckPayouts(const MasternodePayoutShares& payouts, const CKeyID& owner, const CKeyID& voting,
                         const std::optional<std::string>& expected_error)
{
    TxValidationState state;
    BOOST_CHECK_EQUAL(IsPayoutListTriviallyValid(payouts, owner, voting, state), !expected_error.has_value());
    if (expected_error.has_value()) {
        BOOST_CHECK_EQUAL(state.GetRejectReason(), *expected_error);
    }
}

BOOST_AUTO_TEST_CASE(multipayout_list_validation)
{
    CKey owner_key, voting_key, payout_key1, payout_key2, payout_key3;
    const CScript payout1 = NewP2PKHScript(payout_key1);
    const CScript payout2 = NewP2PKHScript(payout_key2);
    const CScript payout3 = NewP2PKHScript(payout_key3);
    owner_key.MakeNewKey(true);
    voting_key.MakeNewKey(true);
    const CKeyID owner_id = owner_key.GetPubKey().GetID();
    const CKeyID voting_id = voting_key.GetPubKey().GetID();

    CheckPayouts({{payout1, 10000}}, owner_id, voting_id, std::nullopt);
    CheckPayouts({{payout1, 5000}, {payout2, 3000}, {payout3, 2000}}, owner_id, voting_id, std::nullopt);
    CheckPayouts({}, owner_id, voting_id, "bad-protx-payouts-count");

    MasternodePayoutShares too_many;
    for (int i = 0; i < 9; ++i) {
        CKey key;
        too_many.emplace_back(NewP2PKHScript(key), 1000);
    }
    CheckPayouts(too_many, owner_id, voting_id, "bad-protx-payouts-count");

    CheckPayouts({{payout1, 99}, {payout2, 9901}}, owner_id, voting_id, "bad-protx-payout-reward");
    CheckPayouts({{payout1, CMasternodePayoutShare::MAX_REWARD + 1}}, owner_id, voting_id, "bad-protx-payout-reward");
    CheckPayouts({{payout1, 5000}, {payout2, 4999}}, owner_id, voting_id, "bad-protx-payout-reward-sum");
    CheckPayouts({{payout1, 5000}, {payout1, 5000}}, owner_id, voting_id, "bad-protx-payee-dup");
    CheckPayouts({{CScript() << OP_RETURN, 10000}}, owner_id, voting_id, "bad-protx-payee");
    CheckPayouts({{GetScriptForDestination(PKHash(owner_id)), 10000}}, owner_id, voting_id, "bad-protx-payee-reuse");
    CheckPayouts({{GetScriptForDestination(PKHash(voting_id)), 10000}}, owner_id, voting_id, "bad-protx-payee-reuse");

    TxValidationState state;
    BOOST_CHECK(!IsPayoutListKeySafe({{payout1, 10000}}, CTxDestination(PKHash(payout_key1.GetPubKey().GetID())),
                                     owner_id, voting_id, /*check_payout_collateral_reuse=*/true, state));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-protx-payee-reuse");
    state = TxValidationState{};
    BOOST_CHECK(IsPayoutListKeySafe({{payout1, 10000}}, CTxDestination(PKHash(payout_key1.GetPubKey().GetID())),
                                    owner_id, voting_id, /*check_payout_collateral_reuse=*/false, state));

    CScript p2sh_collateral = GetScriptForDestination(ScriptHash(payout1));
    CTxDestination p2sh_dest;
    BOOST_CHECK(ExtractDestination(p2sh_collateral, p2sh_dest));
    state = TxValidationState{};
    BOOST_CHECK(!IsPayoutListKeySafe({{p2sh_collateral, 10000}}, p2sh_dest, owner_id, voting_id,
                                     /*check_payout_collateral_reuse=*/true, state));
    BOOST_CHECK_EQUAL(state.GetRejectReason(), "bad-protx-payee-reuse");
}

BOOST_AUTO_TEST_SUITE_END()
