// Copyright (c) 2018-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <llmq/commitment.h>
#include <llmq/signing.h>
#include <llmq/snapshot.h>

#include <util/check.h>
#include <util/std23.h>

#include <univalue.h>

namespace llmq {
UniValue CFinalCommitment::ToJson() const
{
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("version", nVersion);
    obj.pushKV("llmqType", std23::to_underlying(llmqType));
    obj.pushKV("quorumHash", quorumHash.ToString());
    obj.pushKV("quorumIndex", quorumIndex);
    obj.pushKV("signersCount", CountSigners());
    obj.pushKV("signers", BitsVectorToHexStr(signers));
    obj.pushKV("validMembersCount", CountValidMembers());
    obj.pushKV("validMembers", BitsVectorToHexStr(validMembers));
    obj.pushKV("quorumPublicKey", quorumPublicKey.ToString(GetBLSScheme()));
    obj.pushKV("quorumVvecHash", quorumVvecHash.ToString());
    obj.pushKV("quorumSig", quorumSig.ToString(GetBLSScheme()));
    obj.pushKV("membersSig", membersSig.ToString(GetBLSScheme()));
    return obj;
}

UniValue CFinalCommitmentTxPayload::ToJson() const
{
    UniValue ret(UniValue::VOBJ);
    ret.pushKV("version", nVersion);
    ret.pushKV("height", nHeight);
    ret.pushKV("commitment", commitment.ToJson());
    return ret;
}

UniValue CQuorumRotationInfo::ToJson() const
{
    UniValue obj(UniValue::VOBJ);
    obj.pushKV("extraShare", extraShare);

    obj.pushKV("quorumSnapshotAtHMinusC", cycleHMinusC.m_snap.ToJson());
    obj.pushKV("quorumSnapshotAtHMinus2C", cycleHMinus2C.m_snap.ToJson());
    obj.pushKV("quorumSnapshotAtHMinus3C", cycleHMinus3C.m_snap.ToJson());

    if (extraShare) {
        obj.pushKV("quorumSnapshotAtHMinus4C", CHECK_NONFATAL(cycleHMinus4C)->m_snap.ToJson());
    }

    obj.pushKV("mnListDiffTip", mnListDiffTip.ToJson());
    obj.pushKV("mnListDiffH", mnListDiffH.ToJson());
    obj.pushKV("mnListDiffAtHMinusC", cycleHMinusC.m_diff.ToJson());
    obj.pushKV("mnListDiffAtHMinus2C", cycleHMinus2C.m_diff.ToJson());
    obj.pushKV("mnListDiffAtHMinus3C", cycleHMinus3C.m_diff.ToJson());

    if (extraShare) {
        obj.pushKV("mnListDiffAtHMinus4C", CHECK_NONFATAL(cycleHMinus4C)->m_diff.ToJson());
    }
    UniValue hqclists(UniValue::VARR);
    for (const auto& qc : lastCommitmentPerIndex) {
        hqclists.push_back(qc.ToJson());
    }
    obj.pushKV("lastCommitmentPerIndex", hqclists);

    UniValue snapshotlist(UniValue::VARR);
    for (const auto& snap : quorumSnapshotList) {
        snapshotlist.push_back(snap.ToJson());
    }
    obj.pushKV("quorumSnapshotList", snapshotlist);

    UniValue mnlistdifflist(UniValue::VARR);
    for (const auto& mnlist : mnListDiffList) {
        mnlistdifflist.push_back(mnlist.ToJson());
    }
    obj.pushKV("mnListDiffList", mnlistdifflist);
    return obj;
}

UniValue CQuorumSnapshot::ToJson() const
{
    UniValue obj(UniValue::VOBJ);
    UniValue activeQ(UniValue::VARR);
    for (const bool h : activeQuorumMembers) {
        // cppcheck-suppress useStlAlgorithm
        activeQ.push_back(h);
    }
    obj.pushKV("activeQuorumMembers", activeQ);
    obj.pushKV("mnSkipListMode", std23::to_underlying(mnSkipListMode));
    UniValue skipList(UniValue::VARR);
    for (const auto& h : mnSkipList) {
        // cppcheck-suppress useStlAlgorithm
        skipList.push_back(h);
    }
    obj.pushKV("mnSkipList", skipList);
    return obj;
}

UniValue CRecoveredSig::ToJson() const
{
    UniValue ret(UniValue::VOBJ);
    ret.pushKV("llmqType", std23::to_underlying(llmqType));
    ret.pushKV("quorumHash", quorumHash.ToString());
    ret.pushKV("id", id.ToString());
    ret.pushKV("msgHash", msgHash.ToString());
    ret.pushKV("sig", sig.Get().ToString());
    ret.pushKV("hash", sig.Get().GetHash().ToString());
    return ret;
}
} // namespace llmq
