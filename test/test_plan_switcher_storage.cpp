#include <array>
#include <atomic>
#include <expected>
#include <span>
#include <type_traits>
#include <gtest/gtest.h>
import mka.audio.dsp.plan_switcher;
#include "plan_switcher_fixture.hpp"

namespace {
    using namespace mka::audio::dsp;
    using namespace switcher_test;
    static_assert(std::is_final_v<PlanSwitcher> && !std::is_polymorphic_v<PlanSwitcher>);
    static_assert(!std::is_move_constructible_v<PlanSwitcher> && !std::is_copy_constructible_v<PlanSwitcher>);

    TEST(PlanSwitcherStorage, StableSlotsHaveIndependentDescriptorsSamplesAndDelayHistories) {
        DAG graph; PlanSwitcher switcher;
        std::array<NodeID,2> ids; Connection edge;
        ASSERT_TRUE(makeDelayedGraph(graph,ids,edge));
        const std::array delayA{ConnectionDelay{edge,3}},delayB{ConnectionDelay{edge,256}};
        BackendConfig cfg{.maxFrames=4,.inputChannels=1,.outputChannels=1,.connectionDelays=delayA};
        const auto* a=switcher.slotAddress(0); const auto* b=switcher.slotAddress(1);
        ASSERT_NE(a,b); ASSERT_TRUE(switcher.initialize(graph,cfg));
        cfg.connectionDelays=delayB;
        ASSERT_TRUE(switcher.prepareInactivePlan(graph,cfg));
        ASSERT_EQ(switcher.readyPlan(),b);
        EXPECT_NE(a->executionTasks().data(),b->executionTasks().data());
        EXPECT_NE(a->executionTasks()[0].context.outputs[0].buffer,b->executionTasks()[0].context.outputs[0].buffer);
        EXPECT_EQ(a->preparedMemoryUsage().historyBytes,12U);
        EXPECT_EQ(b->preparedMemoryUsage().historyBytes,1024U);
        EXPECT_EQ(a->tasks()[0],b->tasks()[0]); // Nodes are borrowed, storage is not shared.
        auto* const samples=a->executionTasks()[0].context.outputs[0].buffer;
        IO io;
        for (unsigned callback=0;callback<16;++callback) {
            switcher.exec(io.block());
            EXPECT_EQ(switcher.audioGeneration(),1U);
            EXPECT_EQ(switcher.slotAddress(0),a); EXPECT_EQ(switcher.slotAddress(1),b);
            EXPECT_EQ(a->executionTasks()[0].context.outputs[0].buffer,samples);
        }
        EXPECT_EQ(switcher.slotState(0),PlanSlotState::Active);
        EXPECT_EQ(switcher.slotState(1),PlanSlotState::Ready);
        ASSERT_TRUE(switcher.discardReadyPlan());
        EXPECT_EQ(switcher.slotState(1),PlanSlotState::Free);
    }

    TEST(PlanSwitcherStorage, FailedCandidateAndBusySlotPreserveTheActiveGeneration) {
        DAG graph; PlanSwitcher switcher;
        const auto id=graph.add<SignalNode>(); ASSERT_TRUE(id); ASSERT_TRUE(graph.markAsOutput(*id));
        const BackendConfig cfg{.maxFrames=4,.inputChannels=1,.outputChannels=1};
        EXPECT_FALSE(switcher.prepareInactivePlan(graph,cfg));
        ASSERT_TRUE(switcher.initialize(graph,cfg));
        auto* samples=switcher.slotAddress(0)->executionTasks()[0].context.outputs[0].buffer;
        auto rejected=cfg; rejected.memoryBudgetBytes=0;
        const auto failed=switcher.prepareInactivePlan(graph,rejected);
        ASSERT_FALSE(failed); EXPECT_EQ(failed.error().code,ErrorCode::MemoryBudgetExceeded);
        EXPECT_EQ(switcher.slotState(1),PlanSlotState::Free);
        EXPECT_EQ(switcher.slotAddress(0)->executionTasks()[0].context.outputs[0].buffer,samples);
        IO io; switcher.exec(io.block()); EXPECT_EQ(io.input,io.output);
        ASSERT_TRUE(switcher.prepareInactivePlan(graph,cfg));
        EXPECT_FALSE(switcher.prepareInactivePlan(graph,cfg));
        EXPECT_FALSE(switcher.initialize(graph,cfg));
        EXPECT_EQ(switcher.readyGeneration(),2U);
    }
}
