// Copyright (c) 2009-2010 Satoshi Nakamoto
// Copyright (c) 2009-2021 The Bitcoin Core developers
// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <rest/server.h>

#include <blockfilter.h>
#include <chain.h>
#include <chainparams.h>
#include <chainlock/chainlock.h>
#include <context.h>
#include <core_io.h>
#include <index/blockfilterindex.h>
#include <index/txindex.h>
#include <instantsend/instantsend.h>
#include <llmq/context.h>
#include <logging.h>
#include <node/blockstorage.h>
#include <node/context.h>
#include <primitives/block.h>
#include <primitives/transaction.h>
#include <rpc/blockchain.h>
#include <rpc/mempool.h>
#include <rpc/protocol.h>
#include <rpc/server.h>
#include <rpc/server_util.h>
#include <streams.h>
#include <sync.h>
#include <txmempool.h>
#include <util/check.h>
#include <util/sock.h>
#include <util/system.h>
#include <util/threadnames.h>
#include <util/time.h>
#include <validation.h>
#include <version.h>

#include <drogon/HttpAppFramework.h>
#include <trantor/utils/Logger.h>

#include <univalue.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <thread>

using node::GetTransaction;
using node::NodeContext;
using node::ReadBlockFromDisk;

static const size_t MAX_GETUTXOS_OUTPOINTS = 15; //allow a max of 15 outpoints to be queried at once
static constexpr unsigned int MAX_REST_HEADERS_RESULTS = 2000;

namespace rest {
using Callback = std::function<void(const drogon::HttpResponsePtr&)>;
} // namespace rest

namespace {
/** Maximum time that can be spent probing for REST server activation. */
constexpr auto MAX_PROBE_DURATION{10s};
/** Background thread running the server's event loop. */
std::thread g_server_thread;

drogon::HttpResponsePtr MakeResponse(drogon::HttpStatusCode code, drogon::ContentType ct, std::string body)
{
    auto resp = drogon::HttpResponse::newHttpResponse(code, ct);
    resp->setBody(std::move(body));
    return resp;
}

std::string GetQueryParam(const drogon::HttpRequestPtr& req, const std::string& key, const std::string& default_value)
{
    const std::string& val = req->getParameter(key);
    return val.empty() ? default_value : val;
}

void WriteReply(const rest::Callback& cb, drogon::ContentType ct, std::string body)
{
    cb(MakeResponse(drogon::k200OK, ct, std::move(body)));
}
} // anonymous namespace

static const struct {
    RESTResponseFormat rf;
    const char* name;
} rf_names[] = {
      {RESTResponseFormat::UNDEF, ""},
      {RESTResponseFormat::BINARY, "bin"},
      {RESTResponseFormat::HEX, "hex"},
      {RESTResponseFormat::JSON, "json"},
};

struct CCoin {
    uint32_t nHeight;
    CTxOut out;

    CCoin() : nHeight(0) {}
    explicit CCoin(Coin&& in) : nHeight(in.nHeight), out(std::move(in.out)) {}

    SERIALIZE_METHODS(CCoin, obj)
    {
        uint32_t nTxVerDummy = 0;
        READWRITE(nTxVerDummy, obj.nHeight, obj.out);
    }
};

static bool RESTERR(const rest::Callback& cb, drogon::HttpStatusCode status, std::string message)
{
    cb(MakeResponse(status, drogon::CT_TEXT_PLAIN, message + "\r\n"));
    return false;
}

/**
 * Get the node context.
 *
 * @param[in]  context  The CoreContext to extract NodeContext from.
 * @param[in]  cb       Callback invoked with an error response if NodeContext is not found.
 * @returns             Pointer to NodeContext or nullptr if not found.
 */
static NodeContext* GetNodeContext(const CoreContext& context, const rest::Callback& cb)
{
    auto* node_context = GetContext<NodeContext>(context);
    if (!node_context) {
        RESTERR(cb, drogon::k500InternalServerError,
                strprintf("%s:%d (%s)\n"
                          "Internal bug detected: Node context not found!\n"
                          "You may report this issue here: %s\n",
                          __FILE__, __LINE__, __func__, PACKAGE_BUGREPORT));
        return nullptr;
    }
    return node_context;
}

/**
 * Get the node context mempool.
 *
 * @param[in]  context  The core context to extract the mempool from.
 * @param[in]  cb       Callback invoked with an error response if mempool is not found.
 * @returns             Pointer to the mempool or nullptr if no mempool found.
 */
static CTxMemPool* GetMemPool(const CoreContext& context, const rest::Callback& cb)
{
    auto* node_context = GetContext<NodeContext>(context);
    if (!node_context || !node_context->mempool) {
        RESTERR(cb, drogon::k404NotFound, "Mempool disabled or instance not found");
        return nullptr;
    }
    return node_context->mempool.get();
}

/**
 * Get the node context chainstatemanager.
 *
 * @param[in]  context  The core context to extract ChainstateManager from.
 * @param[in]  cb       Callback invoked with an error response if ChainstateManager is not found.
 * @returns             Pointer to ChainstateManager or nullptr if none found.
 */
static ChainstateManager* GetChainman(const CoreContext& context, const rest::Callback& cb)
{
    auto node_context = GetContext<NodeContext>(context);
    if (!node_context || !node_context->chainman) {
        RESTERR(cb, drogon::k500InternalServerError,
                strprintf("%s:%d (%s)\n"
                          "Internal bug detected: Chainman disabled or instance not found!\n"
                          "You may report this issue here: %s\n",
                          __FILE__, __LINE__, __func__, PACKAGE_BUGREPORT));
        return nullptr;
    }
    return node_context->chainman.get();
}

/**
 * Get the node context LLMQContext.
 *
 * @param[in]  context  The core context to extract LLMQContext from.
 * @param[in]  cb       Callback invoked with an error response if LLMQContext is not found.
 * @returns             Pointer to LLMQContext or nullptr if none found.
 */
static LLMQContext* GetLLMQContext(const CoreContext& context, const rest::Callback& cb)
{
    auto node_context = GetContext<NodeContext>(context);
    if (!node_context || !node_context->llmq_ctx) {
        RESTERR(cb, drogon::k500InternalServerError,
                strprintf("%s:%d (%s)\n"
                          "Internal bug detected: LLMQ context not found!\n"
                          "You may report this issue here: %s\n",
                          __FILE__, __LINE__, __func__, PACKAGE_BUGREPORT));
        return nullptr;
    }
    return node_context->llmq_ctx.get();
}

RESTResponseFormat ParseDataFormat(std::string& param, const std::string& strReq)
{
    // Remove query string (if any, separated with '?') as it should not interfere with
    // parsing param and data format
    param = strReq.substr(0, strReq.rfind('?'));
    const std::string::size_type pos_format{param.rfind('.')};

    // No format string is found
    if (pos_format == std::string::npos) {
        return rf_names[0].rf;
    }

    // Match format string to available formats
    const std::string suffix(param, pos_format + 1);
    for (const auto& rf_name : rf_names) {
        if (suffix == rf_name.name) {
            param.erase(pos_format);
            return rf_name.rf;
        }
    }

    // If no suffix is found, return RESTResponseFormat::UNDEF and original string without query string
    return rf_names[0].rf;
}

static std::string AvailableDataFormatsString()
{
    std::string formats;
    for (const auto& rf_name : rf_names) {
        if (strlen(rf_name.name) > 0) {
            formats.append(".");
            formats.append(rf_name.name);
            formats.append(", ");
        }
    }

    if (formats.length() > 0)
        return formats.substr(0, formats.length() - 2);

    return formats;
}

static bool CheckWarmup(const rest::Callback& cb)
{
    std::string statusmessage;
    if (RPCIsInWarmup(&statusmessage))
         return RESTERR(cb, drogon::k503ServiceUnavailable, "Service temporarily unavailable: " + statusmessage);
    return true;
}

static bool rest_headers(const CoreContext& context,
                         const drogon::HttpRequestPtr& req,
                         const rest::Callback& cb,
                         const std::string& strURIPart)
{
    if (!CheckWarmup(cb))
        return false;
    std::string param;
    const RESTResponseFormat rf = ParseDataFormat(param, strURIPart);
    std::vector<std::string> path = SplitString(param, '/');

    std::string raw_count;
    std::string hashStr;
    if (path.size() == 2) {
        // deprecated path: /rest/headers/<count>/<hash>
        hashStr = path[1];
        raw_count = path[0];
    } else if (path.size() == 1) {
        // new path with query parameter: /rest/headers/<hash>?count=<count>
        hashStr = path[0];
        raw_count = GetQueryParam(req, "count", "5");
    } else {
        return RESTERR(cb, drogon::k400BadRequest, "Invalid URI format. Expected /rest/headers/<hash>.<ext>?count=<count>");
    }

    const auto parsed_count{ToIntegral<size_t>(raw_count)};
    if (!parsed_count.has_value() || *parsed_count < 1 || *parsed_count > MAX_REST_HEADERS_RESULTS) {
        return RESTERR(cb, drogon::k400BadRequest, strprintf("Header count is invalid or out of acceptable range (1-%u): %s", MAX_REST_HEADERS_RESULTS, raw_count));
    }

    uint256 hash;
    if (!ParseHashStr(hashStr, hash))
        return RESTERR(cb, drogon::k400BadRequest, "Invalid hash: " + hashStr);

    const CBlockIndex* tip = nullptr;
    std::vector<const CBlockIndex*> headers;
    headers.reserve(*parsed_count);
    {
        ChainstateManager* maybe_chainman = GetChainman(context, cb);
        if (!maybe_chainman) return false;
        ChainstateManager& chainman = *maybe_chainman;
        LOCK(cs_main);
        CChain& active_chain = chainman.ActiveChain();
        tip = active_chain.Tip();
        const CBlockIndex* pindex = chainman.m_blockman.LookupBlockIndex(hash);
        while (pindex != nullptr && active_chain.Contains(pindex)) {
            headers.push_back(pindex);
            if (headers.size() == *parsed_count) {
                break;
            }
            pindex = active_chain.Next(pindex);
        }
    }

    switch (rf) {
    case RESTResponseFormat::BINARY: {
        CDataStream ssHeader(SER_NETWORK, PROTOCOL_VERSION);
        for (const CBlockIndex *pindex : headers) {
            ssHeader << pindex->GetBlockHeader();
        }

        WriteReply(cb, drogon::CT_APPLICATION_OCTET_STREAM, ssHeader.str());
        return true;
    }

    case RESTResponseFormat::HEX: {
        CDataStream ssHeader(SER_NETWORK, PROTOCOL_VERSION);
        for (const CBlockIndex *pindex : headers) {
            ssHeader << pindex->GetBlockHeader();
        }

        WriteReply(cb, drogon::CT_TEXT_PLAIN, HexStr(ssHeader) + "\n");
        return true;
    }
    case RESTResponseFormat::JSON: {
        const NodeContext* const node = GetNodeContext(context, cb);
        if (!node || !node->chainlocks) return false;

        UniValue jsonHeaders(UniValue::VARR);
        for (const CBlockIndex *pindex : headers) {
            jsonHeaders.push_back(blockheaderToJSON(tip, pindex, *node->chainlocks));
        }
        WriteReply(cb, drogon::CT_APPLICATION_JSON, jsonHeaders.write() + "\n");
        return true;
    }
    default: {
        return RESTERR(cb, drogon::k406NotAcceptable, "output format not found (available: " + AvailableDataFormatsString() + ")");
    }
    }
}

static bool rest_block(const CoreContext& context,
                       const drogon::HttpRequestPtr& req,
                       const rest::Callback& cb,
                       const std::string& strURIPart,
                       TxVerbosity tx_verbosity)
{
    if (!CheckWarmup(cb))
        return false;
    std::string hashStr;
    const RESTResponseFormat rf = ParseDataFormat(hashStr, strURIPart);

    uint256 hash;
    if (!ParseHashStr(hashStr, hash))
        return RESTERR(cb, drogon::k400BadRequest, "Invalid hash: " + hashStr);

    CBlock block;
    const CBlockIndex* pblockindex = nullptr;
    const CBlockIndex* tip = nullptr;
    ChainstateManager* maybe_chainman = GetChainman(context, cb);
    if (!maybe_chainman) return false;
    ChainstateManager& chainman = *maybe_chainman;
    {
        LOCK(cs_main);
        tip = chainman.ActiveChain().Tip();
        pblockindex = chainman.m_blockman.LookupBlockIndex(hash);
        if (!pblockindex) {
            return RESTERR(cb, drogon::k404NotFound, hashStr + " not found");
        }

        if (chainman.m_blockman.IsBlockPruned(pblockindex))
            return RESTERR(cb, drogon::k404NotFound, hashStr + " not available (pruned data)");
    }

    if (!ReadBlockFromDisk(block, pblockindex, Params().GetConsensus())) {
        return RESTERR(cb, drogon::k404NotFound, hashStr + " not found");
    }

    switch (rf) {
    case RESTResponseFormat::BINARY: {
        CDataStream ssBlock(SER_NETWORK, PROTOCOL_VERSION);
        ssBlock << block;
        WriteReply(cb, drogon::CT_APPLICATION_OCTET_STREAM, ssBlock.str());
        return true;
    }

    case RESTResponseFormat::HEX: {
        CDataStream ssBlock(SER_NETWORK, PROTOCOL_VERSION);
        ssBlock << block;
        WriteReply(cb, drogon::CT_TEXT_PLAIN, HexStr(ssBlock) + "\n");
        return true;
    }

    case RESTResponseFormat::JSON: {
        const NodeContext* const node = GetNodeContext(context, cb);
        if (!node || !node->chainlocks) return false;

        const LLMQContext* llmq_ctx = GetLLMQContext(context, cb);
        if (!llmq_ctx) return false;

        UniValue objBlock = blockToJSON(chainman.m_blockman, block, tip, pblockindex, *node->chainlocks, *llmq_ctx->isman, tx_verbosity);
        WriteReply(cb, drogon::CT_APPLICATION_JSON, objBlock.write() + "\n");
        return true;
    }

    default: {
        return RESTERR(cb, drogon::k406NotAcceptable, "output format not found (available: " + AvailableDataFormatsString() + ")");
    }
    }
}

static bool rest_block_extended(const CoreContext& context, const drogon::HttpRequestPtr& req, const rest::Callback& cb, const std::string& strURIPart)
{
    return rest_block(context, req, cb, strURIPart, TxVerbosity::SHOW_DETAILS_AND_PREVOUT);
}

static bool rest_block_notxdetails(const CoreContext& context, const drogon::HttpRequestPtr& req, const rest::Callback& cb, const std::string& strURIPart)
{
    return rest_block(context, req, cb, strURIPart, TxVerbosity::SHOW_TXID);
}

static bool rest_filter_header(const CoreContext& context, const drogon::HttpRequestPtr& req, const rest::Callback& cb, const std::string& strURIPart)
{
    if (!CheckWarmup(cb)) return false;

    std::string param;
    const RESTResponseFormat rf = ParseDataFormat(param, strURIPart);

    std::vector<std::string> uri_parts = SplitString(param, '/');
    std::string raw_count;
    std::string raw_blockhash;
    if (uri_parts.size() == 3) {
        // deprecated path: /rest/blockfilterheaders/<filtertype>/<count>/<blockhash>
        raw_blockhash = uri_parts[2];
        raw_count = uri_parts[1];
    } else if (uri_parts.size() == 2) {
        // new path with query parameter: /rest/blockfilterheaders/<filtertype>/<blockhash>?count=<count>
        raw_blockhash = uri_parts[1];
        raw_count = GetQueryParam(req, "count", "5");
    } else {
        return RESTERR(cb, drogon::k400BadRequest, "Invalid URI format. Expected /rest/blockfilterheaders/<filtertype>/<blockhash>.<ext>?count=<count>");
    }

    const auto parsed_count{ToIntegral<size_t>(raw_count)};
    if (!parsed_count.has_value() || *parsed_count < 1 || *parsed_count > MAX_REST_HEADERS_RESULTS) {
        return RESTERR(cb, drogon::k400BadRequest, strprintf("Header count is invalid or out of acceptable range (1-%u): %s", MAX_REST_HEADERS_RESULTS, raw_count));
    }

    uint256 block_hash;
    if (!ParseHashStr(raw_blockhash, block_hash)) {
        return RESTERR(cb, drogon::k400BadRequest, "Invalid hash: " + raw_blockhash);
    }

    BlockFilterType filtertype;
    if (!BlockFilterTypeByName(uri_parts[0], filtertype)) {
        return RESTERR(cb, drogon::k400BadRequest, "Unknown filtertype " + uri_parts[0]);
    }

    BlockFilterIndex* index = GetBlockFilterIndex(filtertype);
    if (!index) {
        return RESTERR(cb, drogon::k400BadRequest, "Index is not enabled for filtertype " + uri_parts[0]);
    }

    std::vector<const CBlockIndex*> headers;
    headers.reserve(*parsed_count);
    {
        ChainstateManager* maybe_chainman = GetChainman(context, cb);
        if (!maybe_chainman) return false;
        ChainstateManager& chainman = *maybe_chainman;
        LOCK(cs_main);
        CChain& active_chain = chainman.ActiveChain();
        const CBlockIndex* pindex = chainman.m_blockman.LookupBlockIndex(block_hash);
        while (pindex != nullptr && active_chain.Contains(pindex)) {
            headers.push_back(pindex);
            if (headers.size() == *parsed_count)
                break;
            pindex = active_chain.Next(pindex);
        }
    }

    bool index_ready = index->BlockUntilSyncedToCurrentChain();

    std::vector<uint256> filter_headers;
    filter_headers.reserve(*parsed_count);
    for (const CBlockIndex* pindex : headers) {
        uint256 filter_header;
        if (!index->LookupFilterHeader(pindex, filter_header)) {
            std::string errmsg = "Filter not found.";

            if (!index_ready) {
                errmsg += " Block filters are still in the process of being indexed.";
            } else {
                errmsg += " This error is unexpected and indicates index corruption.";
            }

            return RESTERR(cb, drogon::k404NotFound, errmsg);
        }
        filter_headers.push_back(filter_header);
    }

    switch (rf) {
    case RESTResponseFormat::BINARY: {
        CDataStream ssHeader{SER_NETWORK, PROTOCOL_VERSION};
        for (const uint256& header : filter_headers) {
            ssHeader << header;
        }

        WriteReply(cb, drogon::CT_APPLICATION_OCTET_STREAM, ssHeader.str());
        return true;
    }
    case RESTResponseFormat::HEX: {
        CDataStream ssHeader{SER_NETWORK, PROTOCOL_VERSION};
        for (const uint256& header : filter_headers) {
            ssHeader << header;
        }

        WriteReply(cb, drogon::CT_TEXT_PLAIN, HexStr(ssHeader) + "\n");
        return true;
    }
    case RESTResponseFormat::JSON: {
        UniValue jsonHeaders(UniValue::VARR);
        for (const uint256& header : filter_headers) {
            jsonHeaders.push_back(header.GetHex());
        }

        WriteReply(cb, drogon::CT_APPLICATION_JSON, jsonHeaders.write() + "\n");
        return true;
    }
    default: {
        return RESTERR(cb, drogon::k406NotAcceptable, "output format not found (available: " + AvailableDataFormatsString() + ")");
    }
    }
}

static bool rest_block_filter(const CoreContext& context, const drogon::HttpRequestPtr& req, const rest::Callback& cb, const std::string& strURIPart)
{
    if (!CheckWarmup(cb)) return false;

    std::string param;
    const RESTResponseFormat rf = ParseDataFormat(param, strURIPart);

    // request is sent over URI scheme /rest/blockfilter/filtertype/blockhash
    std::vector<std::string> uri_parts = SplitString(param, '/');
    if (uri_parts.size() != 2) {
        return RESTERR(cb, drogon::k400BadRequest, "Invalid URI format. Expected /rest/blockfilter/<filtertype>/<blockhash>");
    }

    uint256 block_hash;
    if (!ParseHashStr(uri_parts[1], block_hash)) {
        return RESTERR(cb, drogon::k400BadRequest, "Invalid hash: " + uri_parts[1]);
    }

    BlockFilterType filtertype;
    if (!BlockFilterTypeByName(uri_parts[0], filtertype)) {
        return RESTERR(cb, drogon::k400BadRequest, "Unknown filtertype " + uri_parts[0]);
    }

    BlockFilterIndex* index = GetBlockFilterIndex(filtertype);
    if (!index) {
        return RESTERR(cb, drogon::k400BadRequest, "Index is not enabled for filtertype " + uri_parts[0]);
    }

    const CBlockIndex* block_index;
    bool block_was_connected;
    {
        ChainstateManager* maybe_chainman = GetChainman(context, cb);
        if (!maybe_chainman) return false;
        ChainstateManager& chainman = *maybe_chainman;
        LOCK(cs_main);
        block_index = chainman.m_blockman.LookupBlockIndex(block_hash);
        if (!block_index) {
            return RESTERR(cb, drogon::k404NotFound, uri_parts[1] + " not found");
        }
        block_was_connected = block_index->IsValid(BLOCK_VALID_SCRIPTS);
    }

    bool index_ready = index->BlockUntilSyncedToCurrentChain();

    BlockFilter filter;
    if (!index->LookupFilter(block_index, filter)) {
        std::string errmsg = "Filter not found.";

        if (!block_was_connected) {
            errmsg += " Block was not connected to active chain.";
        } else if (!index_ready) {
            errmsg += " Block filters are still in the process of being indexed.";
        } else {
            errmsg += " This error is unexpected and indicates index corruption.";
        }

        return RESTERR(cb, drogon::k404NotFound, errmsg);
    }

    switch (rf) {
    case RESTResponseFormat::BINARY: {
        CDataStream ssResp{SER_NETWORK, PROTOCOL_VERSION};
        ssResp << filter;

        WriteReply(cb, drogon::CT_APPLICATION_OCTET_STREAM, ssResp.str());
        return true;
    }
    case RESTResponseFormat::HEX: {
        CDataStream ssResp{SER_NETWORK, PROTOCOL_VERSION};
        ssResp << filter;

        WriteReply(cb, drogon::CT_TEXT_PLAIN, HexStr(ssResp) + "\n");
        return true;
    }
    case RESTResponseFormat::JSON: {
        UniValue ret(UniValue::VOBJ);
        ret.pushKV("filter", HexStr(filter.GetEncodedFilter()));
        WriteReply(cb, drogon::CT_APPLICATION_JSON, ret.write() + "\n");
        return true;
    }
    default: {
        return RESTERR(cb, drogon::k406NotAcceptable, "output format not found (available: " + AvailableDataFormatsString() + ")");
    }
    }
}

// A bit of a hack - dependency on a function defined in rpc/blockchain.cpp
RPCHelpMan getblockchaininfo();

static bool rest_chaininfo(const CoreContext& context, const drogon::HttpRequestPtr& req, const rest::Callback& cb, const std::string& strURIPart)
{
    if (!CheckWarmup(cb))
        return false;
    std::string param;
    const RESTResponseFormat rf = ParseDataFormat(param, strURIPart);

    switch (rf) {
    case RESTResponseFormat::JSON: {
        JSONRPCRequest jsonRequest;
        jsonRequest.context = context;
        jsonRequest.params = UniValue(UniValue::VARR);
        UniValue chainInfoObject = getblockchaininfo().HandleRequest(jsonRequest);
        WriteReply(cb, drogon::CT_APPLICATION_JSON, chainInfoObject.write() + "\n");
        return true;
    }
    default: {
        return RESTERR(cb, drogon::k406NotAcceptable, "output format not found (available: json)");
    }
    }
}

static bool rest_mempool(const CoreContext& context, const drogon::HttpRequestPtr& req, const rest::Callback& cb, const std::string& str_uri_part)
{
    if (!CheckWarmup(cb))
        return false;

    std::string param;
    const RESTResponseFormat rf = ParseDataFormat(param, str_uri_part);
    if (param != "contents" && param != "info") {
        return RESTERR(cb, drogon::k400BadRequest, "Invalid URI format. Expected /rest/mempool/<info|contents>.json");
    }

    const CTxMemPool* mempool = GetMemPool(context, cb);
    if (!mempool) return false;

    switch (rf) {
    case RESTResponseFormat::JSON: {
        const LLMQContext* llmq_ctx = GetLLMQContext(context, cb);
        if (!llmq_ctx) return false;

        std::string str_json;
        if (param == "contents") {
            str_json = MempoolToJSON(*mempool, llmq_ctx->isman.get(), true).write() + "\n";
        } else {
            str_json = MempoolInfoToJSON(*mempool, *llmq_ctx->isman).write() + "\n";
        }

        WriteReply(cb, drogon::CT_APPLICATION_JSON, str_json);
        return true;
    }
    default: {
        return RESTERR(cb, drogon::k406NotAcceptable, "output format not found (available: json)");
    }
    }
}

static bool rest_tx(const CoreContext& context, const drogon::HttpRequestPtr& req, const rest::Callback& cb, const std::string& strURIPart)
{
    if (!CheckWarmup(cb))
        return false;
    std::string hashStr;
    const RESTResponseFormat rf = ParseDataFormat(hashStr, strURIPart);

    uint256 hash;
    if (!ParseHashStr(hashStr, hash))
        return RESTERR(cb, drogon::k400BadRequest, "Invalid hash: " + hashStr);

    if (g_txindex) {
        g_txindex->BlockUntilSyncedToCurrentChain();
    }

    const NodeContext* const node = GetNodeContext(context, cb);
    if (!node) return false;
    uint256 hashBlock = uint256();
    const CTransactionRef tx = GetTransaction(/*block_index=*/nullptr, node->mempool.get(), hash, Params().GetConsensus(), hashBlock);
    if (!tx) {
        return RESTERR(cb, drogon::k404NotFound, hashStr + " not found");
    }

    switch (rf) {
    case RESTResponseFormat::BINARY: {
        CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
        ssTx << tx;

        WriteReply(cb, drogon::CT_APPLICATION_OCTET_STREAM, ssTx.str());
        return true;
    }

    case RESTResponseFormat::HEX: {
        CDataStream ssTx(SER_NETWORK, PROTOCOL_VERSION);
        ssTx << tx;

        WriteReply(cb, drogon::CT_TEXT_PLAIN, HexStr(ssTx) + "\n");
        return true;
    }

    case RESTResponseFormat::JSON: {
        UniValue objTx(UniValue::VOBJ);
        TxToUniv(*tx, /*block_hash=*/hashBlock, /*entry=*/objTx);
        WriteReply(cb, drogon::CT_APPLICATION_JSON, objTx.write() + "\n");
        return true;
    }

    default: {
        return RESTERR(cb, drogon::k406NotAcceptable, "output format not found (available: " + AvailableDataFormatsString() + ")");
    }
    }
}

static bool rest_getutxos(const CoreContext& context, const drogon::HttpRequestPtr& req, const rest::Callback& cb, const std::string& strURIPart)
{
    if (!CheckWarmup(cb))
        return false;
    std::string param;
    const RESTResponseFormat rf = ParseDataFormat(param, strURIPart);

    std::vector<std::string> uriParts;
    if (param.length() > 1)
    {
        std::string strUriParams = param.substr(1);
        uriParts = SplitString(strUriParams, '/');
    }

    // throw exception in case of an empty request
    std::string strRequestMutable = std::string(req->body());
    if (strRequestMutable.length() == 0 && uriParts.size() == 0)
        return RESTERR(cb, drogon::k400BadRequest, "Error: empty request");

    bool fInputParsed = false;
    bool fCheckMemPool = false;
    std::vector<COutPoint> vOutPoints;

    // parse/deserialize input
    // input-format = output-format, rest/getutxos/bin requires binary input, gives binary output, ...

    if (uriParts.size() > 0)
    {
        //inputs is sent over URI scheme (/rest/getutxos/checkmempool/txid1-n/txid2-n/...)
        if (uriParts[0] == "checkmempool") fCheckMemPool = true;

        for (size_t i = (fCheckMemPool) ? 1 : 0; i < uriParts.size(); i++)
        {
            uint256 txid;
            int32_t nOutput;
            std::string strTxid = uriParts[i].substr(0, uriParts[i].find('-'));
            std::string strOutput = uriParts[i].substr(uriParts[i].find('-')+1);

            if (!ParseInt32(strOutput, &nOutput) || !IsHex(strTxid))
                return RESTERR(cb, drogon::k400BadRequest, "Parse error");

            txid.SetHex(strTxid);
            vOutPoints.push_back(COutPoint(txid, (uint32_t)nOutput));
        }

        if (vOutPoints.size() > 0)
            fInputParsed = true;
        else
            return RESTERR(cb, drogon::k400BadRequest, "Error: empty request");
    }

    switch (rf) {
    case RESTResponseFormat::HEX: {
        // convert hex to bin, continue then with bin part
        std::vector<unsigned char> strRequestV = ParseHex(strRequestMutable);
        strRequestMutable.assign(strRequestV.begin(), strRequestV.end());
        [[fallthrough]];
    }

    case RESTResponseFormat::BINARY: {
        try {
            //deserialize only if user sent a request
            if (strRequestMutable.size() > 0)
            {
                if (fInputParsed) //don't allow sending input over URI and HTTP RAW DATA
                    return RESTERR(cb, drogon::k400BadRequest, "Combination of URI scheme inputs and raw post data is not allowed");

                CDataStream oss(SER_NETWORK, PROTOCOL_VERSION);
                oss << strRequestMutable;
                oss >> fCheckMemPool;
                oss >> vOutPoints;
            }
        } catch (const std::ios_base::failure&) {
            // abort in case of unreadable binary data
            return RESTERR(cb, drogon::k400BadRequest, "Parse error");
        }
        break;
    }

    case RESTResponseFormat::JSON: {
        if (!fInputParsed)
            return RESTERR(cb, drogon::k400BadRequest, "Error: empty request");
        break;
    }
    default: {
        return RESTERR(cb, drogon::k406NotAcceptable, "output format not found (available: " + AvailableDataFormatsString() + ")");
    }
    }

    // limit max outpoints
    if (vOutPoints.size() > MAX_GETUTXOS_OUTPOINTS)
        return RESTERR(cb, drogon::k400BadRequest, strprintf("Error: max outpoints exceeded (max: %d, tried: %d)", MAX_GETUTXOS_OUTPOINTS, vOutPoints.size()));

    // check spentness and form a bitmap (as well as a JSON capable human-readable string representation)
    std::vector<unsigned char> bitmap;
    std::vector<CCoin> outs;
    std::string bitmapStringRepresentation;
    std::vector<bool> hits;
    bitmap.resize((vOutPoints.size() + 7) / 8);
    ChainstateManager* maybe_chainman = GetChainman(context, cb);
    if (!maybe_chainman) return false;
    ChainstateManager& chainman = *maybe_chainman;
    {
        auto process_utxos = [&vOutPoints, &outs, &hits](const CCoinsView& view, const CTxMemPool* mempool) {
            for (const COutPoint& vOutPoint : vOutPoints) {
                Coin coin;
                bool hit = (!mempool || !mempool->isSpent(vOutPoint)) && view.GetCoin(vOutPoint, coin);
                hits.push_back(hit);
                if (hit) outs.emplace_back(std::move(coin));
            }
        };

        if (fCheckMemPool) {
            const CTxMemPool* mempool = GetMemPool(context, cb);
            if (!mempool) return false;
            // use db+mempool as cache backend in case user likes to query mempool
            LOCK2(cs_main, mempool->cs);
            CCoinsViewCache& viewChain = chainman.ActiveChainstate().CoinsTip();
            CCoinsViewMemPool viewMempool(&viewChain, *mempool);
            process_utxos(viewMempool, mempool);
        } else {
            LOCK(cs_main);
            process_utxos(chainman.ActiveChainstate().CoinsTip(), nullptr);
        }

        for (size_t i = 0; i < hits.size(); ++i) {
            const bool hit = hits[i];
            bitmapStringRepresentation.append(hit ? "1" : "0"); // form a binary string representation (human-readable for json output)
            bitmap[i / 8] |= ((uint8_t)hit) << (i % 8);
        }
    }

    switch (rf) {
    case RESTResponseFormat::BINARY: {
        // serialize data
        // use exact same output as mentioned in Bip64
        CDataStream ssGetUTXOResponse(SER_NETWORK, PROTOCOL_VERSION);
        ssGetUTXOResponse << chainman.ActiveChain().Height() << chainman.ActiveChain().Tip()->GetBlockHash() << bitmap << outs;

        WriteReply(cb, drogon::CT_APPLICATION_OCTET_STREAM, ssGetUTXOResponse.str());
        return true;
    }

    case RESTResponseFormat::HEX: {
        CDataStream ssGetUTXOResponse(SER_NETWORK, PROTOCOL_VERSION);
        ssGetUTXOResponse << chainman.ActiveChain().Height() << chainman.ActiveChain().Tip()->GetBlockHash() << bitmap << outs;

        WriteReply(cb, drogon::CT_TEXT_PLAIN, HexStr(ssGetUTXOResponse) + "\n");
        return true;
    }

    case RESTResponseFormat::JSON: {
        UniValue objGetUTXOResponse(UniValue::VOBJ);

        // pack in some essentials
        // use more or less the same output as mentioned in Bip64
        objGetUTXOResponse.pushKV("chainHeight", chainman.ActiveChain().Height());
        objGetUTXOResponse.pushKV("chaintipHash", chainman.ActiveChain().Tip()->GetBlockHash().GetHex());
        objGetUTXOResponse.pushKV("bitmap", bitmapStringRepresentation);

        UniValue utxos(UniValue::VARR);
        for (const CCoin& coin : outs) {
            UniValue utxo(UniValue::VOBJ);
            utxo.pushKV("height", (int32_t)coin.nHeight);
            utxo.pushKV("value", ValueFromAmount(coin.out.nValue));

            // include the script in a json output
            UniValue o(UniValue::VOBJ);
            ScriptToUniv(coin.out.scriptPubKey, /*out=*/o, /*include_hex=*/true, /*include_address=*/true);
            utxo.pushKV("scriptPubKey", o);
            utxos.push_back(utxo);
        }
        objGetUTXOResponse.pushKV("utxos", utxos);

        WriteReply(cb, drogon::CT_APPLICATION_JSON, objGetUTXOResponse.write() + "\n");
        return true;
    }
    default: {
        return RESTERR(cb, drogon::k406NotAcceptable, "output format not found (available: " + AvailableDataFormatsString() + ")");
    }
    }
}

static bool rest_blockhash_by_height(const CoreContext& context, const drogon::HttpRequestPtr& req,
                                     const rest::Callback& cb, const std::string& str_uri_part)
{
    if (!CheckWarmup(cb)) return false;
    std::string height_str;
    const RESTResponseFormat rf = ParseDataFormat(height_str, str_uri_part);

    int32_t blockheight = -1; // Initialization done only to prevent valgrind false positive, see https://github.com/bitcoin/bitcoin/pull/18785
    if (!ParseInt32(height_str, &blockheight) || blockheight < 0) {
        return RESTERR(cb, drogon::k400BadRequest, "Invalid height: " + SanitizeString(height_str));
    }

    CBlockIndex* pblockindex = nullptr;
    {
        ChainstateManager* maybe_chainman = GetChainman(context, cb);
        if (!maybe_chainman) return false;
        ChainstateManager& chainman = *maybe_chainman;
        LOCK(cs_main);
        const CChain& active_chain = chainman.ActiveChain();
        if (blockheight > active_chain.Height()) {
            return RESTERR(cb, drogon::k404NotFound, "Block height out of range");
        }
        pblockindex = active_chain[blockheight];
    }
    switch (rf) {
    case RESTResponseFormat::BINARY: {
        CDataStream ss_blockhash(SER_NETWORK, PROTOCOL_VERSION);
        ss_blockhash << pblockindex->GetBlockHash();
        WriteReply(cb, drogon::CT_APPLICATION_OCTET_STREAM, ss_blockhash.str());
        return true;
    }
    case RESTResponseFormat::HEX: {
        WriteReply(cb, drogon::CT_TEXT_PLAIN, pblockindex->GetBlockHash().GetHex() + "\n");
        return true;
    }
    case RESTResponseFormat::JSON: {
        UniValue resp = UniValue(UniValue::VOBJ);
        resp.pushKV("blockhash", pblockindex->GetBlockHash().GetHex());
        WriteReply(cb, drogon::CT_APPLICATION_JSON, resp.write() + "\n");
        return true;
    }
    default: {
        return RESTERR(cb, drogon::k406NotAcceptable, "output format not found (available: " + AvailableDataFormatsString() + ")");
    }
    }
}

static const struct {
    const char* prefix;
    bool (*handler)(const CoreContext&, const drogon::HttpRequestPtr&, const rest::Callback&, const std::string&);
} uri_prefixes[] = {
      {"tx/", rest_tx},
      {"block/notxdetails/", rest_block_notxdetails},
      {"block/", rest_block_extended},
      {"blockfilter/", rest_block_filter},
      {"blockfilterheaders/", rest_filter_header},
      {"chaininfo", rest_chaininfo},
      {"mempool/", rest_mempool},
      {"headers/", rest_headers},
      {"getutxos", rest_getutxos},
      {"blockhashbyheight/", rest_blockhash_by_height},
};

static void RegisterHandlers(const CoreContext& context)
{
    // registerSyncAdvice intercepts requests before drogon's routing
    // pipeline.  Handlers run directly on the I/O thread — no pool
    // dispatch overhead, no extra allocations.  With enough I/O threads
    // (set via -restthreads), blocking on cs_main is absorbed.
    auto& app = drogon::app();
    app.registerSyncAdvice(
        [context](const drogon::HttpRequestPtr& req) -> drogon::HttpResponsePtr {
            const std::string& path = req->path();

            static constexpr std::string_view REST_PREFIX{"/rest/"};
            if (path.size() < REST_PREFIX.size() ||
                path.compare(0, REST_PREFIX.size(), REST_PREFIX) != 0) {
                return nullptr;
            }

            const std::string_view rest_sv{path.data() + REST_PREFIX.size(),
                                           path.size() - REST_PREFIX.size()};

            for (const auto& up : uri_prefixes) {
                const size_t plen = strlen(up.prefix);
                if (rest_sv.size() < plen ||
                    rest_sv.compare(0, plen, up.prefix) != 0) continue;

                const std::string uri_part{rest_sv.substr(plen)};

                drogon::HttpResponsePtr result;
                rest::Callback cb = [&result](const drogon::HttpResponsePtr& resp) {
                    result = resp;
                };

                up.handler(context, req, cb, uri_part);
                return result;
            }
            return drogon::HttpResponse::newNotFoundResponse();
        });
}

namespace rest {
bool StartServer(const CoreContext& context, const Options& opts)
{
    if (g_server_thread.joinable()) {
        LogPrint(BCLog::REST, "Server already running, ignoring duplicate start\n");
        return false;
    }

    // Pre-flight: verify the port is bindable before handing it to drogon,
    // whose trantor library calls exit() on bind failure which would crash
    // the process.
    if (!IsPortBindable(opts.m_addr, opts.m_port)) {
        LogPrintf("REST: cannot bind to %s:%d, aborting server start\n", opts.m_addr, opts.m_port);
        return false;
    }

    auto& app = drogon::app();
    app.disableSigtermHandling();
    trantor::Logger::setOutputFunction(
        [](const char* msg, const uint64_t len) {
            std::string_view sv{msg, len};
            // Strip trailing newline as it is reintroduced by LogPrint
            while (!sv.empty() && (sv.back() == '\n' || sv.back() == '\r')) {
                sv.remove_suffix(1);
            }
            LogDebug(BCLog::REST, "Drogon reports %.*s\n", static_cast<int>(sv.size()), sv.data());
        },
        []() { /* LogPrint handles flushing */ });

    app.addListener(/*ip=*/opts.m_addr, opts.m_port);
    app.setThreadNum(opts.m_threads);
    app.setMaxConnectionNum(opts.m_max_connections);
    app.setIdleConnectionTimeout(opts.m_idle_timeout);
    app.setPipeliningRequestsNumber(0);  // Unlimited pipelining depth.
    // Disable unnecessary per-response header generation and compression.
    app.enableDateHeader(false);
    app.enableServerHeader(false);
    app.enableGzip(false);
    app.enableBrotli(false);
    if (opts.m_reuse_port) {
        app.enableReusePort();
    }
    LogPrint(BCLog::REST, "Starting server on %s:%d with %d thread(s), max_conn=%d, idle_timeout=%d, reuseport=%d\n",
             opts.m_addr, opts.m_port, opts.m_threads, opts.m_max_connections, opts.m_idle_timeout, opts.m_reuse_port);

    RegisterHandlers(context);

    try {
        g_server_thread = std::thread([] {
            util::ThreadRename("rest-server");
            try {
                drogon::app().run();
            } catch (const std::exception& e) {
                LogPrint(BCLog::REST, "Server failed: %s\n", e.what());
            }
        });
    } catch (const std::exception& e) {
        LogPrint(BCLog::REST, "Failed to start thread: %s\n", e.what());
        return false;
    }

    // Wait for the event loop to be ready so that a subsequent quit() call
    // (e.g. during rapid shutdown) is not lost due to a race with run().
    constexpr auto RUN_PROBE_SLEEP{250ms};
    for (int i{0}; i < MAX_PROBE_DURATION / RUN_PROBE_SLEEP; ++i) {
        if (drogon::app().isRunning()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds{RUN_PROBE_SLEEP});
    }

    LogPrint(BCLog::REST, "Server failed to start\n");
    return false;
}

void InterruptServer()
{
    LogPrint(BCLog::REST, "Interrupting server\n");
    drogon::app().quit();
}

void StopServer()
{
    if (g_server_thread.joinable()) {
        drogon::app().quit();
        g_server_thread.join();
        LogPrint(BCLog::REST, "Server stopped\n");
    }
}
} // namespace rest
