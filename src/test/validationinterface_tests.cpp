// Copyright (c) 2020 The Bitcoin Core developers
// Distributed under the MIT software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include <boost/test/unit_test.hpp>
#include <consensus/validation.h>
#include <dbwrapper.h>
#include <evo/evodb.h>
#include <evo/snapshot_types.h>
#include <init.h>
#include <node/context.h>
#include <primitives/block.h>
#include <scheduler.h>
#include <test/util/setup_common.h>
#include <util/check.h>
#include <validationinterface.h>

#include <chrono>
#include <future>

BOOST_FIXTURE_TEST_SUITE(validationinterface_tests, ChainTestingSetup)

struct TestSubscriberNoop final : public CValidationInterface {
    void BlockChecked(const CBlock&, const BlockValidationState&) override {}
};

BOOST_AUTO_TEST_CASE(unregister_validation_interface_race)
{
    std::atomic<bool> generate{true};

    // Start thread to generate notifications
    std::thread gen{[&] {
        const CBlock block_dummy;
        BlockValidationState state_dummy;
        while (generate) {
            GetMainSignals().BlockChecked(block_dummy, state_dummy);
        }
    }};

    // Start thread to consume notifications
    std::thread sub{[&] {
        // keep going for about 1 sec, which is 250k iterations
        for (int i = 0; i < 250000; i++) {
            auto sub = std::make_shared<TestSubscriberNoop>();
            RegisterSharedValidationInterface(sub);
            UnregisterSharedValidationInterface(sub);
        }
        // tell the other thread we are done
        generate = false;
    }};

    gen.join();
    sub.join();
    BOOST_CHECK(!generate);
}

class TestInterface : public CValidationInterface
{
public:
    TestInterface(std::function<void()> on_call = nullptr, std::function<void()> on_destroy = nullptr)
        : m_on_call(std::move(on_call)), m_on_destroy(std::move(on_destroy))
    {
    }
    virtual ~TestInterface()
    {
        if (m_on_destroy) m_on_destroy();
    }
    void BlockChecked(const CBlock& block, const BlockValidationState& state) override
    {
        if (m_on_call) m_on_call();
    }
    void ChainStateFlushed(const CBlockLocator&) override
    {
        if (m_on_call) m_on_call();
    }
    static void Call()
    {
        CBlock block;
        BlockValidationState state;
        GetMainSignals().BlockChecked(block, state);
    }
    std::function<void()> m_on_call;
    std::function<void()> m_on_destroy;
};

// Regression test to ensure UnregisterAllValidationInterfaces calls don't
// destroy a validation interface while it is being called. Bug:
// https://github.com/bitcoin/bitcoin/pull/18551
BOOST_AUTO_TEST_CASE(unregister_all_during_call)
{
    bool destroyed = false;
    RegisterSharedValidationInterface(std::make_shared<TestInterface>(
        [&] {
            // First call should decrements reference count 2 -> 1
            UnregisterAllValidationInterfaces();
            BOOST_CHECK(!destroyed);
            // Second call should not decrement reference count 1 -> 0
            UnregisterAllValidationInterfaces();
            BOOST_CHECK(!destroyed);
        },
        [&] { destroyed = true; }));
    TestInterface::Call();
    BOOST_CHECK(destroyed);
}

// A local Evo state error in one subscriber of a queued event keeps the event
// from none of the others. A synchronous signal passes it on to its caller.
// Either way the failing subscriber can still be unregistered.
BOOST_AUTO_TEST_CASE(local_evo_error_in_subscriber)
{
    int failing_calls{0};
    int other_calls{0};
    auto failing{std::make_shared<TestInterface>([&] {
        ++failing_calls;
        throw BlockDataUnavailableError{"test"};
    })};
    auto other{std::make_shared<TestInterface>([&] { ++other_calls; })};
    RegisterSharedValidationInterface(failing);
    RegisterSharedValidationInterface(other);
    const auto queued{[] {
        GetMainSignals().ChainStateFlushed(CBlockLocator{});
        SyncWithValidationInterfaceQueue();
    }};

    queued();
    BOOST_CHECK_EQUAL(failing_calls, 1);
    BOOST_CHECK_EQUAL(other_calls, 1);
    BOOST_CHECK_THROW(TestInterface::Call(), BlockDataUnavailableError);
    BOOST_CHECK_EQUAL(failing_calls, 2);

    UnregisterSharedValidationInterface(failing);
    BOOST_CHECK_NO_THROW(TestInterface::Call());
    queued();
    BOOST_CHECK_EQUAL(failing_calls, 2);
    BOOST_CHECK_EQUAL(other_calls, 3);
    UnregisterSharedValidationInterface(other);
}

// A chainstate load retry releases the Evo database only after the validation
// events the failed attempt queued, bound to that database, have run.
BOOST_AUTO_TEST_CASE(load_retry_runs_queued_events_first)
{
    node::NodeContext node;
    node.evodb = std::make_unique<CEvoDB>(util::DbWrapperParams{.path = {}, .memory = true, .wipe = true});
    bool evodb_alive_in_callback{false};
    auto subscriber{std::make_shared<TestInterface>([&] { evodb_alive_in_callback = node.evodb != nullptr; })};
    RegisterSharedValidationInterface(subscriber);

    // Hold the queue, so the event is still pending when the retry starts.
    std::promise<void> release_queue;
    const std::shared_future<void> queue_released{release_queue.get_future().share()};
    m_node.scheduler->scheduleFromNow([queue_released] { queue_released.wait(); }, std::chrono::milliseconds{0});
    {
        const EvoViewScope bound{node.evodb->ViewFor(EvoDbIdentity::NORMAL)};
        GetMainSignals().ChainStateFlushed(CBlockLocator{});
    }
    auto retry{std::async(std::launch::async, [&] { ReleaseChainstateLoadAttempt(node); })};
    retry.wait_for(std::chrono::milliseconds{200});
    release_queue.set_value();
    retry.get();
    BOOST_CHECK(evodb_alive_in_callback);
    BOOST_CHECK(!node.evodb);
    UnregisterSharedValidationInterface(subscriber);
}

BOOST_AUTO_TEST_SUITE_END()
