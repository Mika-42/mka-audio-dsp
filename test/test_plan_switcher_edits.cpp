/**
 * @file test_plan_switcher_edits.cpp
 * @brief Tests for plan switcher edits.
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
    /// @brief Produces a fixed scalar value to distinguish compiled graph generations.
    class ValueNode final : public Node {
    public:
        /// @brief Store the immutable scalar and declare the logical output channel.
        ValueNode(NodeID id,float value) : Node(id,Config{.layoutOutput={0}}),value_(value) {}
        /// @brief Produce the immutable scalar distinguishing this compiled generation.
        void process(const ProcessContext& ctx) noexcept override {
            for (FrameCount f=0;f<ctx.frames;++f) {
                ctx.outputs[0].buffer[f]=value_;
                for (const auto& out : ctx.backendOutputs) out.buffer[f]=value_;
            }
        }
    private:
        /// @brief Immutable scalar signal produced by this processor.
        const float value_;
    };

    /// @brief Test case: pending snapshot stays immutable while desired C changes and its compilation fails.
    TEST(PlanSwitcherEdits, PendingSnapshotStaysImmutableWhileDesiredCChangesAndItsCompilationFails) {
        DAG graph; PlanSwitcher switcher;
        const BackendConfig cfg{.maxFrames=4,.inputChannels=1,.outputChannels=1};
        const auto a=graph.add<ValueNode>(1.F); ASSERT_TRUE(a); ASSERT_TRUE(graph.markAsOutput(*a));
        EXPECT_FALSE(switcher.prepareAndPublish(graph,cfg));
        ASSERT_TRUE(switcher.initialize(graph,cfg)); IO io;
        switcher.exec(io.block()); EXPECT_EQ(io.output[0],1.F);
        const auto b=graph.add<ValueNode>(2.F); ASSERT_TRUE(b); ASSERT_TRUE(graph.markAsOutput(*b));
        ASSERT_TRUE(switcher.prepareAndPublish(graph,cfg));
        const auto c=graph.add<ValueNode>(3.F); ASSERT_TRUE(c); ASSERT_TRUE(graph.markAsOutput(*c));
        const auto busy=switcher.prepareAndPublish(graph,cfg);
        ASSERT_FALSE(busy); EXPECT_EQ(busy.error().code,ErrorCode::PlanBusy);
        switcher.exec(io.block()); EXPECT_EQ(io.output[0],2.F); EXPECT_EQ(switcher.audioGeneration(),2U);
        ASSERT_TRUE(switcher.retireAcknowledgedPlan());
        auto rejected=cfg; rejected.memoryBudgetBytes=0;
        const auto failed=switcher.prepareAndPublish(graph,rejected);
        ASSERT_FALSE(failed); EXPECT_EQ(failed.error().code,ErrorCode::MemoryBudgetExceeded);
        EXPECT_EQ(switcher.handshakeState(),HandshakeState::Idle); EXPECT_EQ(switcher.readyPlan(),nullptr);
        EXPECT_NE(graph.node(*c),nullptr); EXPECT_EQ(graph.outputNodes().size(),3U);
        switcher.exec(io.block()); EXPECT_EQ(io.output[0],2.F); EXPECT_EQ(switcher.audioGeneration(),2U);
        ASSERT_TRUE(switcher.prepareAndPublish(graph,cfg));
        switcher.exec(io.block()); EXPECT_EQ(io.output[0],3.F); EXPECT_EQ(switcher.audioGeneration(),3U);
        ASSERT_TRUE(switcher.retireAcknowledgedPlan());
    }

    /// @brief Test case: shared node configuration cannot be mutated while any generation borrows it.
    TEST(PlanSwitcherEdits, SharedNodeConfigurationCannotBeMutatedWhileAnyGenerationBorrowsIt) {
        DAG graph; PlanSwitcher switcher;
        const auto id=graph.add<SignalNode>(); ASSERT_TRUE(id); ASSERT_TRUE(graph.markAsOutput(*id));
        const BackendConfig cfg{.maxFrames=4,.inputChannels=1,.outputChannels=1};
        ASSERT_TRUE(switcher.initialize(graph,cfg)); ASSERT_TRUE(switcher.prepareAndPublish(graph,cfg));
        const auto changed=graph.setConfig(*id,Config{.layoutOutput={9}});
        ASSERT_FALSE(changed); EXPECT_EQ(changed.error().code,ErrorCode::NodeInUse);
        EXPECT_EQ(graph.node(*id)->config().layoutOutput[0],0U);
        IO io; switcher.exec(io.block()); EXPECT_EQ(io.input,io.output);
        ASSERT_TRUE(switcher.retireAcknowledgedPlan());
        EXPECT_FALSE(graph.setConfig(*id,Config{})); // B still borrows this Node after A's ACK.
    }
}
