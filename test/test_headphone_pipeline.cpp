#include <array>
#include <expected>
#include <gtest/gtest.h>
import mka.audio.dsp.dag;
#include "headphone_pipeline_fixture.hpp"

namespace {
    using namespace mka::audio::dsp;
    using namespace backend_test;

    TEST(HeadphonePipeline, PhysicalInputsThroughGainReachTwoPairsWithPerSampleLastWriterAndFreshPointers) {
        std::array<NodeID,4> ids;
        std::array<PairRouting,4> routing;
        std::array<Observation,4> seen;
        DAG graph; ASSERT_TRUE(buildHeadphonePipeline(graph,ids,routing,seen));
        auto plan=graph.compile(BackendConfig{.maxFrames=128,.inputChannels=6,.outputChannels=6,.sampleRate=48000});
        ASSERT_TRUE(plan);
        EXPECT_EQ(plan->taskInfos().size(),4U); EXPECT_EQ(graph.connections().size(),6U);
        EXPECT_EQ(plan->sourceTasks().size(),1U); EXPECT_EQ(plan->outputTasks().size(),3U);
        for (const auto& edge : graph.connections()) {
            EXPECT_NE(decodeNode(edge.from),invalidNodeID); EXPECT_NE(decodeNode(edge.to),invalidNodeID);
            EXPECT_EQ(decodePort(edge.from),PortType::Out); EXPECT_EQ(decodePort(edge.to),PortType::In);
        }
        const auto* tasks=plan->executionTasks().data();
        std::array<std::array<const float*,2>,4> internal{};
        for (unsigned t=0;t<4;++t) for (unsigned c=0;c<2;++c) internal[t][c]=tasks[t].context.outputs[c].buffer;
        FakeBackend a(1.F),b(1000.F);
        const auto originalA=a.inputStorage,originalB=b.inputStorage;
        for (FrameCount frames : {0U,1U,2U,7U,128U,1U,128U}) for (auto* backend : {&a,&b}) {
            const auto before=backend->outputStorage;
            const auto io=backend->io(frames,6,6); IOScope scope(io); plan->exec(io);
            for (unsigned c=0;c<6;++c) for (FrameCount f=0;f<130;++f) {
                const auto expected=f>0 && f<=frames ? headphoneSample(c,f-1,*backend) : before[c][f];
                EXPECT_EQ(backend->outputStorage[c][f],expected) << "physical channel=" << c << " offset=" << f;
            }
            EXPECT_EQ(plan->executionTasks().data(),tasks);
            for (unsigned t=0;t<4;++t) {
                EXPECT_EQ(seen[t].physicalOutputs,t==1 ? 0U : 6U);
                EXPECT_TRUE(seen[t].directInputs); EXPECT_TRUE(seen[t].directOutputs); EXPECT_TRUE(seen[t].separateStorage);
                EXPECT_TRUE(tasks[t].context.backendOutputs.empty());
                for (unsigned c=0;c<2;++c) EXPECT_EQ(tasks[t].context.outputs[c].buffer,internal[t][c]);
            }
        }
        EXPECT_EQ(a.inputStorage,originalA); EXPECT_EQ(b.inputStorage,originalB);
        EXPECT_EQ(plan->callbackDiagnostics(),0U);
    }

    TEST(HeadphonePipeline, MalformedBlocksRemainBoundedAndTheSamePreparedPlanRecovers) {
        std::array<NodeID,4> ids;
        std::array<PairRouting,4> routing;
        std::array<Observation,4> seen;
        DAG graph; ASSERT_TRUE(buildHeadphonePipeline(graph,ids,routing,seen));
        auto plan=graph.compile(BackendConfig{.maxFrames=128,.inputChannels=6,.outputChannels=6}); ASSERT_TRUE(plan);
        FakeBackend backend;
        const auto runRejected=[&](const mka::audio::AudioProcessContext& io,CallbackDiagnostic error,FrameCount cleared) {
            backend.reset();
            plan->exec(io);
            EXPECT_EQ(plan->takeCallbackDiagnostics(),static_cast<std::uint32_t>(error));
            for (const auto& o : seen) EXPECT_EQ(o.calls,0U);
            for (unsigned c=0;c<6;++c) for (FrameCount f=0;f<130;++f)
                EXPECT_EQ(backend.outputStorage[c][f],c<io.outputs.size() && f>0 && f<=cleared ? 0.F : guard);
        };
        runRejected(backend.io(129,6,6),CallbackDiagnostic::FrameCapacityExceeded,128);
        runRejected(backend.io(8,5,6),CallbackDiagnostic::ChannelCapacityExceeded,8);
        runRejected(backend.io(8,6,5),CallbackDiagnostic::ChannelCapacityExceeded,8);
        auto shortOutput=backend.io(8,6,6); shortOutput.outputCapacityFrames=3;
        runRejected(shortOutput,CallbackDiagnostic::BufferCapacityExceeded,3);
        auto shortInput=backend.io(8,6,6); shortInput.inputCapacityFrames=3;
        runRejected(shortInput,CallbackDiagnostic::BufferCapacityExceeded,8);
        auto missing=backend.inputs; missing[5]=nullptr;
        auto bad=backend.io(8,6,6); bad.inputs=missing;
        runRejected(bad,CallbackDiagnostic::NullBuffer,8);
        const std::array<const float*,6> nullInputs{};
        const std::array<float*,6> nullOutputs{};
        const mka::audio::AudioProcessContext empty{nullInputs,nullOutputs,0};
        { const IOScope scope(empty); plan->exec(empty); }
        for (const auto& o : seen) EXPECT_EQ(o.calls,1U);
        const auto valid=backend.io(128,6,6);
        { const IOScope scope(valid); plan->exec(valid); }
        for (const auto& o : seen) EXPECT_EQ(o.calls,2U);
        for (unsigned c=0;c<6;++c) for (FrameCount f=0;f<128;++f)
            EXPECT_EQ(backend.outputs[c][f],headphoneSample(c,f,backend));
        EXPECT_EQ(plan->callbackDiagnostics(),0U);
    }
}
