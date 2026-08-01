// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <test/util/setup_common.h>

#include <evo/deterministicmns.h>
#include <evo/dmn_types.h>
#include <evo/dmnstate.h>
#include <evo/providertx.h>
#include <evo/sharedcollateral.h>
#include <evo/specialtx.h>
#include <evo/specialtx_filter.h>
#include <evo/specialtxman.h>

#include <arith_uint256.h>
#include <clientversion.h>
#include <common/bloom.h>
#include <key.h>
#include <merkleblock.h>
#include <messagesigner.h>
#include <policy/policy.h>
#include <random.h>
#include <script/interpreter.h>
#include <script/script.h>
#include <script/standard.h>
#include <streams.h>
#include <util/strencodings.h>

#include <boost/test/unit_test.hpp>

#include <optional>

BOOST_FIXTURE_TEST_SUITE(evo_sharedmn_tests, BasicTestingSetup)

BOOST_AUTO_TEST_CASE(shared_collateral_template)
{
    const CScript& templ = sharedcollateral::SharedCollateralScript();

    // The template must be exactly the 7 bytes specified by the DIP
    BOOST_CHECK_EQUAL(HexStr(templ), "04445348437551");
    BOOST_CHECK(sharedcollateral::IsSharedCollateralScript(templ));

    // Matching is exact-script, never prefix or near-miss
    {
        CScript prefix_extended{templ};
        prefix_extended << OP_NOP;
        BOOST_CHECK(!sharedcollateral::IsSharedCollateralScript(prefix_extended));
    }
    {
        // Same tag, missing OP_TRUE
        CScript truncated = CScript() << std::vector<unsigned char>{'D', 'S', 'H', 'C'} << OP_DROP;
        BOOST_CHECK(!sharedcollateral::IsSharedCollateralScript(truncated));
    }
    {
        // Different tag byte
        CScript wrong_tag = CScript() << std::vector<unsigned char>{'D', 'S', 'H', 'D'} << OP_DROP << OP_TRUE;
        BOOST_CHECK(!sharedcollateral::IsSharedCollateralScript(wrong_tag));
    }
    BOOST_CHECK(!sharedcollateral::IsSharedCollateralScript(CScript()));
}

BOOST_AUTO_TEST_CASE(shared_collateral_template_spendable_at_script_layer)
{
    // The template must evaluate true with an empty scriptSig under standard
    // flags (including CLEANSTACK); all spend protection is consensus-level.
    ScriptError err;
    BOOST_CHECK(VerifyScript(CScript(), sharedcollateral::SharedCollateralScript(), STANDARD_SCRIPT_VERIFY_FLAGS,
                             BaseSignatureChecker(), &err));
    BOOST_CHECK_EQUAL(err, SCRIPT_ERR_OK);
}

static CScript NewP2PKHScript(CKey& key)
{
    key.MakeNewKey(true);
    return GetScriptForDestination(PKHash(key.GetPubKey()));
}

static CCollateralShare NewShare(CAmount amount, CKey& refund_key, CKey& owner_key)
{
    owner_key.MakeNewKey(true);
    return {amount, NewP2PKHScript(refund_key), CScript(), owner_key.GetPubKey().GetID()};
}

static std::vector<std::vector<unsigned char>> DummyJoinSigs(size_t count)
{
    return {count, std::vector<unsigned char>(CPubKey::COMPACT_SIGNATURE_SIZE, 0)};
}

static void CheckShares(const CollateralShares& shares, const std::vector<std::vector<unsigned char>>& join_sigs,
                        uint32_t early_period_blocks, CAmount early_penalty, const CKeyID& voting,
                        const std::optional<std::string>& expected_error)
{
    TxValidationState state;
    BOOST_CHECK_EQUAL(IsShareListTriviallyValid(shares, join_sigs, early_period_blocks, early_penalty,
                                                dmn_types::Regular.collat_amount, voting, state),
                      !expected_error.has_value());
    if (expected_error.has_value()) {
        BOOST_CHECK_EQUAL(state.GetRejectReason(), *expected_error);
    }
}

BOOST_AUTO_TEST_CASE(share_list_validation)
{
    CKey voting_key;
    voting_key.MakeNewKey(true);
    const CKeyID voting_id = voting_key.GetPubKey().GetID();

    CKey refund_keys[8], owner_keys[8];
    CollateralShares two_shares{NewShare(600 * COIN, refund_keys[0], owner_keys[0]),
                                NewShare(400 * COIN, refund_keys[1], owner_keys[1])};
    CheckShares(two_shares, DummyJoinSigs(2), 0, 0, voting_id, std::nullopt);
    CheckShares(two_shares, DummyJoinSigs(2), CProRegTx::MAX_EARLY_PERIOD_BLOCKS, 399 * COIN, voting_id, std::nullopt);

    CollateralShares eight_shares;
    for (size_t i = 0; i < 8; i++) {
        eight_shares.push_back(NewShare(125 * COIN, refund_keys[i], owner_keys[i]));
    }
    CheckShares(eight_shares, DummyJoinSigs(8), 100, 100 * COIN, voting_id, std::nullopt);

    // Count bounds: 0, 1 and 9 shares are invalid
    CheckShares({}, DummyJoinSigs(0), 0, 0, voting_id, "bad-protx-shares-count");
    CheckShares({two_shares[0]}, DummyJoinSigs(1), 0, 0, voting_id, "bad-protx-shares-count");
    {
        CollateralShares nine_shares{eight_shares};
        CKey k1, k2;
        nine_shares.push_back(NewShare(125 * COIN, k1, k2));
        CheckShares(nine_shares, DummyJoinSigs(9), 0, 0, voting_id, "bad-protx-shares-count");
    }

    // One join signature per share, each 65 bytes
    CheckShares(two_shares, DummyJoinSigs(1), 0, 0, voting_id, "bad-protx-shares-sig-count");
    CheckShares(two_shares, DummyJoinSigs(3), 0, 0, voting_id, "bad-protx-shares-sig-count");
    {
        auto bad_sigs = DummyJoinSigs(2);
        bad_sigs[1].resize(64);
        CheckShares(two_shares, bad_sigs, 0, 0, voting_id, "bad-protx-shares-sig-size");
    }

    // Early period cap and penalty bounds
    CheckShares(two_shares, DummyJoinSigs(2), CProRegTx::MAX_EARLY_PERIOD_BLOCKS + 1, 0, voting_id,
                "bad-protx-shares-early-period");
    CheckShares(two_shares, DummyJoinSigs(2), 100, -1, voting_id, "bad-protx-shares-penalty");
    // earlyPenalty must be strictly below the smallest share amount
    CheckShares(two_shares, DummyJoinSigs(2), 100, 400 * COIN, voting_id, "bad-protx-shares-penalty");

    // Amount bounds: below the 100 DASH minimum and sums different from the collateral
    {
        CollateralShares shares{two_shares};
        shares[0].amount = 99 * COIN;
        CheckShares(shares, DummyJoinSigs(2), 0, 0, voting_id, "bad-protx-shares-amount");
        shares[0].amount = 599 * COIN;
        CheckShares(shares, DummyJoinSigs(2), 0, 0, voting_id, "bad-protx-shares-amount-sum");
        shares[0].amount = 601 * COIN;
        CheckShares(shares, DummyJoinSigs(2), 0, 0, voting_id, "bad-protx-shares-amount-sum");
    }

    // Duplicate owner keys and refund scripts within the table
    {
        CollateralShares shares{two_shares};
        shares[1].keyIDOwner = shares[0].keyIDOwner;
        CheckShares(shares, DummyJoinSigs(2), 0, 0, voting_id, "bad-protx-shares-dup-key");
    }
    {
        CollateralShares shares{two_shares};
        shares[1].scriptRefund = shares[0].scriptRefund;
        CheckShares(shares, DummyJoinSigs(2), 0, 0, voting_id, "bad-protx-shares-dup-refund");
    }
    // Duplicate reward scripts are allowed
    {
        CollateralShares shares{two_shares};
        CKey reward_key;
        const CScript reward = NewP2PKHScript(reward_key);
        shares[0].scriptReward = reward;
        shares[1].scriptReward = reward;
        CheckShares(shares, DummyJoinSigs(2), 0, 0, voting_id, std::nullopt);
    }

    // Null owner key
    {
        CollateralShares shares{two_shares};
        shares[0].keyIDOwner = CKeyID();
        CheckShares(shares, DummyJoinSigs(2), 0, 0, voting_id, "bad-protx-shares-key-null");
    }

    // Script type restrictions: template, non-standard, P2SH allowed
    {
        CollateralShares shares{two_shares};
        shares[0].scriptReward = sharedcollateral::SharedCollateralScript();
        CheckShares(shares, DummyJoinSigs(2), 0, 0, voting_id, "bad-protx-shares-payee-template");
        shares[0].scriptReward = CScript() << OP_TRUE;
        CheckShares(shares, DummyJoinSigs(2), 0, 0, voting_id, "bad-protx-shares-payee");
        shares[0].scriptReward = GetScriptForDestination(ScriptHash(CScript() << OP_TRUE));
        CheckShares(shares, DummyJoinSigs(2), 0, 0, voting_id, std::nullopt);
    }
    {
        CollateralShares shares{two_shares};
        shares[0].scriptRefund = sharedcollateral::SharedCollateralScript();
        CheckShares(shares, DummyJoinSigs(2), 0, 0, voting_id, "bad-protx-shares-payee-template");
    }

    // Refund/reward scripts must not pay any table owner key or the voting key
    {
        CollateralShares shares{two_shares};
        shares[0].scriptReward = GetScriptForDestination(PKHash(shares[1].keyIDOwner));
        CheckShares(shares, DummyJoinSigs(2), 0, 0, voting_id, "bad-protx-shares-payee-reuse");
        shares[0].scriptReward = GetScriptForDestination(PKHash(voting_key.GetPubKey()));
        CheckShares(shares, DummyJoinSigs(2), 0, 0, voting_id, "bad-protx-shares-payee-reuse");
    }
}

BOOST_AUTO_TEST_CASE(shared_proregtx_serialization)
{
    CKey refund_keys[8], owner_keys[8], voting_key, collateral_key;
    voting_key.MakeNewKey(true);

    for (const size_t count : {size_t{2}, size_t{8}}) {
        CProRegTx proTx;
        proTx.nVersion = ProTxVersion::ExtAddr;
        proTx.netInfo = NetInfoInterface::MakeNetInfo(proTx.nVersion);
        proTx.keyIDVoting = voting_key.GetPubKey().GetID();
        proTx.collateralOutpoint = COutPoint(uint256(), 0);
        for (size_t i = 0; i < count; i++) {
            proTx.shares.push_back(NewShare(dmn_types::Regular.collat_amount / count, refund_keys[i], owner_keys[i]));
        }
        proTx.vchJoinSigs = DummyJoinSigs(count);
        proTx.nEarlyPeriodBlocks = 1000;
        proTx.nEarlyPenalty = 50 * COIN;

        CDataStream ss(SER_NETWORK, CLIENT_VERSION);
        ss << proTx;
        CProRegTx proTx2;
        ss >> proTx2;

        BOOST_CHECK(proTx2.IsShared());
        BOOST_CHECK_EQUAL(proTx2.shares.size(), count);
        BOOST_CHECK(proTx2.shares == proTx.shares);
        BOOST_CHECK(proTx2.vchJoinSigs == proTx.vchJoinSigs);
        BOOST_CHECK_EQUAL(proTx2.nEarlyPeriodBlocks, proTx.nEarlyPeriodBlocks);
        BOOST_CHECK_EQUAL(proTx2.nEarlyPenalty, proTx.nEarlyPenalty);

        // Serialization must be deterministic and round-trip byte-identically
        CDataStream ss2(SER_NETWORK, CLIENT_VERSION);
        ss2 << proTx2;
        CDataStream ss3(SER_NETWORK, CLIENT_VERSION);
        ss3 << proTx;
        BOOST_CHECK(ss2.str() == ss3.str());
    }

    // A join-signature count differing from the share count cannot round-trip, so serialization
    // refuses to emit it
    {
        CProRegTx mismatched;
        mismatched.nVersion = ProTxVersion::ExtAddr;
        mismatched.netInfo = NetInfoInterface::MakeNetInfo(mismatched.nVersion);
        mismatched.shares.push_back(NewShare(600 * COIN, refund_keys[0], owner_keys[0]));
        mismatched.shares.push_back(NewShare(400 * COIN, refund_keys[1], owner_keys[1]));
        mismatched.vchJoinSigs = DummyJoinSigs(1);
        CDataStream ss_mismatch(SER_NETWORK, CLIENT_VERSION);
        BOOST_CHECK_THROW(ss_mismatch << mismatched, std::ios_base::failure);
    }

    // A non-shared extended payload serializes an empty share list and zeroed penalty fields
    CProRegTx nonShared;
    nonShared.nVersion = ProTxVersion::ExtAddr;
    nonShared.netInfo = NetInfoInterface::MakeNetInfo(nonShared.nVersion);
    CDataStream ss(SER_NETWORK, CLIENT_VERSION);
    ss << nonShared;
    CProRegTx nonShared2;
    ss >> nonShared2;
    BOOST_CHECK(!nonShared2.IsShared());
    BOOST_CHECK(nonShared2.vchJoinSigs.empty());
    BOOST_CHECK_EQUAL(nonShared2.nEarlyPeriodBlocks, uint32_t{0});
    BOOST_CHECK_EQUAL(nonShared2.nEarlyPenalty, CAmount{0});
}

BOOST_AUTO_TEST_CASE(canonical_signature_verification)
{
    CKey key;
    key.MakeNewKey(true);
    const CKeyID key_id = key.GetPubKey().GetID();
    const uint256 hash = GetRandHash();

    std::vector<unsigned char> sig;
    BOOST_REQUIRE(CHashSigner::SignHash(hash, key, sig));
    BOOST_REQUIRE_EQUAL(sig.size(), CPubKey::COMPACT_SIGNATURE_SIZE);

    std::string err;
    BOOST_CHECK(CHashSigner::VerifyHashCanonical(hash, key_id, sig, err));

    // Wrong size is rejected
    {
        auto short_sig = sig;
        short_sig.resize(64);
        BOOST_CHECK(!CHashSigner::VerifyHashCanonical(hash, key_id, short_sig, err));
    }

    // Malleate to the high-S form: s' = N - s, recovery id parity flips. The plain VerifyHash
    // accepts it (same key recovered), the canonical form must reject it.
    {
        auto high_s = sig;
        static constexpr unsigned char ORDER[32] = {
            0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
            0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xfe,
            0xba, 0xae, 0xdc, 0xe6, 0xaf, 0x48, 0xa0, 0x3b,
            0xbf, 0xd2, 0x5e, 0x8c, 0xd0, 0x36, 0x41, 0x41,
        };
        int borrow{0};
        for (int i = 31; i >= 0; i--) {
            const int diff = int{ORDER[i]} - int{high_s[33 + i]} - borrow;
            high_s[33 + i] = static_cast<unsigned char>(diff & 0xff);
            borrow = diff < 0 ? 1 : 0;
        }
        BOOST_REQUIRE_EQUAL(borrow, 0);
        // negating S flips the parity of the recovery id (the low two bits of header - 27);
        // the compression flag (bit 2) must be preserved
        const int header = high_s[0] - 27;
        high_s[0] = static_cast<unsigned char>(27 + (header & 4) + ((header & 3) ^ 1));

        BOOST_CHECK(CHashSigner::VerifyHash(hash, key_id, high_s, err));
        BOOST_CHECK(!CHashSigner::VerifyHashCanonical(hash, key_id, high_s, err));
    }

    // Non-canonical recovery header aliases must be rejected. RecoverCompact only masks
    // (header - 27) with 3 and 4, so header + 8*k recovers the same key; the plain VerifyHash
    // accepts such aliases, the canonical form must not (else the txid stays malleable).
    {
        BOOST_REQUIRE(sig[0] >= 27 && sig[0] <= 34);
        auto aliased = sig;
        aliased[0] = static_cast<unsigned char>(sig[0] + 8);
        BOOST_CHECK(CHashSigner::VerifyHash(hash, key_id, aliased, err));
        BOOST_CHECK(!CHashSigner::VerifyHashCanonical(hash, key_id, aliased, err));
    }

    // Wrong key is rejected
    {
        CKey other;
        other.MakeNewKey(true);
        BOOST_CHECK(!CHashSigner::VerifyHashCanonical(hash, other.GetPubKey().GetID(), sig, err));
    }
}

BOOST_AUTO_TEST_CASE(shared_reg_consent_hash)
{
    CKey refund_keys[2], owner_keys[2], voting_key;
    voting_key.MakeNewKey(true);

    CProRegTx proTx;
    proTx.nVersion = ProTxVersion::ExtAddr;
    proTx.netInfo = NetInfoInterface::MakeNetInfo(proTx.nVersion);
    proTx.keyIDVoting = voting_key.GetPubKey().GetID();
    proTx.collateralOutpoint = COutPoint(uint256(), 0);
    proTx.shares.push_back(NewShare(600 * COIN, refund_keys[0], owner_keys[0]));
    proTx.shares.push_back(NewShare(400 * COIN, refund_keys[1], owner_keys[1]));
    proTx.vchJoinSigs = DummyJoinSigs(2);
    proTx.nEarlyPeriodBlocks = 1000;
    proTx.nEarlyPenalty = 50 * COIN;

    CMutableTransaction mtx;
    mtx.nVersion = 3;
    mtx.nType = TRANSACTION_PROVIDER_REGISTER;
    mtx.vin.emplace_back(COutPoint(GetRandHash(), 0));
    mtx.vin.emplace_back(COutPoint(GetRandHash(), 1));
    mtx.vout.emplace_back(dmn_types::Regular.collat_amount, sharedcollateral::SharedCollateralScript());
    CKey change_key;
    mtx.vout.emplace_back(5 * COIN, NewP2PKHScript(change_key));

    const uint256 base = proTx.MakeSharedRegConsentHash(CTransaction(mtx));
    BOOST_CHECK(!base.IsNull());

    // Signatures are not covered: filling in a joinSig must not change the digest
    {
        CProRegTx mutated{proTx};
        mutated.vchJoinSigs[0][0] = 0x42;
        BOOST_CHECK(mutated.MakeSharedRegConsentHash(CTransaction(mtx)) == base);
    }

    // Every covered field changes the digest
    {
        CProRegTx mutated{proTx};
        mutated.shares[0].amount += 1;
        BOOST_CHECK(mutated.MakeSharedRegConsentHash(CTransaction(mtx)) != base);
    }
    {
        CProRegTx mutated{proTx};
        mutated.nEarlyPenalty += 1;
        BOOST_CHECK(mutated.MakeSharedRegConsentHash(CTransaction(mtx)) != base);
    }
    {
        CProRegTx mutated{proTx};
        mutated.nEarlyPeriodBlocks += 1;
        BOOST_CHECK(mutated.MakeSharedRegConsentHash(CTransaction(mtx)) != base);
    }
    {
        CProRegTx mutated{proTx};
        mutated.nOperatorReward = 100;
        BOOST_CHECK(mutated.MakeSharedRegConsentHash(CTransaction(mtx)) != base);
    }
    {
        CProRegTx mutated{proTx};
        CKey new_voting;
        new_voting.MakeNewKey(true);
        mutated.keyIDVoting = new_voting.GetPubKey().GetID();
        BOOST_CHECK(mutated.MakeSharedRegConsentHash(CTransaction(mtx)) != base);
    }
    {
        // Funding inputs are covered
        CMutableTransaction mtx2{mtx};
        mtx2.vin[0].prevout.n = 5;
        BOOST_CHECK(proTx.MakeSharedRegConsentHash(CTransaction(mtx2)) != base);
    }
    {
        // All outputs are covered, including change
        CMutableTransaction mtx2{mtx};
        mtx2.vout[1].nValue += 1;
        BOOST_CHECK(proTx.MakeSharedRegConsentHash(CTransaction(mtx2)) != base);
    }
    {
        // Input sequences are covered: an uncovered sequence rewrite could impose a BIP68
        // relative timelock on a fully consented registration
        CMutableTransaction mtx2{mtx};
        mtx2.vin[1].nSequence = 0xffff;
        BOOST_CHECK(proTx.MakeSharedRegConsentHash(CTransaction(mtx2)) != base);
    }
    {
        // nLockTime is covered
        CMutableTransaction mtx2{mtx};
        mtx2.nLockTime += 1;
        BOOST_CHECK(proTx.MakeSharedRegConsentHash(CTransaction(mtx2)) != base);
    }
}

BOOST_AUTO_TEST_CASE(split_amount_by_shares)
{
    CKey refund_keys[3], owner_keys[3];
    CollateralShares shares{NewShare(600 * COIN, refund_keys[0], owner_keys[0]),
                            NewShare(300 * COIN, refund_keys[1], owner_keys[1]),
                            NewShare(100 * COIN, refund_keys[2], owner_keys[2])};

    // Conservation and remainder-to-last for an amount that doesn't divide evenly
    const CAmount total{123456789};
    const auto amounts = SplitAmountByShares(total, shares);
    BOOST_REQUIRE_EQUAL(amounts.size(), shares.size());
    BOOST_CHECK_EQUAL(amounts[0] + amounts[1] + amounts[2], total);
    BOOST_CHECK_EQUAL(amounts[0], total * 6 / 10);
    BOOST_CHECK_EQUAL(amounts[1], total * 3 / 10);
    BOOST_CHECK_EQUAL(amounts[2], total - amounts[0] - amounts[1]);

    // No 64-bit overflow for realistic values: reward in duffs times a 1000 DASH share
    const auto big = SplitAmountByShares(566 * COIN, shares);
    BOOST_CHECK_EQUAL(big[0] + big[1] + big[2], 566 * COIN);
    BOOST_CHECK_EQUAL(big[0], 566 * COIN * 6 / 10);
}

BOOST_AUTO_TEST_CASE(dmn_state_shares_roundtrip)
{
    CKey refund_keys[2], owner_keys[2], voting_key;
    voting_key.MakeNewKey(true);

    CProRegTx proTx;
    proTx.nVersion = ProTxVersion::ExtAddr;
    proTx.netInfo = NetInfoInterface::MakeNetInfo(proTx.nVersion);
    proTx.keyIDVoting = voting_key.GetPubKey().GetID();
    proTx.shares.push_back(NewShare(600 * COIN, refund_keys[0], owner_keys[0]));
    proTx.shares.push_back(NewShare(400 * COIN, refund_keys[1], owner_keys[1]));
    proTx.vchJoinSigs = DummyJoinSigs(2);
    proTx.nEarlyPeriodBlocks = 123;
    proTx.nEarlyPenalty = 7 * COIN;

    CDeterministicMNState state(proTx);
    BOOST_CHECK(state.IsShared());
    BOOST_CHECK(state.shares == proTx.shares);
    BOOST_CHECK_EQUAL(state.nEarlyPeriodBlocks, proTx.nEarlyPeriodBlocks);
    BOOST_CHECK_EQUAL(state.nEarlyPenalty, proTx.nEarlyPenalty);

    CDataStream ss(SER_DISK, CLIENT_VERSION);
    ss << state;
    CDeterministicMNState state2;
    ss >> state2;
    BOOST_CHECK(state2.shares == state.shares);
    BOOST_CHECK_EQUAL(state2.nEarlyPeriodBlocks, state.nEarlyPeriodBlocks);
    BOOST_CHECK_EQUAL(state2.nEarlyPenalty, state.nEarlyPenalty);

    // The owner reward scripts of a shared masternode are the per-share effective reward scripts
    {
        const auto scripts = state.GetOwnerRewardScripts();
        BOOST_REQUIRE_EQUAL(scripts.size(), state.shares.size());
        for (size_t i = 0; i < scripts.size(); i++) {
            BOOST_CHECK(scripts[i] == state.shares[i].RewardScript());
        }
    }

    // A reward-script change is reported as one logical field carrying the whole table
    CDeterministicMNState changed{state};
    CKey reward_key;
    changed.shares[1].scriptReward = NewP2PKHScript(reward_key);
    CDeterministicMNStateDiff diff(state, changed);
    BOOST_CHECK(diff.fields & CDeterministicMNStateDiff::Field_shares);
    BOOST_CHECK(!(diff.fields & CDeterministicMNStateDiff::Field_nEarlyPeriodBlocks));

    CDataStream ss_diff(SER_DISK, CLIENT_VERSION);
    ss_diff << diff;
    CDeterministicMNStateDiff diff2;
    ss_diff >> diff2;
    CDeterministicMNState applied{state};
    diff2.ApplyToState(applied);
    BOOST_CHECK(applied.shares == changed.shares);
    // ... and the owner reward scripts follow the update
    BOOST_CHECK(applied.GetOwnerRewardScripts()[1] == changed.shares[1].scriptReward);
}

BOOST_AUTO_TEST_CASE(shared_unique_properties)
{
    CKey refund_keys[2], owner_keys[2], voting_key;
    voting_key.MakeNewKey(true);

    CProRegTx proTx;
    proTx.nVersion = ProTxVersion::ExtAddr;
    proTx.netInfo = NetInfoInterface::MakeNetInfo(proTx.nVersion);
    proTx.keyIDVoting = voting_key.GetPubKey().GetID();
    proTx.shares.push_back(NewShare(600 * COIN, refund_keys[0], owner_keys[0]));
    proTx.shares.push_back(NewShare(400 * COIN, refund_keys[1], owner_keys[1]));

    CDeterministicMNList list(uint256(), 0, 0);
    auto dmn = std::make_shared<CDeterministicMN>(0, MnType::Regular);
    dmn->proTxHash = GetRandHash();
    dmn->collateralOutpoint = COutPoint(dmn->proTxHash, 0);
    dmn->pdmnState = std::make_shared<CDeterministicMNState>(proTx);
    list.AddMN(dmn);

    // Share owner keys land in the same uniqueness namespace as keyIDOwner: a normal
    // registration reusing a share owner key must be detected, and vice versa. This relies on
    // GetUniquePropertyHash being an untagged SerializeHash of the CKeyID.
    BOOST_CHECK(list.HasUniqueProperty(owner_keys[0].GetPubKey().GetID()));
    BOOST_CHECK(list.HasUniqueProperty(owner_keys[1].GetPubKey().GetID()));
    // the null keyIDOwner of a shared masternode is never registered
    BOOST_CHECK(!list.HasUniqueProperty(CKeyID()));

    list.RemoveMN(dmn->proTxHash);
    BOOST_CHECK(!list.HasUniqueProperty(owner_keys[0].GetPubKey().GetID()));
    BOOST_CHECK(!list.HasUniqueProperty(owner_keys[1].GetPubKey().GetID()));
}

struct ProDisTestSetup {
    CKey refund_keys[3], owner_keys[3], voting_key;
    CDeterministicMNList list{uint256(), 0, 0};
    std::shared_ptr<CDeterministicMN> dmn;
    static constexpr int REGISTERED_HEIGHT{1000};
    static constexpr uint32_t EARLY_PERIOD{100};
    static constexpr CAmount PENALTY{50 * COIN};
    static constexpr CAmount FEE{100000};

    ProDisTestSetup()
    {
        voting_key.MakeNewKey(true);
        CProRegTx proTx;
        proTx.nVersion = ProTxVersion::ExtAddr;
        proTx.netInfo = NetInfoInterface::MakeNetInfo(proTx.nVersion);
        proTx.keyIDVoting = voting_key.GetPubKey().GetID();
        proTx.shares.push_back(NewShareFor(500 * COIN, refund_keys[0], owner_keys[0]));
        proTx.shares.push_back(NewShareFor(300 * COIN, refund_keys[1], owner_keys[1]));
        proTx.shares.push_back(NewShareFor(200 * COIN, refund_keys[2], owner_keys[2]));
        proTx.nEarlyPeriodBlocks = EARLY_PERIOD;
        proTx.nEarlyPenalty = PENALTY;

        dmn = std::make_shared<CDeterministicMN>(0, MnType::Regular);
        dmn->proTxHash = GetRandHash();
        dmn->collateralOutpoint = COutPoint(dmn->proTxHash, 0);
        auto state = std::make_shared<CDeterministicMNState>(proTx);
        state->nRegisteredHeight = REGISTERED_HEIGHT;
        dmn->pdmnState = state;
        list.AddMN(dmn);
    }

    static CCollateralShare NewShareFor(CAmount amount, CKey& refund_key, CKey& owner_key)
    {
        return NewShare(amount, refund_key, owner_key);
    }

    const CollateralShares& Shares() const { return dmn->pdmnState->shares; }

    //! Builds a well-formed ProDisTx: non-actor refunds in share order (+pro-rata penalty,
    //! remainder to last), optional actor output, and valid signatures
    CMutableTransaction BuildTx(uint16_t actor, CAmount penalty, bool unanimous, CProDisTx& ptxRet) const
    {
        const auto& shares = Shares();
        CMutableTransaction tx;
        tx.nVersion = 3;
        tx.nType = TRANSACTION_PROVIDER_DISSOLVE;
        tx.vin.emplace_back(dmn->collateralOutpoint);

        CAmount W{0};
        size_t last_non_actor{0};
        for (size_t i = 0; i < shares.size(); i++) {
            if (i != actor) {
                W += shares[i].amount;
                last_non_actor = i;
            }
        }
        CAmount distributed{0};
        for (size_t i = 0; i < shares.size(); i++) {
            if (i == actor) continue;
            CAmount bonus;
            if (i == last_non_actor) {
                bonus = penalty - distributed;
            } else {
                // 128-bit intermediate, like the consensus code
                arith_uint256 v{static_cast<uint64_t>(penalty)};
                v *= arith_uint256{static_cast<uint64_t>(shares[i].amount)};
                v /= arith_uint256{static_cast<uint64_t>(W)};
                bonus = static_cast<CAmount>(v.GetLow64());
            }
            distributed += bonus;
            tx.vout.emplace_back(shares[i].amount + bonus, shares[i].scriptRefund);
        }
        const CAmount actor_out = shares[actor].amount - penalty - FEE;
        if (actor_out > 0) {
            tx.vout.emplace_back(actor_out, shares[actor].scriptRefund);
        }

        ptxRet = CProDisTx();
        ptxRet.proTxHash = dmn->proTxHash;
        ptxRet.actorIndex = actor;
        Sign(tx, ptxRet, unanimous);
        return tx;
    }

    void Sign(const CMutableTransaction& tx, CProDisTx& ptx, bool unanimous) const
    {
        const uint8_t sig_count{unanimous ? static_cast<uint8_t>(std::size(owner_keys)) : uint8_t{1}};
        const uint256 hash = ptx.MakeSignHash(CTransaction(tx), sig_count);
        ptx.vchSigs.clear();
        if (unanimous) {
            for (const auto& key : owner_keys) {
                std::vector<unsigned char> sig;
                BOOST_REQUIRE(CHashSigner::SignHash(hash, key, sig));
                ptx.vchSigs.push_back(sig);
            }
        } else {
            std::vector<unsigned char> sig;
            BOOST_REQUIRE(CHashSigner::SignHash(hash, owner_keys[ptx.actorIndex], sig));
            ptx.vchSigs.push_back(sig);
        }
    }

    void Check(const CMutableTransaction& tx, const CProDisTx& ptx, int spend_height,
               const std::optional<std::string>& expected_error) const
    {
        TxValidationState state;
        BOOST_CHECK_EQUAL(CheckProDisTxForList(CTransaction(tx), ptx, list, spend_height, state, /*check_sigs=*/true),
                          !expected_error.has_value());
        if (expected_error.has_value()) {
            BOOST_CHECK_EQUAL(state.GetRejectReason(), *expected_error);
        }
    }
};

BOOST_AUTO_TEST_CASE(prodis_validation)
{
    ProDisTestSetup t;
    constexpr int IN_EARLY{ProDisTestSetup::REGISTERED_HEIGHT + 50};
    constexpr int AFTER_EARLY{ProDisTestSetup::REGISTERED_HEIGHT + ProDisTestSetup::EARLY_PERIOD};

    // Unilateral with penalty: valid during the early period and, by monotonicity, forever after
    {
        CProDisTx ptx;
        const auto tx = t.BuildTx(/*actor=*/2, ProDisTestSetup::PENALTY, /*unanimous=*/false, ptx);
        t.Check(tx, ptx, IN_EARLY, std::nullopt);
        t.Check(tx, ptx, AFTER_EARLY + 100000, std::nullopt);
    }
    // Unilateral without penalty: invalid during the early period, valid at and after the boundary
    {
        CProDisTx ptx;
        const auto tx = t.BuildTx(2, 0, false, ptx);
        t.Check(tx, ptx, IN_EARLY, "bad-prodis-penalty-floor");
        t.Check(tx, ptx, AFTER_EARLY, std::nullopt);
    }
    // Unanimous without penalty: valid at any height
    {
        CProDisTx ptx;
        const auto tx = t.BuildTx(0, 0, true, ptx);
        t.Check(tx, ptx, IN_EARLY, std::nullopt);
    }
    // Overpaying the penalty is always valid
    {
        CProDisTx ptx;
        const auto tx = t.BuildTx(2, ProDisTestSetup::PENALTY + 12345, false, ptx);
        t.Check(tx, ptx, IN_EARLY, std::nullopt);
    }
    // Unknown masternode / not-shared handled by callers; unknown hash here
    {
        CProDisTx ptx;
        auto tx = t.BuildTx(0, 0, true, ptx);
        ptx.proTxHash = GetRandHash();
        t.Check(tx, ptx, IN_EARLY, "bad-prodis-hash");
    }
    // Wrong input, extra input, non-empty scriptSig
    {
        CProDisTx ptx;
        auto tx = t.BuildTx(0, 0, true, ptx);
        tx.vin[0].prevout.n = 1;
        t.Sign(tx, ptx, true);
        t.Check(tx, ptx, IN_EARLY, "bad-prodis-input");
    }
    {
        CProDisTx ptx;
        auto tx = t.BuildTx(0, 0, true, ptx);
        tx.vin.emplace_back(COutPoint(GetRandHash(), 0));
        t.Sign(tx, ptx, true);
        t.Check(tx, ptx, IN_EARLY, "bad-prodis-input");
    }
    {
        CProDisTx ptx;
        auto tx = t.BuildTx(0, 0, true, ptx);
        tx.vin[0].scriptSig = CScript() << OP_TRUE;
        t.Sign(tx, ptx, true);
        t.Check(tx, ptx, IN_EARLY, "bad-prodis-input");
    }
    // Actor index out of range
    {
        CProDisTx ptx;
        auto tx = t.BuildTx(0, 0, true, ptx);
        ptx.actorIndex = 3;
        t.Sign(tx, ptx, true);
        t.Check(tx, ptx, IN_EARLY, "bad-prodis-actor");
    }
    // Signature count other than 1 or sharesCount
    {
        CProDisTx ptx;
        auto tx = t.BuildTx(0, 0, true, ptx);
        ptx.vchSigs.resize(2);
        t.Check(tx, ptx, IN_EARLY, "bad-prodis-sig-count");
    }
    // Wrong actor signature (signed by a non-actor share)
    {
        CProDisTx ptx;
        auto tx = t.BuildTx(2, ProDisTestSetup::PENALTY, false, ptx);
        std::vector<unsigned char> sig;
        BOOST_REQUIRE(CHashSigner::SignHash(ptx.MakeSignHash(CTransaction(tx), /*sig_count=*/1), t.owner_keys[0], sig));
        ptx.vchSigs = {sig};
        t.Check(tx, ptx, IN_EARLY, "bad-prodis-sig");
    }
    // Unanimous signatures out of share order
    {
        CProDisTx ptx;
        auto tx = t.BuildTx(0, 0, true, ptx);
        std::swap(ptx.vchSigs[0], ptx.vchSigs[1]);
        t.Check(tx, ptx, IN_EARLY, "bad-prodis-sig");
    }
    // Non-malleable mode: a penalty-free unanimous dissolution cannot be stripped to just the
    // actor's signature and re-interpreted as unilateral. Outputs are byte-identical to a valid
    // unilateral penalty-free dissolution, but the retained signature committed to sigCount=3, so
    // verifying it as a 1-signature transaction (which recomputes the digest with sigCount=1) fails.
    {
        CProDisTx ptx;
        auto tx = t.BuildTx(0, 0, true, ptx); // unanimous, penalty 0, actor 0
        ptx.vchSigs = {ptx.vchSigs[0]};       // keep only the actor share's signature
        t.Check(tx, ptx, AFTER_EARLY, "bad-prodis-sig");
    }
    // Redirected refund output
    {
        CProDisTx ptx;
        auto tx = t.BuildTx(0, 0, true, ptx);
        tx.vout[0].scriptPubKey = t.Shares()[0].scriptRefund; // should be share 1's refund
        t.Sign(tx, ptx, true);
        t.Check(tx, ptx, IN_EARLY, "bad-prodis-payee");
    }
    // Reordered refund outputs
    {
        CProDisTx ptx;
        auto tx = t.BuildTx(0, 0, true, ptx);
        std::swap(tx.vout[0], tx.vout[1]);
        t.Sign(tx, ptx, true);
        t.Check(tx, ptx, IN_EARLY, "bad-prodis-payee");
    }
    // Extra output
    {
        CProDisTx ptx;
        auto tx = t.BuildTx(0, 0, true, ptx);
        tx.vout.emplace_back(1, t.Shares()[0].scriptRefund);
        t.Sign(tx, ptx, true);
        t.Check(tx, ptx, IN_EARLY, "bad-prodis-payee-count");
    }
    // Underpaying one non-actor share fails the per-recipient floor
    {
        CProDisTx ptx;
        auto tx = t.BuildTx(2, ProDisTestSetup::PENALTY, false, ptx);
        tx.vout[0].nValue -= 1;
        t.Sign(tx, ptx, false);
        t.Check(tx, ptx, IN_EARLY, "bad-prodis-penalty-floor");
    }
    // Zero-value actor output must be omitted
    {
        CProDisTx ptx;
        auto tx = t.BuildTx(0, 0, true, ptx);
        // replace the actor output with a zero-value one; the output count stays valid
        tx.vout.pop_back();
        tx.vout.emplace_back(0, t.Shares()[0].scriptRefund);
        t.Sign(tx, ptx, true);
        t.Check(tx, ptx, IN_EARLY, "bad-prodis-actor-output-zero");
    }
}

BOOST_AUTO_TEST_CASE(shared_tx_filter_matching)
{
    CKey refund_keys[2], owner_keys[2], reward_key, voting_key, new_voting_key;
    voting_key.MakeNewKey(true);
    new_voting_key.MakeNewKey(true);

    CProRegTx proTx;
    proTx.nVersion = ProTxVersion::ExtAddr;
    proTx.netInfo = NetInfoInterface::MakeNetInfo(proTx.nVersion);
    proTx.keyIDVoting = voting_key.GetPubKey().GetID();
    proTx.collateralOutpoint = COutPoint(uint256(), 0);
    proTx.shares.push_back(NewShare(600 * COIN, refund_keys[0], owner_keys[0]));
    proTx.shares.push_back(NewShare(400 * COIN, refund_keys[1], owner_keys[1]));
    proTx.shares[1].scriptReward = NewP2PKHScript(reward_key);
    proTx.vchJoinSigs = DummyJoinSigs(2);

    CMutableTransaction reg_mtx;
    reg_mtx.nVersion = 3;
    reg_mtx.nType = TRANSACTION_PROVIDER_REGISTER;
    reg_mtx.vin.emplace_back(COutPoint(GetRandHash(), 0));
    reg_mtx.vout.emplace_back(dmn_types::Regular.collat_amount, sharedcollateral::SharedCollateralScript());
    SetTxPayload(reg_mtx, proTx);
    const CTransaction reg_tx(reg_mtx);

    const auto hash_vec = [](const auto& h) { return std::vector<unsigned char>(h.begin(), h.end()); };
    const auto script_vec = [](const CScript& script) { return std::vector<unsigned char>(script.begin(), script.end()); };
    // Fresh single-element filter each time so one match cannot bleed into the next check
    const auto matches = [](const CTransaction& tx, const std::vector<unsigned char>& element) {
        CBloomFilter filter(20, 0.001, 0, BLOOM_UPDATE_NONE);
        filter.insert(element);
        return filter.IsRelevantAndUpdate(tx);
    };

    // A registration matches on every share's refund script, effective reward script and owner
    // key id, so a light client sees the formation whichever of its own elements it watches
    for (size_t i = 0; i < proTx.shares.size(); i++) {
        BOOST_CHECK(matches(reg_tx, hash_vec(refund_keys[i].GetPubKey().GetID())));
        BOOST_CHECK(matches(reg_tx, hash_vec(proTx.shares[i].keyIDOwner)));
    }
    BOOST_CHECK(matches(reg_tx, hash_vec(reward_key.GetPubKey().GetID())));
    BOOST_CHECK(!matches(reg_tx, hash_vec(GetRandHash())));

    // The lifecycle transactions are matched by proTxHash. With BLOOM_UPDATE_ALL, matching the
    // registration inserts its hash, so an owner-key watcher follows the lifecycle automatically.
    const uint256 protx_hash = reg_tx.GetHash();

    CProDisTx dis;
    dis.proTxHash = protx_hash;
    dis.actorIndex = 0;
    dis.vchSigs = DummyJoinSigs(1);
    CMutableTransaction dis_mtx;
    dis_mtx.nVersion = 3;
    dis_mtx.nType = TRANSACTION_PROVIDER_DISSOLVE;
    dis_mtx.vin.emplace_back(COutPoint(protx_hash, 0));
    dis_mtx.vout.emplace_back(400 * COIN, proTx.shares[1].scriptRefund);
    SetTxPayload(dis_mtx, dis);
    const CTransaction dis_tx(dis_mtx);

    {
        CBloomFilter filter(20, 0.001, 0, BLOOM_UPDATE_ALL);
        filter.insert(hash_vec(proTx.shares[0].keyIDOwner));
        BOOST_CHECK(!filter.contains(protx_hash));
        BOOST_CHECK(filter.IsRelevantAndUpdate(reg_tx));
        BOOST_CHECK(filter.contains(protx_hash));
        BOOST_CHECK(filter.IsRelevantAndUpdate(dis_tx));
    }
    BOOST_CHECK(matches(dis_tx, hash_vec(protx_hash)));
    // A dissolution's refund payments are ordinary outputs, matched by the base output loop
    BOOST_CHECK(matches(dis_tx, hash_vec(refund_keys[1].GetPubKey().GetID())));
    BOOST_CHECK(!matches(dis_tx, hash_vec(GetRandHash())));

    CKey new_reward_key;
    CProUpShareTx upshare;
    upshare.proTxHash = protx_hash;
    upshare.shareIndex = 0;
    upshare.scriptReward = NewP2PKHScript(new_reward_key);
    CMutableTransaction upshare_mtx;
    upshare_mtx.nVersion = 3;
    upshare_mtx.nType = TRANSACTION_PROVIDER_UPDATE_SHARE;
    upshare_mtx.vin.emplace_back(COutPoint(GetRandHash(), 0));
    SetTxPayload(upshare_mtx, upshare);
    const CTransaction upshare_tx(upshare_mtx);

    BOOST_CHECK(matches(upshare_tx, hash_vec(protx_hash)));
    BOOST_CHECK(matches(upshare_tx, hash_vec(new_reward_key.GetPubKey().GetID())));
    BOOST_CHECK(!matches(upshare_tx, hash_vec(GetRandHash())));
    {
        // A reward-script match inserts the proTxHash so the watcher keeps following the MN
        CBloomFilter filter(20, 0.001, 0, BLOOM_UPDATE_ALL);
        filter.insert(hash_vec(new_reward_key.GetPubKey().GetID()));
        BOOST_CHECK(!filter.contains(protx_hash));
        BOOST_CHECK(filter.IsRelevantAndUpdate(upshare_tx));
        BOOST_CHECK(filter.contains(protx_hash));
    }

    CProUpSharedRegTx upreg;
    upreg.proTxHash = protx_hash;
    upreg.pubKeyOperator.Set(CBLSPublicKey(), /*specificLegacyScheme=*/false);
    upreg.keyIDVoting = new_voting_key.GetPubKey().GetID();
    upreg.vchSigs = DummyJoinSigs(2);
    CMutableTransaction upreg_mtx;
    upreg_mtx.nVersion = 3;
    upreg_mtx.nType = TRANSACTION_PROVIDER_UPDATE_SHARED_REGISTRAR;
    upreg_mtx.vin.emplace_back(COutPoint(GetRandHash(), 0));
    SetTxPayload(upreg_mtx, upreg);
    const CTransaction upreg_tx(upreg_mtx);

    BOOST_CHECK(matches(upreg_tx, hash_vec(protx_hash)));
    BOOST_CHECK(matches(upreg_tx, hash_vec(upreg.keyIDVoting)));
    BOOST_CHECK(!matches(upreg_tx, hash_vec(GetRandHash())));

    // Compact filters must extract the same fields (see the sync note in specialtx_filter.cpp)
    const auto extract = [](const CTransaction& tx) {
        std::vector<std::vector<unsigned char>> ret;
        ExtractSpecialTxFilterElements(tx, [&](Span<const unsigned char> e) { ret.emplace_back(e.begin(), e.end()); });
        return ret;
    };
    const auto has = [](const std::vector<std::vector<unsigned char>>& elements, const std::vector<unsigned char>& v) {
        return std::find(elements.begin(), elements.end(), v) != elements.end();
    };

    {
        const auto elements = extract(reg_tx);
        for (size_t i = 0; i < proTx.shares.size(); i++) {
            BOOST_CHECK(has(elements, script_vec(proTx.shares[i].scriptRefund)));
            BOOST_CHECK(has(elements, script_vec(proTx.shares[i].RewardScript())));
            BOOST_CHECK(has(elements, hash_vec(proTx.shares[i].keyIDOwner)));
        }
    }
    {
        const auto elements = extract(dis_tx);
        BOOST_CHECK(has(elements, hash_vec(protx_hash)));
    }
    {
        const auto elements = extract(upshare_tx);
        BOOST_CHECK(has(elements, hash_vec(protx_hash)));
        BOOST_CHECK(has(elements, script_vec(upshare.scriptReward)));
    }
    {
        const auto elements = extract(upreg_tx);
        BOOST_CHECK(has(elements, hash_vec(protx_hash)));
        BOOST_CHECK(has(elements, hash_vec(upreg.keyIDVoting)));
    }

    // CMerkleBlock gates bloom matching through its special-tx-type allowlist, so cover the whole
    // path a BIP37 client actually takes: a block containing the full lifecycle must match a
    // filter that starts from a single share owner key
    {
        CBlock block;
        block.vtx.push_back(MakeTransactionRef(reg_tx));
        block.vtx.push_back(MakeTransactionRef(dis_tx));
        block.vtx.push_back(MakeTransactionRef(upshare_tx));
        block.vtx.push_back(MakeTransactionRef(upreg_tx));

        CBloomFilter filter(20, 0.001, 0, BLOOM_UPDATE_ALL);
        filter.insert(hash_vec(proTx.shares[0].keyIDOwner));
        CMerkleBlock merkle_block(block, filter);
        BOOST_CHECK_EQUAL(merkle_block.vMatchedTxn.size(), block.vtx.size());
    }
}

BOOST_AUTO_TEST_SUITE_END()
