#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <thread>
#include <gtest/gtest.h>
#include "switcher_backend_fixture.hpp"
import mka.audio.dsp.plan_switcher;
#include "plan_switcher_fixture.hpp"

namespace {
    using namespace mka::audio::dsp;
    using namespace switcher_test;

    TEST(PlanSwitcherStop, PendingWithoutAnyCallbackIsCanceledWithoutWaitingAndCanResume) {
        DAG graph; PlanSwitcher switcher;
        const auto id=graph.add<SignalNode>(); ASSERT_TRUE(id); ASSERT_TRUE(graph.markAsOutput(*id));
        const BackendConfig cfg{.maxFrames=4,.inputChannels=1,.outputChannels=1};
        ASSERT_TRUE(switcher.initialize(graph,cfg)); ASSERT_TRUE(switcher.prepareAndPublish(graph,cfg));
        ASSERT_TRUE(switcher.stopAfterBackendStopped()); // No backend thread has ever started.
        ASSERT_TRUE(switcher.stopAfterBackendStopped());
        EXPECT_EQ(switcher.handshakeState(),HandshakeState::Idle); EXPECT_EQ(switcher.controlGeneration(),1U);
        EXPECT_EQ(switcher.slotState(1),PlanSlotState::Free);
        IO io; io.output.fill(99.F); switcher.exec(io.block());
        EXPECT_EQ(io.output,(std::array<float,4>{0,0,0,0})); EXPECT_FALSE(switcher.trySwapAtBlockBoundary());
        ASSERT_TRUE(switcher.prepareInactivePlan(graph,cfg));
        EXPECT_FALSE(switcher.publishReadyPlan()); // Stopped: no new publication accepted.
        ASSERT_TRUE(switcher.resumeAfterBackendStopped()); ASSERT_TRUE(switcher.publishReadyPlan());
        switcher.exec(io.block()); EXPECT_EQ(switcher.audioGeneration(),3U); EXPECT_EQ(io.input,io.output);
        ASSERT_TRUE(switcher.retireAcknowledgedPlan());
    }

    TEST(PlanSwitcherStop, StopAfterAdoptionReconcilesAckAndDiscardsUnpublishedReadyOffAudio) {
        DAG graph; PlanSwitcher switcher;
        const auto id=graph.add<SignalNode>(); ASSERT_TRUE(id); ASSERT_TRUE(graph.markAsOutput(*id));
        const BackendConfig cfg{.maxFrames=4,.inputChannels=1,.outputChannels=1};
        ASSERT_TRUE(switcher.initialize(graph,cfg)); ASSERT_TRUE(switcher.prepareAndPublish(graph,cfg));
        IO io; switcher.exec(io.block());
        ASSERT_TRUE(switcher.stopAfterBackendStopped());
        EXPECT_EQ(switcher.controlGeneration(),2U); EXPECT_EQ(switcher.handshakeState(),HandshakeState::Idle);
        EXPECT_EQ(switcher.slotState(0),PlanSlotState::Free);
        ASSERT_TRUE(switcher.prepareInactivePlan(graph,cfg));
        ASSERT_TRUE(switcher.stopAfterBackendStopped()); EXPECT_EQ(switcher.readyPlan(),nullptr);
        ASSERT_TRUE(switcher.shutdownAfterBackendStopped()); ASSERT_TRUE(switcher.shutdownAfterBackendStopped());
        EXPECT_FALSE(switcher.controlGeneration()); EXPECT_FALSE(switcher.resumeAfterBackendStopped());
        ASSERT_TRUE(switcher.initialize(graph,cfg)); EXPECT_EQ(switcher.audioGeneration(),4U);
    }

    class HeldNode final : public Node {
    public:
        HeldNode(NodeID id,std::atomic<bool>& entered,std::atomic<bool>& release,std::atomic<bool>& finished)
            : Node(id,Config{}),entered_(entered),release_(release),finished_(finished) {}
        void process(const ProcessContext&) noexcept override {
            entered_.store(true,std::memory_order_release);
            while (!release_.load(std::memory_order_acquire)) std::this_thread::yield(); // Test harness pause only.
            finished_.store(true,std::memory_order_release);
        }
    private:
        std::atomic<bool>& entered_; std::atomic<bool>& release_; std::atomic<bool>& finished_;
    };

    TEST(PlanSwitcherStop, BackendStopJoinsAnInFlightCallbackBeforeCancelingPending) {
        std::atomic<bool> entered{false},release{false},finished{false},stopped{false};
        DAG graph; PlanSwitcher switcher;
        const auto id=graph.add<HeldNode>(entered,release,finished); ASSERT_TRUE(id);
        const BackendConfig cfg{.maxFrames=4,.inputChannels=1,.outputChannels=1};
        ASSERT_TRUE(switcher.initialize(graph,cfg)); IO io; CallbackThread backend;
        backend.start([&] { switcher.exec(io.block()); });
        while (!entered.load(std::memory_order_acquire)) std::this_thread::yield();
        const auto published=switcher.prepareAndPublish(graph,cfg);
        const auto premature=switcher.stopAfterBackendStopped(); // Missing join is detected while executing.
        std::thread stopper([&] { backend.stop(); stopped.store(true,std::memory_order_release); });
        while (!backend.stopRequested.load(std::memory_order_acquire)) std::this_thread::yield();
        EXPECT_FALSE(stopped.load(std::memory_order_acquire)); EXPECT_FALSE(finished.load(std::memory_order_acquire));
        release.store(true,std::memory_order_release); stopper.join();
        ASSERT_TRUE(published); ASSERT_FALSE(premature); EXPECT_EQ(premature.error().code,ErrorCode::PlanBusy);
        EXPECT_TRUE(finished.load()); ASSERT_TRUE(switcher.stopAfterBackendStopped());
        EXPECT_EQ(switcher.controlGeneration(),1U); EXPECT_EQ(switcher.handshakeState(),HandshakeState::Idle);
        ASSERT_TRUE(switcher.shutdownAfterBackendStopped());
    }
}
