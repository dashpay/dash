// Copyright (c) 2018-2025 The Dash Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <bench/bench.h>

#include <bls/bls_worker.h>
#include <clientversion.h>
#include <hash.h>
#include <llmq/options_args.h>
#include <streams.h>
#include <util/helpers.h>

#include <random.h>

#include <map>
#include <memory>
#include <utility>
#include <vector>

struct Member {
    CBLSId id;

    BLSVerificationVectorPtr vvec;
    std::vector<CBLSSecretKey> skShares;
};

//! What the members of a quorum contribute to a DKG
struct Contributions {
    std::vector<CBLSId> ids;
    std::vector<Member> members;

    Contributions(CBLSWorker& blsWorker, int quorumSize, int threshold)
    {
        members.reserve(quorumSize);
        ids.reserve(quorumSize);

        for (const int i : util::irange(quorumSize)) {
            uint256 id;
            WriteLE64(id.begin(), i + 1);
            members.push_back({CBLSId(id), {}, {}});
            ids.emplace_back(id);
        }
        for (auto& member : members) {
            blsWorker.GenerateContributions(threshold, ids, member.vvec, member.skShares);
        }
    }
};

//! Generating the contributions of a quorum takes much longer than any step measured on them, so
//! every benchmark of a quorum of the same shape works on the same ones. No step changes them.
static const Contributions& SharedContributions(CBLSWorker& blsWorker, int quorumSize, int threshold)
{
    // Never destroyed: the secret keys in it would outlive the pool their memory comes from.
    static auto& cache{*new std::map<std::pair<int, int>, std::unique_ptr<const Contributions>>};
    auto& entry{cache[{quorumSize, threshold}]};
    if (!entry) {
        entry = std::make_unique<const Contributions>(blsWorker, quorumSize, threshold);
    }
    return *entry;
}

class DKG
{
private:
    CBLSWorker blsWorker;
    //! Set when the contributions are generated for this object alone
    std::unique_ptr<const Contributions> ownContributions;
    const Contributions& contributions;
    const std::vector<CBLSId>& ids;
    const std::vector<Member>& members;
    std::vector<BLSVerificationVectorPtr> receivedVvecs;

    std::vector<CBLSSecretKey> receivedSkShares;
    BLSVerificationVectorPtr quorumVvec;

    const Contributions& Init(int quorumSize, int threshold, bool shared)
    {
        blsWorker.Start(llmq::DEFAULT_WORKER_COUNT);
        if (shared) {
            return SharedContributions(blsWorker, quorumSize, threshold);
        }
        ownContributions = std::make_unique<const Contributions>(blsWorker, quorumSize, threshold);
        return *ownContributions;
    }

    void ReceiveVvecs()
    {
        receivedVvecs.clear();
        for (const auto& member : members) {
            receivedVvecs.emplace_back(member.vvec);
        }
        quorumVvec = blsWorker.BuildQuorumVerificationVector(receivedVvecs);
    }

    void ReceiveShares(size_t whoAmI)
    {
        receivedSkShares.clear();
        for (const auto& member : members) {
            receivedSkShares.emplace_back(member.skShares[whoAmI]);
        }
    }

    void VerifyContributionShares(size_t whoAmI, const std::set<size_t>& invalidIndexes, bool aggregated)
    {
        auto result = blsWorker.VerifyContributionShares(members[whoAmI].id, receivedVvecs, receivedSkShares,
                                                         /*parallel=*/true, aggregated);
        for (const size_t i : util::irange(receivedVvecs.size())) {
            if (invalidIndexes.count(i)) {
                assert(!result[i]);
            } else {
                assert(result[i]);
            }
        }
    }

public:
    explicit DKG(int quorumSize) :
        DKG(quorumSize, quorumSize / 2 + 1)
    {
    }

    DKG(int quorumSize, int threshold, bool shared = true) :
        contributions(Init(quorumSize, threshold, shared)),
        ids(contributions.ids),
        members(contributions.members)
    {
    }
    ~DKG()
    {
        blsWorker.Stop();
    }

    void Bench_BuildQuorumVerificationVectors(benchmark::Bench& bench, uint32_t epoch_iters)
    {
        ReceiveVvecs();

        bench.minEpochIterations(epoch_iters).run([&] {
            quorumVvec = blsWorker.BuildQuorumVerificationVector(receivedVvecs, false);
        });
    }

    // The work of ActiveDKGSession::SendCommitment. A cold run decodes every verification vector from its
    // database encoding first, as CDKGSessionManager::GetVerifiedContributions does on a cache miss.
    void Bench_BuildCommitment(benchmark::Bench& bench, bool cold)
    {
        ReceiveVvecs();
        ReceiveShares(0);
        DataStream db{};
        for (const auto& vvec : receivedVvecs) {
            db << *vvec;
        }

        bench.epochs(3).run([&] {
            auto vvecs = receivedVvecs;
            if (cold) {
                DataStream s{db};
                for (auto& vvec : vvecs) {
                    vvec = std::make_shared<std::vector<CBLSPublicKey>>();
                    s >> *vvec;
                }
            }
            const auto vvec = blsWorker.BuildQuorumVerificationVector(vvecs);
            const auto skShare = blsWorker.AggregateSecretKeys(receivedSkShares);
            const auto sig = skShare.Sign(::SerializeHash(*vvec));
            assert(sig.IsValid());
        });
    }

    // The per-sender work of CDKGSession::ReceiveMessage(CDKGPrematureCommitment) once the vvec is built
    void Bench_VerifyCommitment(benchmark::Bench& bench)
    {
        ReceiveVvecs();
        ReceiveShares(0);
        const uint256 hash = ::SerializeHash(*quorumVvec);
        const auto sig = blsWorker.AggregateSecretKeys(receivedSkShares).Sign(hash);

        bench.run([&] {
            const bool valid = ::SerializeHash(*quorumVvec) == hash &&
                               sig.VerifyInsecure(CBLSWorker::BuildPubKeyShare(quorumVvec, ids[0]), hash);
            assert(valid);
        });
    }

    void Bench_VerifyContributionShares(benchmark::Bench& bench, int invalidCount, bool aggregated, uint32_t epoch_iters)
    {
        ReceiveVvecs();
        size_t memberIdx = 0;
        // one run for a large quorum takes over a second
        bench.epochs(members.size() < 400 ? 11 : 3).minEpochIterations(epoch_iters).run([&] {
            ReceiveShares(memberIdx);

            std::set<size_t> invalidIndexes;
            for ([[maybe_unused]] const auto _ : util::irange(invalidCount)) {
                size_t shareIdx = GetRand<size_t>(receivedSkShares.size());
                receivedSkShares[shareIdx].MakeNewKey();
                invalidIndexes.emplace(shareIdx);
            }

            VerifyContributionShares(memberIdx, invalidIndexes, aggregated);

            memberIdx = (memberIdx + 1) % members.size();
        });
    }
};

static void BLSDKG_GenerateContributions(benchmark::Bench& bench, uint32_t epoch_iters, int quorumSize)
{
    CBLSWorker blsWorker;
    blsWorker.Start(llmq::DEFAULT_WORKER_COUNT);
    std::vector<CBLSId> ids;
    std::vector<Member> members;
    if (!bench.output()) {
        epoch_iters = 1;
        quorumSize = 1;
    }
    for (const int i : util::irange(quorumSize)) {
        uint256 id;
        WriteLE64(id.begin(), i + 1);
        members.push_back({CBLSId(id), {}, {}});
        ids.emplace_back(id);
    }
    bench.minEpochIterations(epoch_iters).run([&blsWorker, &quorumSize, &ids, &members] {
        for (auto& member : members) {
            blsWorker.GenerateContributions(quorumSize / 2 + 1, ids, member.vvec, member.skShares);
        }
    });
    blsWorker.Stop();
}

#define BENCH_GenerateContributions(name, quorumSize, epoch_iters)                          \
    static void BLSDKG_GenerateContributions_##name##_##quorumSize(benchmark::Bench& bench) \
    {                                                                                       \
        BLSDKG_GenerateContributions(bench, epoch_iters, quorumSize);                       \
    }                                                                                       \
    BENCHMARK(BLSDKG_GenerateContributions_##name##_##quorumSize, benchmark::PriorityLevel::HIGH)

static void BLSDKG_InitDKG(benchmark::Bench& bench, uint32_t epoch_iters, int quorumSize)
{
    if (!bench.output()) {
        epoch_iters = 1;
        quorumSize = 1;
    }
    bench.minEpochIterations(epoch_iters).run([&] {
        DKG d(quorumSize, quorumSize / 2 + 1, /*shared=*/false);
    });
}

#define BENCH_InitDKG(name, quorumSize, epoch_iters)                          \
    static void BLSDKG_InitDKG_##name##_##quorumSize(benchmark::Bench& bench) \
    {                                                                         \
        BLSDKG_InitDKG(bench, epoch_iters, quorumSize);                       \
    }                                                                         \
    BENCHMARK(BLSDKG_InitDKG_##name##_##quorumSize, benchmark::PriorityLevel::HIGH)

#define BENCH_BuildQuorumVerificationVectors(name, quorumSize, epoch_iters)                          \
    static void BLSDKG_BuildQuorumVerificationVectors_##name##_##quorumSize(benchmark::Bench& bench) \
    {                                                                                                \
        if (!bench.output()) {                                                                       \
            std::unique_ptr<DKG> ptr = std::make_unique<DKG>(2);                                     \
            ptr->Bench_BuildQuorumVerificationVectors(bench, 1);                                     \
            ptr.reset();                                                                             \
            return;                                                                                  \
        }                                                                                            \
        std::unique_ptr<DKG> ptr = std::make_unique<DKG>(quorumSize);                                \
        ptr->Bench_BuildQuorumVerificationVectors(bench, epoch_iters);                               \
        ptr.reset();                                                                                 \
    }                                                                                                \
    BENCHMARK(BLSDKG_BuildQuorumVerificationVectors_##name##_##quorumSize, benchmark::PriorityLevel::HIGH)

#define BENCH_VerifyContributionShares(name, quorumSize, invalidCount, aggregated, epoch_iters) \
    static void BLSDKG_VerifyContributionShares_##name##_##quorumSize(benchmark::Bench& bench)  \
    {                                                                                           \
        if (!bench.output()) {                                                                  \
            std::unique_ptr<DKG> ptr = std::make_unique<DKG>(2);                                \
            ptr->Bench_VerifyContributionShares(bench, invalidCount, aggregated, 1);            \
            ptr.reset();                                                                        \
            return;                                                                             \
        }                                                                                       \
        std::unique_ptr<DKG> ptr = std::make_unique<DKG>(quorumSize);                           \
        ptr->Bench_VerifyContributionShares(bench, invalidCount, aggregated, epoch_iters);      \
        ptr.reset();                                                                            \
    }                                                                                           \
    BENCHMARK(BLSDKG_VerifyContributionShares_##name##_##quorumSize, benchmark::PriorityLevel::HIGH)

// llmq_400_85
static void BLSDKG_BuildCommitment_cold_400(benchmark::Bench& bench)
{
    DKG(bench.output() ? 400 : 2, bench.output() ? 340 : 2).Bench_BuildCommitment(bench, /*cold=*/true);
}
static void BLSDKG_BuildCommitment_cached_400(benchmark::Bench& bench)
{
    DKG(bench.output() ? 400 : 2, bench.output() ? 340 : 2).Bench_BuildCommitment(bench, /*cold=*/false);
}
static void BLSDKG_VerifyCommitment_400(benchmark::Bench& bench)
{
    DKG(bench.output() ? 400 : 2, bench.output() ? 340 : 2).Bench_VerifyCommitment(bench);
}
BENCHMARK(BLSDKG_BuildCommitment_cold_400, benchmark::PriorityLevel::HIGH)
BENCHMARK(BLSDKG_BuildCommitment_cached_400, benchmark::PriorityLevel::HIGH)
BENCHMARK(BLSDKG_VerifyCommitment_400, benchmark::PriorityLevel::HIGH)

BENCH_GenerateContributions(simple, 50, 10);
BENCH_GenerateContributions(simple, 100, 5);

BENCH_InitDKG(simple, 10, 10)
BENCH_InitDKG(simple, 50, 5)

BENCH_BuildQuorumVerificationVectors(simple, 10, 1000)
BENCH_BuildQuorumVerificationVectors(simple, 100, 10)
BENCH_BuildQuorumVerificationVectors(simple, 400, 1)

BENCH_VerifyContributionShares(simple, 10, 5, false, 100)
BENCH_VerifyContributionShares(simple, 100, 5, false, 1)
BENCH_VerifyContributionShares(simple, 400, 5, false, 1)

// Shares are checked a batch at a time. A batch that holds an invalid share fails as a whole,
// and its shares are then checked one by one to find it.
BENCH_VerifyContributionShares(aggregated_valid, 10, 0, true, 100)
BENCH_VerifyContributionShares(aggregated_valid, 100, 0, true, 1)
BENCH_VerifyContributionShares(aggregated_valid, 400, 0, true, 1)

BENCH_VerifyContributionShares(aggregated_invalid, 10, 5, true, 100)
BENCH_VerifyContributionShares(aggregated_invalid, 100, 5, true, 3)
BENCH_VerifyContributionShares(aggregated_invalid, 400, 5, true, 1)
