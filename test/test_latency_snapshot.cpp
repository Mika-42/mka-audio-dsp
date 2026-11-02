#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <expected>
#include <span>
#include <thread>
#include <vector>
#include <gtest/gtest.h>
import mka.audio.dsp.plan_switcher;
#include "plan_switcher_fixture.hpp"

namespace {
    using namespace mka::audio::dsp;
    class SnapshotNode final : public Node {
    public:
        SnapshotNode(NodeID id,LatencySamples latency) : Node(id,Config{.layoutOutput={0}}),latency_(latency) {}
        LatencySamples latencySamples() const noexcept override { ++queries; return latency_; }
        void process(const ProcessContext& ctx) noexcept override {
            // Mutable audio-owned state must never be read during capture of B.
            ++blocks_;
            for (FrameCount f=0;f<ctx.frames;++f) {
                ctx.outputs[0].buffer[f]=1.F;
                for (const auto& out : ctx.backendOutputs) out.buffer[f]=1.F;
            }
        }
        void setLatencyAfterQuiescence(LatencySamples latency) { latency_=latency; }
        mutable std::atomic<unsigned> queries{0};
    private:
        LatencySamples latency_;
        unsigned blocks_=0;
    };
    const BackendConfig cfg{.maxFrames=4,.outputChannels=1,.sampleRate=48000};

    TEST(LatencyCapture, DeterministicOwnedTableUsesEachDeclarationOnceAndNeverInExec) {
        DAG graph;
        const auto zero=graph.add<SnapshotNode>(0U),a=graph.add<SnapshotNode>(64U),b=graph.add<SnapshotNode>(256U);
        ASSERT_TRUE(zero); ASSERT_TRUE(a); ASSERT_TRUE(b); ASSERT_TRUE(graph.markAsOutput(*b));
        auto structural=graph.compile(); ASSERT_TRUE(structural);
        EXPECT_EQ(static_cast<SnapshotNode*>(graph.node(*a))->queries.load(),0U);
        ASSERT_TRUE(structural->prepareBackend(cfg,graph.connections(),graph.outputNodes()));
        const auto* snapshot=structural->latencySnapshot(); ASSERT_NE(snapshot,nullptr);
        EXPECT_EQ(snapshot->sampleRate,48000U);
        EXPECT_EQ(snapshot->nodes,(std::vector<IntrinsicLatency>{{*zero,0},{*a,64},{*b,256}}));
        EXPECT_EQ(snapshot->find(*a)->samples,64U); EXPECT_EQ(snapshot->find(999),nullptr);
        auto detached=*snapshot; detached.nodes[1].samples=999;
        EXPECT_EQ(snapshot->find(*a)->samples,64U);
        switcher_test::IO io;
        for (unsigned i=0;i<32;++i) structural->exec(io.block()); // Counts mismatch is also never a latency query.
        std::array<float,4> output{}; const std::array outputs{output.data()};
        for (unsigned i=0;i<32;++i) ASSERT_TRUE(structural->exec(BackendBlock{.outputs=outputs,.frames=4}));
        for (const auto id : {*zero,*a,*b}) EXPECT_EQ(static_cast<SnapshotNode*>(graph.node(id))->queries.load(),1U);
    }

    TEST(LatencyCapture, NewBorrowEpochCapturesChangedDeclarationWithoutEditingOldSnapshot) {
        DAG graph; const auto id=graph.add<SnapshotNode>(64U); ASSERT_TRUE(id);
        auto plan=graph.compile(cfg); ASSERT_TRUE(plan);
        const auto previous=*plan->latencySnapshot();
        *plan=ExecutionPlan{}; // No active or structural plan borrows this Node now.
        static_cast<SnapshotNode*>(graph.node(*id))->setLatencyAfterQuiescence(256);
        plan=graph.compile(cfg); ASSERT_TRUE(plan);
        EXPECT_EQ(previous.find(*id)->samples,64U);
        EXPECT_EQ(plan->latencySnapshot()->find(*id)->samples,256U);
        EXPECT_EQ(static_cast<SnapshotNode*>(graph.node(*id))->queries.load(),2U);
    }

    TEST(LatencyCapture, SharedActiveAndPendingReuseFrozenValueWithoutVirtualDSPAccess) {
        DAG graph; PlanSwitcher switcher; const auto id=graph.add<SnapshotNode>(64U); ASSERT_TRUE(id);
        ASSERT_TRUE(graph.markAsOutput(*id)); ASSERT_TRUE(switcher.initialize(graph,cfg));
        ASSERT_TRUE(switcher.prepareInactivePlan(graph,cfg));
        const auto* b=switcher.readyPlan(); ASSERT_NE(b,nullptr);
        EXPECT_NE(b->latencySnapshot()->nodes.data(),switcher.slotAddress(0)->latencySnapshot()->nodes.data());
        EXPECT_EQ(b->latencySnapshot()->find(*id)->samples,64U);
        ASSERT_TRUE(switcher.publishReadyPlan());
        // Desired C changes while B is immutable. Never mutate the shared DSP.
        ASSERT_TRUE(graph.remove(*id)); const auto c=graph.add<SnapshotNode>(256U); ASSERT_TRUE(c);
        EXPECT_EQ(b->latencySnapshot()->find(*id)->samples,64U);
        EXPECT_EQ(b->latencySnapshot()->find(*c),nullptr);
        EXPECT_EQ(static_cast<SnapshotNode*>(b->tasks()[0])->queries.load(),1U);
        std::array<float,4> output{}; const std::array outputs{output.data()};
        switcher.exec(mka::audio::AudioProcessContext{{},outputs,4,4,4});
        EXPECT_EQ(output[0],1.F); ASSERT_TRUE(switcher.retireAcknowledgedPlan());
    }

    TEST(LatencyCapture, RateChangeWithSharedNodeFailsAndLeavesAExecutable) {
        DAG graph; PlanSwitcher switcher; const auto id=graph.add<SnapshotNode>(64U); ASSERT_TRUE(id);
        ASSERT_TRUE(graph.markAsOutput(*id)); ASSERT_TRUE(switcher.initialize(graph,cfg));
        auto changed=cfg; changed.sampleRate=96000;
        const auto rejected=switcher.prepareAndPublish(graph,changed);
        ASSERT_FALSE(rejected); EXPECT_EQ(rejected.error(),(Error{ErrorCode::NodeInUse,*id}));
        EXPECT_EQ(switcher.handshakeState(),HandshakeState::Idle);
        EXPECT_EQ(switcher.slotAddress(0)->latencySnapshot()->sampleRate,48000U);
        EXPECT_EQ(static_cast<SnapshotNode*>(graph.node(*id))->queries.load(),1U);
        ASSERT_TRUE(switcher.shutdownAfterBackendStopped());
        static_cast<SnapshotNode*>(graph.node(*id))->setLatencyAfterQuiescence(128);
        ASSERT_TRUE(switcher.initialize(graph,changed));
        EXPECT_EQ(switcher.slotAddress(0)->latencySnapshot()->find(*id)->samples,128U);
        EXPECT_EQ(switcher.slotAddress(0)->latencySnapshot()->sampleRate,96000U);
    }

    TEST(LatencyCapture, UncertainUnpreparedSharingRefusesCaptureBeforeQueryingDSP) {
        DAG graph; const auto id=graph.add<SnapshotNode>(64U); ASSERT_TRUE(id);
        auto a=graph.compile(); auto b=graph.compile(); ASSERT_TRUE(a); ASSERT_TRUE(b);
        const auto refused=b->prepareBackend(cfg,graph.connections(),graph.outputNodes());
        ASSERT_FALSE(refused); EXPECT_EQ(refused.error().code,ErrorCode::NodeInUse);
        EXPECT_EQ(b->latencySnapshot(),nullptr);
        EXPECT_EQ(static_cast<SnapshotNode*>(graph.node(*id))->queries.load(),0U);
        *a=ExecutionPlan{};
        ASSERT_TRUE(b->prepareBackend(cfg,graph.connections(),graph.outputNodes()));
    }

    TEST(LatencyCapture, InputOrderDoesNotChangeTableAndMalformedNodesAreRejected) {
        SnapshotNode a{9,256},b{2,64};
        const std::array<Node*,2> reversed{&a,&b};
        const auto snapshot=detail::captureLatencies(reversed,48000); ASSERT_TRUE(snapshot);
        EXPECT_EQ(snapshot->nodes,(std::vector<IntrinsicLatency>{{2,64},{9,256}}));
        const std::array<Node*,2> duplicate{&a,&a};
        EXPECT_EQ(detail::captureLatencies(duplicate,48000).error().code,ErrorCode::InvalidParameter);
        const std::array<Node*,1> missing{nullptr};
        EXPECT_EQ(detail::captureLatencies(missing,48000).error().code,ErrorCode::InvalidParameter);
        a.setLatencyAfterQuiescence(512);
        const auto fresh=detail::captureLatencies(reversed,96000); ASSERT_TRUE(fresh);
        EXPECT_EQ(fresh->find(9)->samples,512U);
        EXPECT_EQ(fresh->sampleRate,96000U);
    }

    TEST(LatencyCapture, ConcurrentCandidatePreparationNeverRequeriesAudioOwnedDSP) {
        DAG graph; PlanSwitcher switcher; const auto id=graph.add<SnapshotNode>(256U); ASSERT_TRUE(id);
        ASSERT_TRUE(graph.markAsOutput(*id)); ASSERT_TRUE(switcher.initialize(graph,cfg));
        std::atomic<bool> running{true}; std::atomic<unsigned> callbacks{0};
        std::thread audio([&] {
            std::array<float,4> output{}; const std::array outputs{output.data()};
            while (running.load(std::memory_order_acquire)) {
                switcher.exec(mka::audio::AudioProcessContext{{},outputs,4,4,4});
                callbacks.fetch_add(1,std::memory_order_relaxed);
            }
        });
        bool prepared=true;
        for (unsigned i=0;i<1000;++i) {
            if (!switcher.prepareInactivePlan(graph,cfg)) { prepared=false; break; }
            if (!switcher.discardReadyPlan()) { prepared=false; break; }
        }
        running.store(false,std::memory_order_release); audio.join();
        EXPECT_TRUE(prepared); EXPECT_GT(callbacks.load(),0U);
        EXPECT_EQ(static_cast<SnapshotNode*>(graph.node(*id))->queries.load(),1U);
        ASSERT_TRUE(switcher.shutdownAfterBackendStopped());
    }
}
