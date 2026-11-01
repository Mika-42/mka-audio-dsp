#include <array>
#include <cstddef>
#include <cstdint>
#include <gtest/gtest.h>
import mka.audio.dsp.dag;
#include "offline_pipeline.hpp"

namespace {
    using namespace mka::audio::dsp;
    using namespace offline_test;

    TEST(OfflinePipeline, OneCompilationRendersExactStereoHalfGainOnVariableBlocksAndFreshBackendBuffers) {
        DAG graph;
        std::array<NodeID,3> ids{}; ASSERT_TRUE(buildPipeline(graph,ids));
        ASSERT_EQ(graph.connections().size(),4U);
        auto plan=graph.compile(BackendConfig{.maxFrames=FakeBackend::maximum,.inputChannels=2,.outputChannels=3}); ASSERT_TRUE(plan);
        const auto* tasks=plan->executionTasks().data();
        const auto* sourceSamples=tasks[0].context.outputs[0].buffer;
        for (auto id : ids) EXPECT_EQ(static_cast<PipelineNode*>(graph.node(id))->calls,0U);
        FakeBackend first{1.F},second{-3.F};
        const auto inputFirst=first.inputStorage, inputSecond=second.inputStorage;
        for (std::size_t channels : {2U,3U})
            for (FrameCount frames : {0U,1U,128U,FakeBackend::maximum})
                for (unsigned block=0;block<2;++block) {
                    auto& current=block ? second : first;
                    auto& previous=block ? first : second;
                    current.resetOutputs(999.F); previous.resetOutputs(-777.F);
                    plan->exec(current.io(frames,channels));
                    for (std::size_t lane=0;lane<3;++lane)
                        for (std::size_t i=0;i<FakeBackend::maximum+2;++i) {
                            float expected=999.F;
                            if (lane<channels && i>0 && i<=frames)
                                expected=lane<2 ? current.inputStorage[lane][i]*0.5F : 0.F;
                            EXPECT_EQ(current.outputStorage[lane][i],expected);
                        }
                    for (const auto& lane : previous.outputStorage)
                        for (const auto sample : lane) EXPECT_EQ(sample,-777.F);
                    for (auto id : ids) EXPECT_EQ(static_cast<PipelineNode*>(graph.node(id))->lastFrames,frames);
                    EXPECT_EQ(plan->executionTasks().data(),tasks);
                    EXPECT_EQ(tasks[0].context.outputs[0].buffer,sourceSamples);
                }
        for (auto id : ids) EXPECT_EQ(static_cast<PipelineNode*>(graph.node(id))->calls,16U);
        EXPECT_EQ(first.inputStorage,inputFirst); EXPECT_EQ(second.inputStorage,inputSecond);
        EXPECT_EQ(plan->callbackDiagnostics(),0U);
    }

    TEST(OfflinePipeline, CapacityFailureIsBoundedAndZeroFrameNullBuffersThenValidAudioRecover) {
        DAG graph;
        std::array<NodeID,3> ids{}; ASSERT_TRUE(buildPipeline(graph,ids));
        auto plan=graph.compile(BackendConfig{.maxFrames=FakeBackend::maximum,.inputChannels=2,.outputChannels=3}); ASSERT_TRUE(plan);
        FakeBackend backend{2.F};
        plan->exec(backend.io(FakeBackend::maximum+1));
        for (auto id : ids) EXPECT_EQ(static_cast<PipelineNode*>(graph.node(id))->calls,0U);
        for (const auto& lane : backend.outputStorage) {
            EXPECT_EQ(lane.front(),999.F); EXPECT_EQ(lane.back(),999.F);
            for (std::size_t i=1;i<=FakeBackend::maximum;++i) EXPECT_EQ(lane[i],0.F);
        }
        const std::array<const float*,2> nullInputs{nullptr,nullptr};
        const std::array<float*,3> nullOutputs{nullptr,nullptr,nullptr};
        plan->exec(mka::audio::AudioProcessContext{nullInputs,nullOutputs,0});
        for (auto id : ids) EXPECT_EQ(static_cast<PipelineNode*>(graph.node(id))->calls,1U);
        EXPECT_EQ(plan->takeCallbackDiagnostics(),static_cast<std::uint32_t>(CallbackDiagnostic::FrameCapacityExceeded));
        backend.resetOutputs(999.F);
        plan->exec(backend.io(1));
        for (auto id : ids) EXPECT_EQ(static_cast<PipelineNode*>(graph.node(id))->calls,2U);
        for (std::size_t lane=0;lane<2;++lane) EXPECT_EQ(backend.outputs[lane][0],backend.inputs[lane][0]*0.5F);
        EXPECT_EQ(backend.outputs[2][0],0.F); EXPECT_EQ(plan->callbackDiagnostics(),0U);
    }
}
