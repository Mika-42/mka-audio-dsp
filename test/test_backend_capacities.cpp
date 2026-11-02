#include <array>
#include <concepts>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <gtest/gtest.h>
import mka.audio.dsp.dag;
#include "backend_routing_fixture.hpp"

namespace {
    using namespace mka::audio::dsp;
    using namespace backend_test;
    static_assert(std::is_aggregate_v<BackendConfig> && !std::is_polymorphic_v<BackendConfig>);
    static_assert(std::same_as<decltype(BackendConfig::inputChannels),std::uint32_t>);
    static_assert(std::same_as<decltype(BackendConfig::outputChannels),std::uint32_t>);
    static_assert(std::same_as<decltype(BackendConfig::sampleRate),std::uint32_t>);
    static_assert(makeBackendConfig(6,6,48000,128)->sampleRate==48000);

    TEST(BackendCapacities, FixedZeroOneTwoSixChannelSnapshotsSurviveCallerEditsAndAddressChanges) {
        for (const std::uint32_t count : {0U,1U,2U,6U}) {
            SCOPED_TRACE(count);
            Observation observation; observation.expectOutput=true;
            DAG graph;
            const auto id=graph.add<ProbeNode>(observation); ASSERT_TRUE(id);
            ASSERT_TRUE(graph.markAsOutput(*id));
            BackendConfig cfg{.maxFrames=128,.inputChannels=count,.outputChannels=count,.sampleRate=48000};
            auto plan=graph.compile(cfg); ASSERT_TRUE(plan);
            cfg.sampleRate=96000; cfg.inputChannels=0;
            auto snapshot=plan->backendConfig(); ASSERT_TRUE(snapshot);
            EXPECT_EQ(snapshot->sampleRate,48000U); EXPECT_EQ(snapshot->inputChannels,count);
            snapshot->outputChannels=0;
            EXPECT_EQ(plan->backendConfig()->outputChannels,count);
            FakeBackend first(0.F),second(100.F);
            for (const auto frames : {0U,1U,128U}) for (auto* backend : {&first,&second}) {
                const auto io=backend->io(frames,count,count); IOScope scope(io);
                plan->exec(io);
                EXPECT_EQ(observation.inputs,count); EXPECT_EQ(observation.physicalOutputs,count);
                EXPECT_TRUE(observation.directInputs); EXPECT_TRUE(observation.directOutputs);
                EXPECT_EQ(plan->callbackDiagnostics(),0U);
                for (std::uint32_t c=0;c<count;++c) {
                    for (FrameCount f=0;f<frames;++f) EXPECT_EQ(backend->outputs[c][f],0.F);
                    EXPECT_EQ(backend->outputStorage[c].front(),guard);
                    EXPECT_EQ(backend->outputStorage[c].back(),guard);
                }
            }
        }
    }

    TEST(BackendCapacities, ShapeMismatchAndNullOrOversizedBlocksRejectWithoutProcessingOrWritingPastCapacity) {
        Observation observation;
        DAG graph; const auto id=graph.add<ProbeNode>(observation); ASSERT_TRUE(id);
        auto plan=graph.compile(BackendConfig{.maxFrames=128,.inputChannels=2,.outputChannels=2}); ASSERT_TRUE(plan);
        FakeBackend backend;
        for (const auto counts : {std::array{1U,2U},std::array{2U,1U},std::array{6U,2U},std::array{2U,6U}}) {
            backend.reset();
            plan->exec(backend.io(128,counts[0],counts[1]));
            EXPECT_EQ(plan->takeCallbackDiagnostics(),static_cast<std::uint32_t>(CallbackDiagnostic::ChannelCapacityExceeded));
            EXPECT_EQ(observation.calls,0U);
            for (std::uint32_t c=0;c<counts[1];++c) {
                for (FrameCount f=0;f<128;++f) EXPECT_EQ(backend.outputs[c][f],0.F);
                EXPECT_EQ(backend.outputStorage[c].back(),guard);
            }
        }
        backend.reset(); plan->exec(backend.io(129,2,2));
        EXPECT_EQ(plan->takeCallbackDiagnostics(),static_cast<std::uint32_t>(CallbackDiagnostic::FrameCapacityExceeded));
        EXPECT_EQ(backend.outputStorage[0].back(),guard);
        const std::array<const float*,2> missing{nullptr,nullptr};
        auto io=backend.io(1,2,2); io.inputs=missing;
        plan->exec(io);
        EXPECT_EQ(plan->takeCallbackDiagnostics(),static_cast<std::uint32_t>(CallbackDiagnostic::NullBuffer));
        io.frames=0;
        plan->exec(io); EXPECT_EQ(observation.calls,1U); // Exact shape, no samples read.
        plan->exec(backend.io(0,0,2));
        EXPECT_EQ(plan->takeCallbackDiagnostics(),static_cast<std::uint32_t>(CallbackDiagnostic::ChannelCapacityExceeded));
        EXPECT_EQ(observation.calls,1U);
    }

    TEST(BackendCapacities, WideValuesAreRejectedBeforeAnyNarrowingAndFailedPreparationKeepsTheSnapshot) {
        constexpr std::uint64_t excessive=std::uint64_t{UINT32_MAX}+1;
        const std::array<std::array<std::uint64_t,4>,4> cases{{{excessive,0,0,0},{0,excessive,0,0},
                                                             {0,0,excessive,0},{0,0,0,excessive}}};
        for (const auto values : cases) {
            const auto cfg=makeBackendConfig(values[0],values[1],values[2],values[3]);
            ASSERT_FALSE(cfg); EXPECT_EQ(cfg.error().code,ErrorCode::CapacityExceeded);
        }
        ExecutionPlan unprepared; EXPECT_FALSE(unprepared.backendConfig());
        DAG graph;
        auto plan=graph.compile(BackendConfig{.maxFrames=128,.sampleRate=44100}); ASSERT_TRUE(plan);
        const auto failed=plan->prepareBackend(BackendConfig{.maxFrames=4096,.sampleRate=96000,.memoryBudgetBytes=0},{},{});
        ASSERT_FALSE(failed); EXPECT_EQ(plan->backendConfig()->sampleRate,44100U);
        EXPECT_EQ(plan->backendConfig()->maxFrames,128U);
    }
}
