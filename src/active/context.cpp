// Copyright (c) 2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <active/context.h>

#include <instantsend/instantsend.h>
#include <instantsend/signing.h>
#include <llmq/context.h>

ActiveContext::ActiveContext(CChainState& chainstate, LLMQContext& llmq_ctx, CSporkManager& sporkman,
                             CTxMemPool& mempool, const CMasternodeSync& mn_sync) :
    m_llmq_ctx{llmq_ctx},
    is_signer{std::make_unique<instantsend::InstantSendSigner>(chainstate, *llmq_ctx.clhandler, *llmq_ctx.isman,
                                                               *llmq_ctx.sigman, *llmq_ctx.shareman, *llmq_ctx.qman,
                                                               sporkman, mempool, mn_sync)}
{
    m_llmq_ctx.isman->ConnectSigner(is_signer.get());
}

ActiveContext::~ActiveContext()
{
    m_llmq_ctx.isman->DisconnectSigner();
}
