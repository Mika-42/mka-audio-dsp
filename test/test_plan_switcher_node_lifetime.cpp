/**
 * @file test_plan_switcher_node_lifetime.cpp
 * @brief Tests for plan switcher node lifetime.
 * @author Mika
 */
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <gtest/gtest.h>
import mka.audio.dsp.plan_switcher;
#include "plan_switcher_fixture.hpp"

namespace {
    using namespace mka::audio::dsp;
    using namespace switcher_test;

    /// @brief Test case: external generation keeps A removed node alive after both manager slots release it.
    TEST(PlanSwitcherNodeLifetime, ExternalGenerationKeepsARemovedNodeAliveAfterBothManagerSlotsReleaseIt) {
        LifetimeTrace trace;
        DAG graph; PlanSwitcher switcher;
        const auto id=graph.add<SignalNode>(&trace); ASSERT_TRUE(id); ASSERT_TRUE(graph.markAsOutput(*id));
        const BackendConfig cfg{.maxFrames=4,.inputChannels=1,.outputChannels=1};
        ASSERT_TRUE(switcher.initialize(graph,cfg)); ASSERT_TRUE(switcher.prepareInactivePlan(graph,cfg));
        ExecutionPlan archived=*switcher.readyPlan(); // Copy on control, before callbacks/publication.
        ASSERT_TRUE(switcher.publishReadyPlan()); ASSERT_TRUE(graph.remove(*id));
        IO io; switcher.exec(io.block()); ASSERT_TRUE(switcher.retireAcknowledgedPlan());
        ASSERT_TRUE(switcher.prepareAndPublish(graph,cfg));
        switcher.exec(io.block()); ASSERT_TRUE(switcher.retireAcknowledgedPlan());
        EXPECT_EQ(graph.collectRetiredNodes(),0U); EXPECT_EQ(trace.destroyed.load(),0U);
        archived.exec(io.block()); EXPECT_EQ(io.input,io.output); // Explicit, quiescent control-side use.
        archived=ExecutionPlan{};
        EXPECT_EQ(graph.collectRetiredNodes(),1U); EXPECT_EQ(trace.destroyed.load(),1U);
        EXPECT_EQ(trace.destroyedOnAudio.load(),0U);
    }

    /// @brief Test case: removal with A failed replacement still renders the old node until successful retirement.
    TEST(PlanSwitcherNodeLifetime, RemovalWithAFailedReplacementStillRendersTheOldNodeUntilSuccessfulRetirement) {
        LifetimeTrace trace;
        DAG graph; PlanSwitcher switcher;
        const auto id=graph.add<SignalNode>(&trace); ASSERT_TRUE(id); ASSERT_TRUE(graph.markAsOutput(*id));
        const BackendConfig cfg{.maxFrames=4,.inputChannels=1,.outputChannels=1};
        ASSERT_TRUE(switcher.initialize(graph,cfg));
        ASSERT_TRUE(switcher.prepareInactivePlan(graph,cfg));
        ASSERT_TRUE(graph.remove(*id));
        EXPECT_EQ(graph.collectRetiredNodes(),0U);
        ASSERT_TRUE(switcher.discardReadyPlan()); // A still owns a borrow.
        auto rejected=cfg; rejected.memoryBudgetBytes=0;
        EXPECT_FALSE(switcher.prepareAndPublish(graph,rejected));
        EXPECT_EQ(graph.collectRetiredNodes(),0U); EXPECT_EQ(trace.destroyed.load(),0U);
        IO io; switcher.exec(io.block()); EXPECT_EQ(io.input,io.output);
        ASSERT_TRUE(switcher.prepareAndPublish(graph,cfg));
        switcher.exec(io.block()); EXPECT_EQ(graph.collectRetiredNodes(),0U);
        ASSERT_TRUE(switcher.retireAcknowledgedPlan());
        EXPECT_EQ(graph.collectRetiredNodes(),1U); EXPECT_EQ(trace.destroyed.load(),1U);
    }

    /// @brief Test case: repeated add remove never destroys on audio or reuses live identity.
    TEST(PlanSwitcherNodeLifetime, RepeatedAddRemoveNeverDestroysOnAudioOrReusesLiveIdentity) {
        LifetimeTrace trace;
        DAG graph; PlanSwitcher switcher;
        const BackendConfig cfg{.maxFrames=4,.inputChannels=1,.outputChannels=1};
        ASSERT_TRUE(switcher.initialize(graph,cfg)); IO io; NodeID last=0;
        for (unsigned cycle=0;cycle<32;++cycle) {
            const auto id=graph.add<SignalNode>(&trace); ASSERT_TRUE(id); EXPECT_GT(*id,last); last=*id;
            ASSERT_TRUE(graph.markAsOutput(*id)); ASSERT_TRUE(switcher.prepareAndPublish(graph,cfg));
            onAudio=true; switcher.exec(io.block()); onAudio=false;
            ASSERT_TRUE(switcher.retireAcknowledgedPlan());
            ASSERT_TRUE(graph.remove(*id)); EXPECT_EQ(graph.collectRetiredNodes(),0U);
            ASSERT_TRUE(switcher.prepareAndPublish(graph,cfg));
            onAudio=true; switcher.exec(io.block()); onAudio=false;
            ASSERT_TRUE(switcher.retireAcknowledgedPlan());
            EXPECT_EQ(graph.collectRetiredNodes(),1U);
        }
        EXPECT_EQ(trace.destroyed.load(),32U); EXPECT_EQ(trace.destroyedOnAudio.load(),0U);
        const auto removed=graph.add<SignalNode>(&trace); ASSERT_TRUE(removed);
        ASSERT_TRUE(graph.remove(*removed)); // Never referenced: immediate control destruction.
        EXPECT_EQ(trace.destroyed.load(),33U);
    }
}
