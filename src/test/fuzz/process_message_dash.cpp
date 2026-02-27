// Copyright (c) 2026-present The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <consensus/consensus.h>
#include <net.h>
#include <protocol.h>
#include <script/script.h>
#include <sync.h>
#include <test/fuzz/FuzzedDataProvider.h>
#include <test/fuzz/fuzz.h>
#include <test/fuzz/util.h>
#include <test/util/mining.h>
#include <test/util/net.h>
#include <test/util/setup_common.h>
#include <test/util/validation.h>
#include <validationinterface.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <memory>
#include <string_view>

namespace {
const TestingSetup* g_setup;
std::string_view LIMIT_TO_MESSAGE_TYPE{};

const std::array<const char*, 41> DASH_MESSAGE_TYPES{
    NetMsgType::SPORK,
    NetMsgType::GETSPORKS,
    NetMsgType::DSACCEPT,
    NetMsgType::DSVIN,
    NetMsgType::DSFINALTX,
    NetMsgType::DSSIGNFINALTX,
    NetMsgType::DSCOMPLETE,
    NetMsgType::DSSTATUSUPDATE,
    NetMsgType::DSTX,
    NetMsgType::DSQUEUE,
    NetMsgType::SENDDSQUEUE,
    NetMsgType::SYNCSTATUSCOUNT,
    NetMsgType::MNGOVERNANCESYNC,
    NetMsgType::MNGOVERNANCEOBJECT,
    NetMsgType::MNGOVERNANCEOBJECTVOTE,
    NetMsgType::GETMNLISTDIFF,
    NetMsgType::MNLISTDIFF,
    NetMsgType::QSENDRECSIGS,
    NetMsgType::QFCOMMITMENT,
    NetMsgType::QCONTRIB,
    NetMsgType::QCOMPLAINT,
    NetMsgType::QJUSTIFICATION,
    NetMsgType::QPCOMMITMENT,
    NetMsgType::QWATCH,
    NetMsgType::QSIGSESANN,
    NetMsgType::QSIGSHARESINV,
    NetMsgType::QGETSIGSHARES,
    NetMsgType::QBSIGSHARES,
    NetMsgType::QSIGREC,
    NetMsgType::QSIGSHARE,
    NetMsgType::QGETDATA,
    NetMsgType::QDATA,
    NetMsgType::CLSIG,
    NetMsgType::ISDLOCK,
    NetMsgType::MNAUTH,
    NetMsgType::GETHEADERS2,
    NetMsgType::SENDHEADERS2,
    NetMsgType::HEADERS2,
    NetMsgType::GETQUORUMROTATIONINFO,
    NetMsgType::QUORUMROTATIONINFO,
    NetMsgType::PLATFORMBAN,
};
} // namespace

void initialize_process_message_dash()
{
    if (const auto val{std::getenv("LIMIT_TO_MESSAGE_TYPE")}) {
        LIMIT_TO_MESSAGE_TYPE = val;
        Assert(std::count(DASH_MESSAGE_TYPES.begin(), DASH_MESSAGE_TYPES.end(), LIMIT_TO_MESSAGE_TYPE)); // Unknown message type passed
    }

    static const auto testing_setup = MakeNoLogFileContext<const TestingSetup>(
        /*chain_name=*/CBaseChainParams::REGTEST,
        /*extra_args=*/{"-txreconciliation"});
    g_setup = testing_setup.get();
    for (int i = 0; i < 2 * COINBASE_MATURITY; i++) {
        MineBlock(g_setup->m_node, CScript() << OP_TRUE);
    }
    SyncWithValidationInterfaceQueue();
}

FUZZ_TARGET(process_message_dash, .init = initialize_process_message_dash)
{
    FuzzedDataProvider fuzzed_data_provider(buffer.data(), buffer.size());

    ConnmanTestMsg& connman = *static_cast<ConnmanTestMsg*>(g_setup->m_node.connman.get());
    TestChainState& chainstate = *static_cast<TestChainState*>(&g_setup->m_node.chainman->ActiveChainstate());
    SetMockTime(1610000000); // any time to successfully reset ibd
    chainstate.ResetIbd();

    LOCK(NetEventsInterface::g_msgproc_mutex);

    CNode& p2p_node = *ConsumeNodeAsUniquePtr(fuzzed_data_provider).release();

    connman.AddTestNode(p2p_node);
    FillNode(fuzzed_data_provider, connman, p2p_node);

    const auto mock_time = ConsumeTime(fuzzed_data_provider);
    SetMockTime(mock_time);

    CSerializedNetMsg net_msg;
    net_msg.m_type = fuzzed_data_provider.PickValueInArray(DASH_MESSAGE_TYPES);
    if (!LIMIT_TO_MESSAGE_TYPE.empty() && net_msg.m_type != LIMIT_TO_MESSAGE_TYPE) {
        return;
    }
    net_msg.data = ConsumeRandomLengthByteVector(fuzzed_data_provider, MAX_PROTOCOL_MESSAGE_LENGTH);

    connman.FlushSendBuffer(p2p_node);
    (void)connman.ReceiveMsgFrom(p2p_node, std::move(net_msg));

    bool more_work{true};
    LIMITED_WHILE(more_work, 10000)
    {
        p2p_node.fPauseSend = false;
        try {
            more_work = connman.ProcessMessagesOnce(p2p_node);
        } catch (const std::ios_base::failure&) {
            more_work = false;
        }
        g_setup->m_node.peerman->SendMessages(&p2p_node);
    }
    SyncWithValidationInterfaceQueue();
    g_setup->m_node.connman->StopNodes();
}
