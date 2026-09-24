// Copyright (c) 2021-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <chainlock/clsig.h>

#include <chain.h>
#include <chainlock/chainlock.h>
#include <chainparams.h>
#include <consensus/merkle.h>
#include <evo/cbtx.h>
#include <evo/specialtx.h>
#include <hash.h>
#include <llmq/quorumsman.h>
#include <node/blockstorage.h>
#include <saltedhasher.h>
#include <shutdown.h>
#include <streams.h>
#include <sync.h>
#include <unordered_lru_cache.h>

#include <algorithm>
#include <stdexcept>
#include <string_view>

namespace chainlock {
static constexpr std::string_view CLSIG_REQUESTID_PREFIX{"clsig"};
static constexpr size_t MAX_HISTORICAL_CARRIER_LOOKUPS{16384};

namespace {
/** What one validated coinbase says about ChainLocks: the embedded signature
 *  bytes and height offset, or nothing. Holding raw bytes keeps a hit free of
 *  BLS point decompression; only certificates a proof actually uses are parsed. */
struct CarrierEntry {
    bool has_chainlock{false};
    uint32_t height_diff{0};
    std::array<uint8_t, CBLSSignature::SerSize> signature{};
};

/** Process-wide memo of coinbase ChainLocks, keyed by carrier block hash.
 *  A block hash fixes the block's contents, so an entry never goes stale and a
 *  reorg simply stops asking for the orphaned hashes. Per-request binary search
 *  touches ~40 carriers per certificate, overwhelmingly the same ones for every
 *  request, so this replaces most block reads and signature decoding. */
class CarrierCache
{
    Mutex m_mutex;
    // 32768 entries (≈ 6–7 MiB with map overhead); covers every probe of years of mainnet history.
    Uint256LruHashMap<CarrierEntry, 32768, 36864> m_entries GUARDED_BY(m_mutex);

public:
    bool Get(const uint256& hash, CarrierEntry& entry) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        LOCK(m_mutex);
        return m_entries.get(hash, entry);
    }
    void Insert(const uint256& hash, const CarrierEntry& entry) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        LOCK(m_mutex);
        m_entries.insert(hash, entry);
    }
    void Clear() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        LOCK(m_mutex);
        m_entries.clear();
    }
};

CarrierCache& GetCarrierCache()
{
    static CarrierCache cache;
    return cache;
}

/** Decoded ChainLock signatures, keyed by their serialized bytes and the BLS
 *  scheme they were decoded under. Decoding a G2 point (decompression plus
 *  subgroup check) costs far more than the lookups around it, and proofs sharing
 *  history ask for the same signatures each time. */
class DecodedSignatureCache
{
    Mutex m_mutex;
    // ~300 B per decoded point: 8192 entries ≈ 2.5 MiB.
    Uint256LruHashMap<CBLSSignature, 8192, 9216> m_entries GUARDED_BY(m_mutex);

public:
    CBLSSignature Decode(const std::array<uint8_t, CBLSSignature::SerSize>& bytes) EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        // Decode exactly as the coinbase payload was always decoded: under the
        // process-wide scheme flag, including its opposite-scheme rejection. The
        // flag is part of the key so a transient value cannot leak into later calls.
        const bool legacy = bls::bls_legacy_scheme.load();
        const uint256 key = (CHashWriter(SER_GETHASH, 0) << Span{bytes} << legacy).GetHash();
        CBLSSignature sig;
        {
            LOCK(m_mutex);
            if (m_entries.get(key, sig)) return sig;
        }
        CDataStream in(Span<const uint8_t>{bytes}, SER_NETWORK, PROTOCOL_VERSION);
        sig.Unserialize(in, legacy);
        if (sig.IsValid()) {
            LOCK(m_mutex);
            m_entries.insert(key, sig);
        }
        return sig;
    }
    void Clear() EXCLUSIVE_LOCKS_REQUIRED(!m_mutex)
    {
        LOCK(m_mutex);
        m_entries.clear();
    }
};

DecodedSignatureCache& GetDecodedSignatureCache()
{
    static DecodedSignatureCache cache;
    return cache;
}
} // namespace

void ClearCoinbaseChainLockCacheForTesting()
{
    GetCarrierCache().Clear();
    GetDecodedSignatureCache().Clear();
}

namespace {

/** Offset of bestCLHeightDiff in a CLSIG_AND_BALANCE coinbase payload:
 *  nVersion u16, nHeight i32, merkleRootMNList, merkleRootQuorums. */
constexpr size_t CBTX_CLSIG_OFFSET{2 + 4 + 32 + 32};

/** The raw bestCLSignature bytes of a validated CLSIG_AND_BALANCE payload.
 *  Read from the wire rather than from the decoded CCbTx, whose signature
 *  depends on the process-wide BLS scheme flag at decode time; a cached value
 *  must be a function of the block alone. */
std::array<uint8_t, CBLSSignature::SerSize> RawChainLockSignature(const std::vector<unsigned char>& payload)
{
    CDataStream in(Span<const uint8_t>{payload}.subspan(CBTX_CLSIG_OFFSET), SER_NETWORK, PROTOCOL_VERSION);
    uint64_t height_diff;
    in >> COMPACTSIZE(height_diff);
    std::array<uint8_t, CBLSSignature::SerSize> signature;
    in.read(AsWritableBytes(Span{signature}));
    return signature;
}

/** Reads and validates one carrier coinbase. Throws on missing or invalid data. */
CarrierEntry LoadCarrier(const CBlockIndex* carrier, int carrier_height)
{
    CBlock block;
    if (!node::ReadBlockFromDisk(block, carrier, Params().GetConsensus()) || block.vtx.empty()) {
        throw std::runtime_error("Historical ChainLock block data unavailable");
    }
    // ReadBlockFromDisk checks the header only. The result below is kept for the
    // life of the process, so make sure the coinbase is the one the header commits to.
    if (BlockMerkleRoot(block) != block.hashMerkleRoot) {
        throw std::runtime_error("Historical ChainLock block data corrupted");
    }
    const auto cb = GetTxPayload<CCbTx>(*block.vtx[0]);
    if (!block.vtx[0]->IsCoinBase() || block.vtx[0]->nType != TRANSACTION_COINBASE || !cb ||
        cb->nVersion < CCbTx::Version::CLSIG_AND_BALANCE || cb->nHeight != carrier_height) {
        throw std::runtime_error("Invalid historical ChainLock coinbase");
    }
    if (cb->bestCLHeightDiff >= uint32_t(carrier_height)) {
        throw std::runtime_error("Invalid historical coinbase ChainLock height");
    }
    // Consensus writes a null certificate as all-zero bytes. Anything else is a
    // certificate; an encoding that does not decode surfaces later from Signed()
    // instead of being remembered as absent.
    CarrierEntry entry;
    entry.signature = RawChainLockSignature(block.vtx[0]->vExtraPayload);
    entry.has_chainlock = std::ranges::any_of(entry.signature, [](uint8_t b) { return b != 0; });
    entry.height_diff = cb->bestCLHeightDiff;
    return entry;
}
} // namespace

std::optional<CoinbaseChainLock> CoinbaseChainLockReader::Read(int carrier_height)
{
    const auto* carrier = m_tip ? m_tip->GetAncestor(carrier_height) : nullptr;
    if (!carrier || carrier_height < Params().GetConsensus().V20Height) return std::nullopt;
    if (const auto it = m_cache.find(carrier_height); it != m_cache.end()) return it->second;
    if (ShutdownRequested()) throw std::runtime_error("ChainLock lookup interrupted");
    if (m_cache.size() >= MAX_HISTORICAL_CARRIER_LOOKUPS) throw std::runtime_error("ChainLock lookup budget exhausted");
    const uint256 carrier_hash = carrier->GetBlockHash();
    CarrierEntry entry;
    if (!GetCarrierCache().Get(carrier_hash, entry)) {
        entry = LoadCarrier(carrier, carrier_height);
        GetCarrierCache().Insert(carrier_hash, entry);
    }
    if (!entry.has_chainlock) {
        return m_cache.emplace(carrier_height, std::nullopt).first->second;
    }
    const int height = carrier_height - int(entry.height_diff) - 1;
    const auto* signed_block = m_tip->GetAncestor(height);
    if (!signed_block) throw std::runtime_error("Invalid historical ChainLock block height");
    // Deferred decode: binary search only compares heights, so most carriers
    // never need their signature. Callers that build a certificate decode it.
    return m_cache
        .emplace(carrier_height, CoinbaseChainLock{height, signed_block->GetBlockHash(), carrier, entry.signature})
        .first->second;
}

ChainLockSig CoinbaseChainLock::Signed() const
{
    // Validated by consensus when the carrier was connected, so a failure here
    // means corrupt block data (or a transient BLS scheme flag); surface it for
    // this request rather than proving with it.
    const CBLSSignature sig = GetDecodedSignatureCache().Decode(signature_bytes);
    if (!sig.IsValid()) throw std::runtime_error("Invalid historical ChainLock signature");
    return ChainLockSig{height, block_hash, sig};
}

std::optional<CoinbaseChainLock> CoinbaseChainLockReader::Find(int minimum_height, int maximum_height)
{
    const int tip_height = m_tip ? m_tip->nHeight : -1;
    if (minimum_height < 0 || minimum_height > maximum_height || minimum_height >= tip_height)
        return std::nullopt;
    const int first_carrier = std::max(minimum_height + 1, Params().GetConsensus().V20Height);
    if (first_carrier > tip_height) return std::nullopt;

    // CheckCbTxBestChainlock enforces nondecreasing certified heights and
    // forbids null signatures after the first certificate on a validated chain.
    auto below_minimum = [&](int height) {
        const auto entry = Read(height);
        return !entry || entry->height < minimum_height;
    };
    int low = first_carrier;
    int high = first_carrier;
    int64_t step = 1;
    while (below_minimum(high)) {
        if (high == tip_height) return std::nullopt;
        low = high + 1;
        high = int(std::min<int64_t>(tip_height, int64_t(high) + step));
        step *= 2;
    }
    while (low < high) {
        const int middle = low + (high - low) / 2;
        if (below_minimum(middle)) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    const auto entry = Read(low);
    return entry && entry->height <= maximum_height ? entry : std::nullopt;
}

uint256 GenSigRequestId(const int32_t nHeight)
{
    return ::SerializeHash(std::make_pair(CLSIG_REQUESTID_PREFIX, nHeight));
}

llmq::VerifyRecSigStatus VerifyChainLock(const Consensus::Params& params, const CChain& chain,
                                         const llmq::CQuorumManager& qman, const chainlock::ChainLockSig& clsig)
{
    const auto llmqType = params.llmqTypeChainLocks;
    const uint256 request_id = GenSigRequestId(clsig.getHeight());

    return llmq::VerifyRecoveredSig(llmqType, chain, qman, clsig.getHeight(), request_id, clsig.getBlockHash(),
                                    clsig.getSig());
}

llmq::VerifyRecSigStatus VerifyChainLock(const Consensus::Params& params, const llmq::CQuorumManager& qman,
                                         const chainlock::ChainLockSig& clsig, const CBlockIndex* pindexStart)
{
    const auto llmqType = params.llmqTypeChainLocks;
    const uint256 request_id = GenSigRequestId(clsig.getHeight());

    return llmq::VerifyRecoveredSig(llmqType, qman, pindexStart, request_id, clsig.getBlockHash(), clsig.getSig());
}
} // namespace chainlock
