#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <thread>
#include <type_traits>
#include <gtest/gtest.h>
import mka.audio.dsp.plan_switcher;
#include "plan_switcher_fixture.hpp"

namespace {
    using namespace mka::audio::dsp;
    using namespace switcher_test;
    static_assert(std::is_same_v<std::underlying_type_t<HandshakeState>,std::uint8_t>);

    TEST(PlanSwitcherHandshake, IdlePendingAcknowledgedIdleKeepsIndicesAndSlotOwnershipCoherent) {
        DAG graph; PlanSwitcher switcher;
        const auto id=graph.add<SignalNode>(); ASSERT_TRUE(id); ASSERT_TRUE(graph.markAsOutput(*id));
        const BackendConfig cfg{.maxFrames=4,.inputChannels=1,.outputChannels=1};
        EXPECT_TRUE(switcher.lockFreeAtomics()); EXPECT_FALSE(switcher.publishReadyPlan());
        ASSERT_TRUE(switcher.initialize(graph,cfg));
        EXPECT_EQ(switcher.controlGeneration(),1U); EXPECT_FALSE(switcher.trySwapAtBlockBoundary());
        EXPECT_FALSE(switcher.publishReadyPlan());
        ASSERT_TRUE(switcher.prepareInactivePlan(graph,cfg));
        ASSERT_TRUE(switcher.publishReadyPlan());
        EXPECT_EQ(switcher.handshakeState(),HandshakeState::Pending);
        EXPECT_FALSE(switcher.controlGeneration()); EXPECT_EQ(switcher.readyPlan(),nullptr);
        EXPECT_FALSE(switcher.publishReadyPlan()); EXPECT_FALSE(switcher.prepareInactivePlan(graph,cfg));
        EXPECT_FALSE(switcher.discardReadyPlan()); EXPECT_FALSE(switcher.retireAcknowledgedPlan());
        EXPECT_EQ(switcher.slotState(0),PlanSlotState::Active);
        EXPECT_EQ(switcher.slotState(1),PlanSlotState::Pending);
        ASSERT_TRUE(switcher.trySwapAtBlockBoundary());
        EXPECT_EQ(switcher.handshakeState(),HandshakeState::Acknowledged);
        EXPECT_EQ(switcher.audioGeneration(),2U); EXPECT_EQ(switcher.controlGeneration(),2U);
        EXPECT_FALSE(switcher.publishReadyPlan()); EXPECT_FALSE(switcher.prepareInactivePlan(graph,cfg));
        ASSERT_TRUE(switcher.retireAcknowledgedPlan());
        EXPECT_EQ(switcher.handshakeState(),HandshakeState::Idle);
        EXPECT_EQ(switcher.slotState(0),PlanSlotState::Free); EXPECT_EQ(switcher.slotState(1),PlanSlotState::Active);
        EXPECT_FALSE(switcher.retireAcknowledgedPlan());
        ASSERT_TRUE(switcher.prepareInactivePlan(graph,cfg)); EXPECT_EQ(switcher.readyGeneration(),3U);
    }

    TEST(PlanSwitcherHandshake, ReleaseAcquirePublishesCompleteDescriptorsAndRetiresOnlyAfterAck) {
        DAG graph; PlanSwitcher switcher;
        std::array<NodeID,2> ids; Connection edge;
        ASSERT_TRUE(makeDelayedGraph(graph,ids,edge));
        const BackendConfig cfg{.maxFrames=4,.inputChannels=1,.outputChannels=1};
        ASSERT_TRUE(switcher.initialize(graph,cfg));
        std::atomic<bool> run{false};
        IO io;
        bool swapped=false;
        std::thread audio([&] {
            while (!run.load(std::memory_order_acquire)) std::this_thread::yield(); // Harness only.
            swapped=switcher.trySwapAtBlockBoundary();
            switcher.exec(io.block());
        });
        const std::array delays{ConnectionDelay{edge,3}};
        auto next=cfg; next.connectionDelays=delays;
        const auto prepared=switcher.prepareInactivePlan(graph,next);
        const auto published=prepared ? switcher.publishReadyPlan() : prepared;
        run.store(true,std::memory_order_release);
        audio.join();
        ASSERT_TRUE(prepared); ASSERT_TRUE(published); EXPECT_TRUE(swapped);
        EXPECT_EQ(io.output,(std::array<float,4>{0,0,0,1}));
        ASSERT_TRUE(switcher.retireAcknowledgedPlan());
        EXPECT_EQ(switcher.controlGeneration(),2U);
    }
}
