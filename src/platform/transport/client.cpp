// Copyright (c) 2026 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <platform/client.h>

#include <platform/drive/queries.h>
#include <platform/dpp/document.h>
#include <platform/transport/cbor.h>
#include <platform/transport/endpoint_retry.h>
#include <platform/transport/freshness.h>
#include <platform/transport/grpcweb.h>
#include <platform/transport/protobuf.h>

#include <logging.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

namespace platform {

namespace {

constexpr char SVC[] = "/org.dash.platform.dapi.v0.Platform/";
constexpr int CALL_TIMEOUT_MS = 20000;
//! Bound on how many distinct endpoints a single logical operation retries
//! before giving up, so a persistently failing query terminates.
constexpr size_t MAX_OP_ATTEMPTS{4};

pb::Writer VersionWrap(pb::Writer inner)
{
    pb::Writer w;
    w.Message(1, inner.take());
    return w;
}

class GrpcWebClient final : public PlatformClient
{
public:
    explicit GrpcWebClient(Params params) : m_params(std::move(params))
    {
        // Push the network context into the bridge's verification provider.
        // The Platform activation height is only consulted by FromProof
        // paths the GUI does not use, so 0 ("unknown") is passed until a
        // caller needs it.
        std::string err;
        if (!drive::SetContext(m_params.network_id, /*platform_activation_height=*/0, err)) {
            LogPrintf("Platform client: unable to set verification context: %s\n", err);
        }
        m_worker = std::thread([this] { Run(); });
    }
    ~GrpcWebClient() override { shutdown(); }

    void shutdown() override
    {
        if (m_stop.exchange(true)) return;
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            m_queue.clear();
        }
        m_cv.notify_all();
        if (m_worker.joinable()) m_worker.join();
    }

    void updateEndpoints(std::vector<Endpoint> endpoints) override
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (m_endpoints == endpoints) return;
        m_endpoints = std::move(endpoints);
        m_ep_index = m_endpoints.empty() ? 0 : m_ep_index % m_endpoints.size();
    }
    void updateQuorumKeys(uint8_t llmq_type, std::vector<QuorumKey> keys) override
    {
        // Quorum-signature verification happens inside the Rust bridge; hand
        // the keys over in the byte order DAPI proofs carry the quorum hash
        // (display order, the reverse of uint256's internal order — the
        // representation boundary QuorumKey::matchesProofHash documents).
        std::vector<drive::BridgeQuorumKey> bridge_keys;
        bridge_keys.reserve(keys.size());
        for (const QuorumKey& key : keys) {
            drive::BridgeQuorumKey bridge_key;
            for (size_t i = 0; i < bridge_key.proof_hash.size(); ++i) {
                bridge_key.proof_hash[i] = key.quorum_hash.begin()[bridge_key.proof_hash.size() - 1 - i];
            }
            bridge_key.pubkey = key.pubkey;
            bridge_keys.push_back(std::move(bridge_key));
        }
        std::string err;
        if (!drive::UpdateQuorumKeys(llmq_type, bridge_keys, err)) {
            LogPrintf("Platform client: unable to update quorum keys: %s\n", err);
        }
    }
    void updateCoreChainLockedHeight(int32_t height) override
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        m_freshness.SetLocalChainLockHeight(height);
    }

    // Queries — each enqueues a task on the worker thread.
    void resolveName(const std::string& normalized_label, Callback<std::optional<DpnsName>> cb) override
    {
        Enqueue([=, this] { DoResolveName(normalized_label, cb); });
    }
    void searchNames(const std::string& prefix, uint32_t limit, Callback<std::vector<DpnsName>> cb) override
    {
        Enqueue([=, this] { DoSearchNames(prefix, limit, cb); });
    }
    void namesOfIdentity(const Identifier& identity, Callback<std::vector<DpnsName>> cb) override
    {
        Enqueue([=, this] { DoNamesOfIdentity(identity, cb); });
    }
    void getIdentity(const Identifier& id, Callback<std::optional<Identity>> cb) override
    {
        Enqueue([=, this] { DoGetIdentity(id, cb); });
    }
    void getIdentityByPublicKeyHash(const std::array<uint8_t, 20>& h, Callback<std::optional<Identity>> cb) override
    {
        Enqueue([=, this] { DoGetIdentityByPubKeyHash(h, cb); });
    }
    void getIdentityNonce(const Identifier& id, Callback<uint64_t> cb) override
    {
        Enqueue([=, this] { DoGetNonce(id, std::nullopt, cb); });
    }
    void getIdentityContractNonce(const Identifier& id, const Identifier& contract, Callback<uint64_t> cb) override
    {
        Enqueue([=, this] { DoGetNonce(id, contract, cb); });
    }
    void getProfile(const Identifier& owner_id, Callback<std::optional<Profile>> cb) override
    {
        Enqueue([=, this] { DoGetProfile(owner_id, cb); });
    }
    void getContactRequests(const Identifier& identity, bool to_me, uint64_t since_ms,
                            Callback<std::vector<ContactRequest>> cb) override
    {
        Enqueue([=, this] { DoGetContactRequests(identity, to_me, since_ms, cb); });
    }
    void getContestedNameState(const std::string& normalized_label, Callback<ContestedNameState> cb) override
    {
        Enqueue([=, this] { DoGetContestedNameState(normalized_label, cb); });
    }
    void broadcastStateTransition(const std::vector<uint8_t>& st, Callback<BroadcastResult> cb) override
    {
        Enqueue([=, this] { DoBroadcast(st, cb); });
    }

private:
    // ---- worker plumbing ----
    void Enqueue(std::function<void()> task)
    {
        {
            std::lock_guard<std::mutex> lk(m_mtx);
            if (m_stop) return;
            m_queue.push_back(std::move(task));
        }
        m_cv.notify_one();
    }
    void Run()
    {
        for (;;) {
            std::function<void()> task;
            {
                std::unique_lock<std::mutex> lk(m_mtx);
                m_cv.wait(lk, [this] { return m_stop || !m_queue.empty(); });
                if (m_stop) return;
                task = std::move(m_queue.front());
                m_queue.pop_front();
            }
            task();
        }
    }

    //! Stable identity of an endpoint for per-endpoint freshness tracking:
    //! its deterministic-masternode proTxHash when known, else its address.
    static std::string EndpointKey(const Endpoint& ep)
    {
        if (!ep.pro_tx_hash.IsNull()) return ep.pro_tx_hash.ToString();
        return ep.service.ToStringAddrPort();
    }

    //! Snapshot the current endpoint set and advance the round-robin cursor
    //! once per logical operation, so successive operations start at
    //! different nodes but a single operation can pin one node.
    std::vector<Endpoint> SnapshotEndpoints(size_t& start)
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        start = m_ep_index++;
        return m_endpoints;
    }

    //! Round-robin pick of a single endpoint (advancing the cursor).
    std::optional<Endpoint> NextEndpoint()
    {
        std::lock_guard<std::mutex> lk(m_mtx);
        if (m_endpoints.empty()) return std::nullopt;
        const Endpoint ep = m_endpoints[m_ep_index % m_endpoints.size()];
        ++m_ep_index;
        return ep;
    }

    //! Issue a single unary call to one specific endpoint (no rotation).
    transport::GrpcCallResult CallOn(const Endpoint& ep, const std::string& method,
                                     const std::vector<uint8_t>& req)
    {
        if (m_stop) {
            return {
                .transport_ok = false,
                .grpc_status = -1,
                .grpc_message = {},
                .message = {},
                .transport_error = "Platform request interrupted",
            };
        }
        return transport::GrpcWebUnary(ep.service.ToStringAddr(), ep.service.GetPort(), std::string(SVC) + method, req,
                                       CALL_TIMEOUT_MS, [this] { return m_stop.load(); });
    }

    //! Post-verification checks on the signature-authenticated metadata of a
    //! proved response: the tenderdash chain id must be this network's (the
    //! quorum signature covers it, so a cross-chain replay cannot forge it),
    //! and the response must be fresh (transport::FreshnessTracker) so an
    //! on-path attacker (TLS is unauthenticated by design) cannot feed a
    //! stale-but-validly-signed response. `endpoint_key` identifies the
    //! answering node so a lagging honest node is not rejected merely
    //! because a different node was ahead.
    bool AcceptMeta(const ResponseMetadata& meta, const std::string& endpoint_key, std::string& err)
    {
        if (meta.chain_id != m_params.tenderdash_chain_id) {
            err = "response signed for tenderdash chain '" + meta.chain_id + "', expected '" +
                  m_params.tenderdash_chain_id + "'";
            return false;
        }
        std::lock_guard<std::mutex> lk(m_mtx);
        return m_freshness.Accept(endpoint_key, meta.height, meta.core_chain_locked_height, err);
    }

    //! Rotating single-shot call with transport-level failover across
    //! endpoints. On success, reports the answering endpoint's key (for
    //! per-endpoint freshness) via `endpoint_key` when non-null. Suitable for
    //! operations that make exactly one proved request; multi-proof
    //! operations pin an endpoint via RetryAcrossEndpoints + CallOn instead.
    transport::GrpcCallResult Call(const std::string& method, const std::vector<uint8_t>& req,
                                   std::string& transport_err, std::string* endpoint_key = nullptr)
    {
        const size_t n = [&] { std::lock_guard<std::mutex> lk(m_mtx); return std::max<size_t>(1, m_endpoints.size()); }();
        transport::GrpcCallResult last;
        for (size_t i = 0; i < n; ++i) {
            if (m_stop) {
                transport_err = "Platform request interrupted";
                return last;
            }
            const auto ep = NextEndpoint();
            if (!ep) { transport_err = "no evonode endpoints available"; return {}; }
            last = CallOn(*ep, method, req);
            if (last.transport_ok) {
                if (endpoint_key != nullptr) *endpoint_key = EndpointKey(*ep);
                return last;
            }
            transport_err = last.transport_error;
        }
        return last;
    }

    // ---- operations ----
    void DoBroadcast(const std::vector<uint8_t>& st, const Callback<BroadcastResult>& cb)
    {
        pb::Writer w;
        w.Bytes(1, st);
        std::string terr;
        auto r = Call("broadcastStateTransition", w.data(), terr);
        Result<BroadcastResult> out;
        if (!r.transport_ok) { out.error = terr; cb(out); return; }
        BroadcastResult br;
        br.accepted = (r.grpc_status == 0);
        if (!br.accepted) { br.error = r.grpc_message; br.error_code = static_cast<uint32_t>(r.grpc_status); }
        out.value = br;
        cb(out);
    }

    void DoGetNonce(const Identifier& id, std::optional<Identifier> contract, const Callback<uint64_t>& cb)
    {
        pb::Writer v0;
        v0.Bytes(1, std::vector<uint8_t>(id.begin(), id.end()));
        std::string method;
        if (contract) {
            v0.Bytes(2, std::vector<uint8_t>(contract->begin(), contract->end()));
            v0.Bool(3, true);
            method = "getIdentityContractNonce";
        } else {
            v0.Bool(2, true);
            method = "getIdentityNonce";
        }
        const std::vector<uint8_t> request{VersionWrap(std::move(v0)).take()};
        std::string terr, endpoint_key;
        auto r = Call(method, request, terr, &endpoint_key);
        Result<uint64_t> out;
        if (!r.transport_ok || r.grpc_status != 0) { out.error = r.transport_ok ? r.grpc_message : terr; cb(out); return; }
        std::optional<uint64_t> nonce;
        ResponseMetadata meta;
        std::string err;
        const bool verified = contract
            ? drive::VerifyGetIdentityContractNonce(request, r.message, nonce, meta, err)
            : drive::VerifyGetIdentityNonce(request, r.message, nonce, meta, err);
        if (!verified || !AcceptMeta(meta, endpoint_key, err)) {
            out.error = method + ": proof verification failed: " + err; cb(out); return;
        }
        out.value = nonce.value_or(0);
        out.metadata = std::move(meta);
        cb(out);
    }

    void DoGetIdentity(const Identifier& id, const Callback<std::optional<Identity>>& cb)
    {
        // Proof-verified full identity: a single getIdentity request whose
        // proof covers balance, revision and keys at one height
        // (Drive::verify_full_identity_by_identity_id inside the bridge). A
        // response that fails verification retries against another endpoint.
        Result<std::optional<Identity>> out;

        size_t start{0};
        const std::vector<Endpoint> endpoints{SnapshotEndpoints(start)};
        if (endpoints.empty()) { out.error = "no evonode endpoints available"; cb(out); return; }

        pb::Writer v0;
        v0.Bytes(1, std::vector<uint8_t>(id.begin(), id.end()));
        v0.Bool(2, true); // prove
        const std::vector<uint8_t> request{VersionWrap(std::move(v0)).take()};

        bool succeeded{false};
        transport::RetryAcrossEndpoints(endpoints, start, MAX_OP_ATTEMPTS,
            [&](const Endpoint& ep, size_t) -> transport::AttemptStatus {
                auto r = CallOn(ep, "getIdentity", request);
                if (!r.transport_ok || r.grpc_status != 0) {
                    out.error = r.transport_ok ? r.grpc_message : r.transport_error;
                    return transport::AttemptStatus::Retry;
                }
                std::optional<Identity> identity;
                ResponseMetadata meta;
                std::string err;
                if (!drive::VerifyGetIdentity(request, r.message, identity, meta, err) ||
                    !AcceptMeta(meta, EndpointKey(ep), err)) {
                    out.error = "identity proof verification failed: " + err;
                    return transport::AttemptStatus::Retry;
                }
                out.value = std::move(identity);
                out.metadata = std::move(meta);
                out.error.clear();
                succeeded = true;
                return transport::AttemptStatus::Success;
            });

        if (!succeeded && out.error.empty()) out.error = "identity query failed against all endpoints";
        cb(out);
    }

    void DoGetIdentityByPubKeyHash(const std::array<uint8_t, 20>& h, const Callback<std::optional<Identity>>& cb)
    {
        // One proof resolves the unique key hash to the full identity
        // (Drive::verify_full_identity_by_unique_public_key_hash inside the
        // bridge); no follow-up getIdentity round trip is needed.
        pb::Writer v0;
        v0.Bytes(1, std::vector<uint8_t>(h.begin(), h.end()));
        v0.Bool(2, true);
        const std::vector<uint8_t> request{VersionWrap(std::move(v0)).take()};
        std::string terr, endpoint_key;
        auto r = Call("getIdentityByPublicKeyHash", request, terr, &endpoint_key);
        Result<std::optional<Identity>> out;
        if (!r.transport_ok || r.grpc_status != 0) { out.error = r.transport_ok ? r.grpc_message : terr; cb(out); return; }
        std::optional<Identity> identity;
        ResponseMetadata meta;
        std::string err;
        if (!drive::VerifyGetIdentityByPubKeyHash(request, r.message, identity, meta, err) ||
            !AcceptMeta(meta, endpoint_key, err)) {
            out.error = err; cb(out); return;
        }
        out.value = std::move(identity);
        out.metadata = std::move(meta);
        cb(out);
    }

    // Document queries (DPNS domain / DashPay profile & contactRequest). The
    // server returns only a GroveDB proof; the bridge reconstructs the
    // requested Drive query from the request bytes — including cryptographic
    // absence — and binds the proven root to a locally-known Platform LLMQ
    // key before document bytes escape.
    std::vector<std::vector<uint8_t>> GetDocuments(const Identifier& contract, const std::string& doc_type,
                                                   const std::vector<uint8_t>& where_cbor, uint32_t limit,
                                                   std::string& err,
                                                   const std::vector<uint8_t>& order_by_cbor = {})
    {
        pb::Writer v0;
        v0.Bytes(1, std::vector<uint8_t>(contract.begin(), contract.end()));
        v0.Str(2, doc_type);
        if (!where_cbor.empty()) v0.Bytes(3, where_cbor);
        if (!order_by_cbor.empty()) v0.Bytes(4, order_by_cbor);
        if (limit) v0.Varint(5, limit);
        v0.Bool(8, true); // prove
        const std::vector<uint8_t> request{VersionWrap(std::move(v0)).take()};
        std::string terr, endpoint_key;
        auto r = Call("getDocuments", request, terr, &endpoint_key);
        std::vector<std::vector<uint8_t>> docs;
        if (!r.transport_ok || r.grpc_status != 0) {
            err = r.transport_ok ? r.grpc_message : terr;
            LogPrintf("Platform getDocuments(%s) failed: %s\n", doc_type, err);
            return docs;
        }
        ResponseMetadata meta;
        if (!drive::VerifyGetDocuments(request, r.message, docs, meta, err)) {
            LogPrintf("Platform getDocuments(%s) proof verification failed: %s\n", doc_type, err);
            return {};
        }
        if (!AcceptMeta(meta, endpoint_key, err)) {
            LogPrintf("Platform getDocuments(%s) metadata rejected: %s\n", doc_type, err);
            return {};
        }
        return docs;
    }

    //! drive-abci decodes each getContestedResourceVoteState index value as a
    //! bincode (standard, big-endian) platform Value; strings are
    //! Value::Text = declaration-order discriminant 18 + length + utf8
    //! (rs-platform-value src/lib.rs, rs-drive-abci
    //! src/query/voting/contested_resource_vote_state/v0/mod.rs).
    static std::vector<uint8_t> BincodeTextValue(const std::string& text)
    {
        std::vector<uint8_t> out;
        out.push_back(18);
        // bincode varint: single byte below 251; DPNS labels are <= 63 chars.
        if (text.size() >= 251) return {};
        out.push_back(static_cast<uint8_t>(text.size()));
        out.insert(out.end(), text.begin(), text.end());
        return out;
    }

    //! Mirrors drive-abci's default_query_limit for requests that pin `count`
    //! so the locally reconstructed PathQuery matches the prover's exactly.
    static constexpr uint16_t CONTESTED_VOTE_COUNT{100};

    void DoGetContestedNameState(const std::string& normalized_label, const Callback<ContestedNameState>& cb)
    {
        // getContestedResourceVoteState on the DPNS contested
        // parentNameAndLabel index, VoteTally result type with locked and
        // abstaining tallies (platform.proto
        // GetContestedResourceVoteStateRequestV0).
        pb::Writer v0;
        v0.Bytes(1, std::vector<uint8_t>(DPNS_CONTRACT_ID.begin(), DPNS_CONTRACT_ID.end()));
        v0.Str(2, "domain");
        v0.Str(3, "parentNameAndLabel");
        v0.Bytes(4, BincodeTextValue("dash"));
        v0.Bytes(4, BincodeTextValue(normalized_label));
        v0.Varint(5, 1); // ResultType::VOTE_TALLY
        v0.Bool(6, true); // allow_include_locked_and_abstaining_vote_tally
        v0.Varint(8, CONTESTED_VOTE_COUNT);
        v0.Bool(9, true); // prove
        const std::vector<uint8_t> request{VersionWrap(std::move(v0)).take()};
        std::string terr, endpoint_key;
        auto r = Call("getContestedResourceVoteState", request, terr, &endpoint_key);
        Result<ContestedNameState> out;
        if (!r.transport_ok || r.grpc_status != 0) {
            out.error = r.transport_ok ? r.grpc_message : terr;
            cb(out);
            return;
        }
        drive::ContestedVoteState state;
        ResponseMetadata meta;
        std::string err;
        if (!drive::VerifyGetContestedVoteState(request, r.message, state, meta, err) ||
            !AcceptMeta(meta, endpoint_key, err)) {
            out.error = "contested vote state proof verification failed: " + err;
            cb(out);
            return;
        }
        out.metadata = std::move(meta);

        ContestedNameState result;
        result.normalized_label = normalized_label;
        result.contenders = std::move(state.contenders);
        result.abstain_votes = state.abstain_votes.value_or(0);
        result.lock_votes = state.lock_votes.value_or(0);
        if (!state.contest_found) {
            result.status = ContestedNameState::Status::UNKNOWN;
        } else if (!state.finished) {
            result.status = ContestedNameState::Status::CONTEST_IN_PROGRESS;
        } else if (state.locked) {
            result.status = ContestedNameState::Status::LOCKED;
            result.ends_at = state.finished_at_time_ms;
        } else {
            result.status = ContestedNameState::Status::WON;
            result.winner = state.winner;
            result.ends_at = state.finished_at_time_ms;
        }
        out.value = std::move(result);
        cb(out);
    }

    static std::vector<uint8_t> DpnsWhere(const std::string& normalized_label, bool starts_with)
    {
        transport::cbor::Writer w;
        w.Array(2);
        w.Array(3); w.Text("normalizedParentDomainName"); w.Text("=="); w.Text("dash");
        w.Array(3); w.Text("normalizedLabel"); w.Text(starts_with ? "startsWith" : "=="); w.Text(normalized_label);
        return w.take();
    }

    static std::vector<uint8_t> DpnsPrefixOrderBy()
    {
        transport::cbor::Writer w;
        w.Array(1);
        w.Array(2); w.Text("normalizedLabel"); w.Text("asc");
        return w.take();
    }

    void DoResolveName(const std::string& normalized_label, const Callback<std::optional<DpnsName>>& cb)
    {
        std::string err;
        auto docs = GetDocuments(DPNS_CONTRACT_ID, "domain", DpnsWhere(normalized_label, false), 1, err);
        Result<std::optional<DpnsName>> out;
        if (!err.empty()) { out.error = err; cb(out); return; }
        if (docs.empty()) { out.value = std::optional<DpnsName>{}; cb(out); return; } // available / absent
        DpnsName name;
        if (dpp::DecodeDpnsDomain(docs.front(), name)) { out.value = name; }
        else {
            out.error = "DPNS domain document decoding failed";
            LogPrintf("Platform resolveName(%s): %s (%u bytes)\n", normalized_label, out.error,
                      docs.front().size());
        }
        cb(out);
    }

    void DoSearchNames(const std::string& prefix, uint32_t limit, const Callback<std::vector<DpnsName>>& cb)
    {
        std::string err;
        auto docs = GetDocuments(DPNS_CONTRACT_ID, "domain", DpnsWhere(prefix, true), limit, err,
                                 DpnsPrefixOrderBy());
        Result<std::vector<DpnsName>> out;
        if (!err.empty()) { out.error = err; cb(out); return; }
        std::vector<DpnsName> names;
        for (const auto& d : docs) {
            DpnsName n;
            if (dpp::DecodeDpnsDomain(d, n)) names.push_back(std::move(n));
        }
        out.value = std::move(names);
        cb(out);
    }

    void DoNamesOfIdentity(const Identifier& identity, const Callback<std::vector<DpnsName>>& cb)
    {
        transport::cbor::Writer w;
        w.Array(1);
        w.Array(3); w.Text("records.identity"); w.Text("==");
        w.Bytes(Span<const uint8_t>{identity.data(), identity.size()});
        std::string err;
        constexpr uint32_t limit{100};
        auto docs = GetDocuments(DPNS_CONTRACT_ID, "domain", w.take(), limit, err);
        Result<std::vector<DpnsName>> out;
        if (!err.empty()) { out.error = err; cb(out); return; }
        std::vector<DpnsName> names;
        for (const auto& d : docs) {
            DpnsName n;
            if (dpp::DecodeDpnsDomain(d, n)) names.push_back(std::move(n));
        }
        out.value = std::move(names);
        cb(out);
    }

    void DoGetProfile(const Identifier& owner_id, const Callback<std::optional<Profile>>& cb)
    {
        transport::cbor::Writer w;
        w.Array(1);
        w.Array(3); w.Text("$ownerId"); w.Text("=="); w.Bytes(Span<const uint8_t>{owner_id.data(), owner_id.size()});
        std::string err;
        auto docs = GetDocuments(DASHPAY_CONTRACT_ID, "profile", w.take(), 1, err);
        Result<std::optional<Profile>> out;
        if (!err.empty()) { out.error = err; cb(out); return; }
        if (docs.empty()) { out.value = std::optional<Profile>{}; cb(out); return; }
        Profile prof;
        if (dpp::DecodeDashPayProfile(docs.front(), prof)) out.value = prof;
        else out.value = std::optional<Profile>{};
        cb(out);
    }

    void DoGetContactRequests(const Identifier& identity, bool to_me, uint64_t /*since_ms*/,
                              const Callback<std::vector<ContactRequest>>& cb)
    {
        transport::cbor::Writer w;
        w.Array(1);
        w.Array(3);
        w.Text(to_me ? "toUserId" : "$ownerId");
        w.Text("==");
        w.Bytes(Span<const uint8_t>{identity.data(), identity.size()});
        transport::cbor::Writer order_by;
        order_by.Array(1);
        order_by.Array(2);
        order_by.Text("$createdAt");
        order_by.Text("asc");
        std::string err;
        auto docs = GetDocuments(DASHPAY_CONTRACT_ID, "contactRequest", w.take(), 100, err,
                                 order_by.take());
        Result<std::vector<ContactRequest>> out;
        if (!err.empty()) { out.error = err; cb(out); return; }
        std::vector<ContactRequest> reqs;
        for (const auto& d : docs) {
            ContactRequest cr;
            if (dpp::DecodeDashPayContactRequest(d, cr)) reqs.push_back(std::move(cr));
        }
        out.value = std::move(reqs);
        cb(out);
    }

    Params m_params;
    std::thread m_worker;
    std::mutex m_mtx;
    std::condition_variable m_cv;
    std::deque<std::function<void()>> m_queue;
    std::atomic_bool m_stop{false};
    std::vector<Endpoint> m_endpoints;
    size_t m_ep_index{0};
    //! Per-endpoint replay/staleness guard (guarded by m_mtx).
    transport::FreshnessTracker m_freshness;
};

} // namespace

std::unique_ptr<PlatformClient> MakeGrpcWebPlatformClient(const Params& params)
{
    return std::make_unique<GrpcWebClient>(params);
}

} // namespace platform
