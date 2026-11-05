/**
 * @file test_plan_switcher_retirement.cpp
 * @brief Tests for plan switcher retirement.
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

    /// @brief Test case: no ack means no release and an acknowledged slot is cleared exactly once.
    TEST(PlanSwitcherRetirement, NoAckMeansNoReleaseAndAnAcknowledgedSlotIsClearedExactlyOnce) {
        LifetimeTrace trace;
        DAG graph; PlanSwitcher switcher;
        const auto id=graph.add<SignalNode>(&trace); ASSERT_TRUE(id); ASSERT_TRUE(graph.markAsOutput(*id));
        const BackendConfig cfg{.maxFrames=4,.inputChannels=1,.outputChannels=1};
        ASSERT_TRUE(switcher.initialize(graph,cfg));
        const auto* const address=switcher.slotAddress(0);
        ASSERT_TRUE(graph.remove(*id));
        ASSERT_TRUE(switcher.prepareInactivePlan(graph,cfg)); ASSERT_TRUE(switcher.publishReadyPlan());
        EXPECT_FALSE(switcher.retireAcknowledgedPlan()); EXPECT_EQ(graph.collectRetiredNodes(),0U);
        EXPECT_EQ(trace.destroyed.load(),0U);
        IO io; switcher.exec(io.block());
        EXPECT_EQ(switcher.handshakeState(),HandshakeState::Acknowledged);
        EXPECT_EQ(trace.destroyed.load(),0U); EXPECT_EQ(graph.collectRetiredNodes(),0U);
        ASSERT_TRUE(switcher.retireAcknowledgedPlan());
        EXPECT_EQ(switcher.slotAddress(0),address); EXPECT_TRUE(address->executionTasks().empty());
        EXPECT_EQ(switcher.slotState(0),PlanSlotState::Free);
        EXPECT_EQ(graph.collectRetiredNodes(),1U); EXPECT_EQ(trace.destroyed.load(),1U);
        EXPECT_FALSE(switcher.retireAcknowledgedPlan()); EXPECT_EQ(graph.collectRetiredNodes(),0U);
        EXPECT_EQ(trace.destroyedOnAudio.load(),0U);
    }

    /// @brief Test case: shared node survives the first ack until every generation releases its borrow.
    TEST(PlanSwitcherRetirement, SharedNodeSurvivesTheFirstAckUntilEveryGenerationReleasesItsBorrow) {
        LifetimeTrace trace;
        DAG graph; PlanSwitcher switcher;
        const auto id=graph.add<SignalNode>(&trace); ASSERT_TRUE(id); ASSERT_TRUE(graph.markAsOutput(*id));
        const BackendConfig cfg{.maxFrames=4,.inputChannels=1,.outputChannels=1};
        ASSERT_TRUE(switcher.initialize(graph,cfg));
        ASSERT_TRUE(switcher.prepareInactivePlan(graph,cfg));
        EXPECT_EQ(switcher.slotAddress(0)->tasks()[0],switcher.readyPlan()->tasks()[0]);
        ASSERT_TRUE(switcher.publishReadyPlan()); ASSERT_TRUE(graph.remove(*id));
        IO io; switcher.exec(io.block()); EXPECT_EQ(io.input,io.output);
        ASSERT_TRUE(switcher.retireAcknowledgedPlan());
        EXPECT_EQ(graph.collectRetiredNodes(),0U); EXPECT_EQ(trace.destroyed.load(),0U);
        ASSERT_TRUE(switcher.prepareInactivePlan(graph,cfg)); ASSERT_TRUE(switcher.publishReadyPlan());
        switcher.exec(io.block());
        EXPECT_EQ(io.output,(std::array<float,4>{0,0,0,0}));
        EXPECT_EQ(graph.collectRetiredNodes(),0U);
        ASSERT_TRUE(switcher.retireAcknowledgedPlan());
        EXPECT_EQ(graph.collectRetiredNodes(),1U);
        EXPECT_EQ(trace.destroyed.load(),1U); EXPECT_EQ(trace.destroyedOnAudio.load(),0U);
    }
}
