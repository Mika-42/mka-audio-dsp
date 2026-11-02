#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <thread>
#include <gtest/gtest.h>
import mka.audio.dsp.plan_switcher;
#include "plan_switcher_fixture.hpp"

namespace {
    using namespace mka::audio::dsp;
    using namespace switcher_test;

    TEST(PlanSwitcherBoundary, TenThousandSwapsRenderExactlyOneGenerationPerBlock) {
        DAG graph; PlanSwitcher switcher;
        const auto id=graph.add<GenerationNode>(switcher); ASSERT_TRUE(id); ASSERT_TRUE(graph.markAsOutput(*id));
        const BackendConfig cfg{.maxFrames=4,.inputChannels=1,.outputChannels=1};
        ASSERT_TRUE(switcher.initialize(graph,cfg)); IO io;
        for (GenerationID generation=2;generation<=10001;++generation) {
            ASSERT_TRUE(switcher.prepareInactivePlan(graph,cfg));
            ASSERT_TRUE(switcher.publishReadyPlan());
            switcher.exec(io.block());
            EXPECT_EQ(switcher.audioGeneration(),generation);
            for (const auto sample : io.output) ASSERT_EQ(sample,static_cast<float>(generation));
            ASSERT_TRUE(switcher.retireAcknowledgedPlan());
        }
        auto* node=static_cast<GenerationNode*>(graph.node(*id));
        EXPECT_TRUE(node->coherent); EXPECT_TRUE(node->noMidBlockSwap);
        EXPECT_EQ(switcher.controlGeneration(),10001U);
    }

    // This test-only Node is deliberately paused to force a publication during
    // process. The pause is harness synchronization, never part of the RT probe.
    class PausedGeneration final : public Node {
    public:
        PausedGeneration(NodeID id,PlanSwitcher& switcher,std::atomic<bool>& entered,std::atomic<bool>& release)
            : Node(id,Config{}),switcher_(switcher),entered_(entered),release_(release) {}
        void process(const ProcessContext& ctx) noexcept override {
            before=switcher_.audioGeneration();
            entered_.store(true,std::memory_order_release);
            while (!release_.load(std::memory_order_acquire)) std::this_thread::yield();
            swapped=switcher_.trySwapAtBlockBoundary();
            for (const auto& output : ctx.backendOutputs) for (FrameCount f=0;f<ctx.frames;++f)
                output.buffer[f]=static_cast<float>(before);
            after=switcher_.audioGeneration();
        }
        GenerationID before=0,after=0; bool swapped=true;
    private:
        PlanSwitcher& switcher_; std::atomic<bool>& entered_; std::atomic<bool>& release_;
    };

    TEST(PlanSwitcherBoundary, PendingPublishedDuringProcessWaitsForTheNextCallback) {
        std::atomic<bool> entered{false},release{false};
        DAG graph; PlanSwitcher switcher;
        const auto id=graph.add<PausedGeneration>(switcher,entered,release); ASSERT_TRUE(id);
        ASSERT_TRUE(graph.markAsOutput(*id));
        const BackendConfig cfg{.maxFrames=4,.inputChannels=1,.outputChannels=1};
        ASSERT_TRUE(switcher.initialize(graph,cfg));
        ASSERT_TRUE(switcher.prepareInactivePlan(graph,cfg)); ASSERT_TRUE(switcher.publishReadyPlan());
        IO io;
        std::thread audio([&] { switcher.exec(io.block()); });
        while (!entered.load(std::memory_order_acquire)) std::this_thread::yield();
        const auto retired=switcher.retireAcknowledgedPlan();
        const auto prepared=retired ? switcher.prepareInactivePlan(graph,cfg) : retired;
        const auto published=prepared ? switcher.publishReadyPlan() : prepared;
        release.store(true,std::memory_order_release);
        audio.join();
        ASSERT_TRUE(retired); ASSERT_TRUE(prepared); ASSERT_TRUE(published);
        const auto* node=static_cast<PausedGeneration*>(graph.node(*id));
        EXPECT_FALSE(node->swapped); EXPECT_EQ(node->before,2U); EXPECT_EQ(node->after,2U);
        for (const auto sample : io.output) EXPECT_EQ(sample,2.F);
        EXPECT_EQ(switcher.handshakeState(),HandshakeState::Pending);
        switcher.exec(io.block());
        EXPECT_EQ(node->before,3U); EXPECT_EQ(node->after,3U);
        for (const auto sample : io.output) EXPECT_EQ(sample,3.F);
        ASSERT_TRUE(switcher.retireAcknowledgedPlan());
    }
}
