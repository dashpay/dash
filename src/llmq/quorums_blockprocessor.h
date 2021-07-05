// Public Resource ℗ 2021 Đ𝖺𝗌𝗁 𝖣𝖾𝗏𝖾𝗅𝗈𝗉𝖾𝗋𝗌
// Public Resource ℗ 2020 𝖠𝗆𝖾𝗋𝗈 𝖣𝖾𝗏𝖾𝗅𝗈𝗉𝖾𝗋𝗌
// THIS REPOSITORY IS LICENSED UNDER THE AMERO PUBLIC RESOURCE LICENSE.

#ifndef BITCOIN_LLMQ_QUORUMS_BLOCKPROCESSOR_H
#define BITCOIN_LLMQ_QUORUMS_BLOCKPROCESSOR_H

#include <llmq/quorums_commitment.h>
#include <llmq/quorums_utils.h>

#include <consensus/params.h>
#include <primitives/transaction.h>
#include <saltedhasher.h>
#include <sync.h>

#include <map>
#include <unordered_map>
#include <unordered_lru_cache.h>

class CNode;
class CConnman;

namespace llmq
{

class CQuorumBlockProcessor
{
private:
    CEvoDB& evoDb;

    // TODO cleanup
    CCriticalSection minableCommitmentsCs;
    std::map<std::pair<Consensus::LLMQType, uint256>, uint256> minableCommitmentsByQuorum;
    std::map<uint256, CFinalCommitment> minableCommitments;

    std::map<Consensus::LLMQType, unordered_lru_cache<uint256, bool, StaticSaltedHasher>> mapHasMinedCommitmentCache;

public:
    explicit CQuorumBlockProcessor(CEvoDB& _evoDb);

    bool UpgradeDB();

    void ProcessMessage(CNode* pfrom, const std::string& strCommand, CDataStream& vRecv);

    bool ProcessBlock(const CBlock& block, const CBlockIndex* pindex, CValidationState& state, bool fJustCheck);
    bool UndoBlock(const CBlock& block, const CBlockIndex* pindex);

    void AddMinableCommitment(const CFinalCommitment& fqc);
    bool HasMinableCommitment(const uint256& hash);
    bool GetMinableCommitmentByHash(const uint256& commitmentHash, CFinalCommitment& ret);
    bool GetMinableCommitment(Consensus::LLMQType llmqType, int nHeight, CFinalCommitment& ret);
    bool GetMinableCommitmentTx(Consensus::LLMQType llmqType, int nHeight, CTransactionRef& ret);

    bool HasMinedCommitment(Consensus::LLMQType llmqType, const uint256& quorumHash);
    bool GetMinedCommitment(Consensus::LLMQType llmqType, const uint256& quorumHash, CFinalCommitment& ret, uint256& retMinedBlockHash);

    std::vector<const CBlockIndex*> GetMinedCommitmentsUntilBlock(Consensus::LLMQType llmqType, const CBlockIndex* pindex, size_t maxCount);
    std::map<Consensus::LLMQType, std::vector<const CBlockIndex*>> GetMinedAndActiveCommitmentsUntilBlock(const CBlockIndex* pindex);

private:
    static bool GetCommitmentsFromBlock(const CBlock& block, const CBlockIndex* pindex, std::map<Consensus::LLMQType, CFinalCommitment>& ret, CValidationState& state);
    bool ProcessCommitment(int nHeight, const uint256& blockHash, const CFinalCommitment& qc, CValidationState& state, bool fJustCheck);
    static bool IsMiningPhase(Consensus::LLMQType llmqType, int nHeight);
    bool IsCommitmentRequired(Consensus::LLMQType llmqType, int nHeight);
    static uint256 GetQuorumBlockHash(Consensus::LLMQType llmqType, int nHeight);
};

extern CQuorumBlockProcessor* quorumBlockProcessor;

} // namespace llmq

#endif // BITCOIN_LLMQ_QUORUMS_BLOCKPROCESSOR_H
