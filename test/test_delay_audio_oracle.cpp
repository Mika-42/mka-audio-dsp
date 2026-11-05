/**
 * @file test_delay_audio_oracle.cpp
 * @brief Tests for delay audio oracle.
 * @author Mika
 */
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <thread>
#include <vector>
#include <gtest/gtest.h>
import mka.audio.dsp.plan_switcher;
#include "delay_audio_fixture.hpp"

namespace {
    using namespace mka::audio::dsp;
    using namespace audio_pdc_test;
    /// @brief Owns the sample buffers and observed results of one oracle rendering.
    struct Render {
        /// @brief Captured physical output samples for independent rendering comparisons.
        std::array<std::vector<float>,2> physical;
        /// @brief Captured logical signals, separate from arbitrary physical routing.
        std::vector<std::vector<float>> logical;
        /// @brief Number of sample slots retained by the rendered plan.
        std::uint32_t slots=0;
    };
    /// @brief Render a graph scenario with the chosen block partition and retain physical and logical
    /// samples.
    void run(Scenario scenario,SlotStrategy strategy,std::span<const FrameCount> sizes,Render& result) {
        Matrix model; ASSERT_TRUE(model.make(scenario));
        const BackendConfig cfg{.maxFrames=128,.inputChannels=2,.outputChannels=2,.sampleRate=48000,.slotStrategy=strategy};
        auto plan=model.graph.compile(cfg); ASSERT_TRUE(plan);
        for (unsigned lane=0;lane<2;++lane) {
            EXPECT_EQ(plan->pdcAnalysis()->find(model.mainEdge(lane))->samples,model.mainDelay());
            EXPECT_EQ(plan->pdcAnalysis()->find(model.secondaryEdge(lane))->samples,model.secondaryDelay());
        }
        EXPECT_EQ(plan->delayTaskCount(),scenario==Scenario::Zero ? 0U : 4U);
        EXPECT_EQ(plan->preparedMemoryUsage().historyBytes,2*(model.mainDelay()+model.secondaryDelay())*sizeof(float));
        constexpr std::size_t total=1536;
        for (auto& channel : result.physical) channel.resize(total); result.slots=plan->bufferSlotCount();
        using Samples=std::array<std::array<float,130>,2>;
        std::array<Samples,2> input{},output{};
        std::size_t n=0,block=0;
        while (n<total) {
            const auto bank=block%2;
            const auto frames=static_cast<FrameCount>(std::min<std::size_t>(sizes[block%sizes.size()],total-n)); ++block;
            std::array<const float*,2> inputs{}; std::array<float*,2> outputs{};
            for (unsigned lane=0;lane<2;++lane) {
                input[bank][lane].fill(-999.F); output[bank][lane].fill(-999.F);
                for (FrameCount f=0;f<frames;++f) input[bank][lane][f+1]=inputSample(lane,n+f);
                inputs[lane]=input[bank][lane].data()+1; outputs[lane]=output[bank][lane].data()+1;
            }
            const auto before=input[bank]; const auto oldOutput=output[1-bank];
            plan->exec(mka::audio::AudioProcessContext{inputs,outputs,frames,128,128});
            EXPECT_EQ(input[bank],before); EXPECT_EQ(output[1-bank],oldOutput);
            for (unsigned lane=0;lane<2;++lane) {
                for (FrameCount f=0;f<frames;++f) {
                    const auto expected=expectedSample(lane,n+f,model.pb,2.F);
                    EXPECT_EQ(output[bank][lane][f+1],expected)<<"physical lane="<<lane<<" n="<<n+f;
                    result.physical[lane][n+f]=output[bank][lane][f+1];
                }
                EXPECT_EQ(output[bank][lane].front(),-999.F);
                for (std::size_t f=frames+1;f<130;++f) EXPECT_EQ(output[bank][lane][f],-999.F);
            }
            n+=frames;
        }
        for (const auto& ref : model.references) {
            SCOPED_TRACE(::testing::Message()<<"NodeID="<<ref.id<<" lane="<<ref.lane<<" P="<<ref.intrinsic<<" L="<<ref.outDelay);
            const auto& processor=model.processor(ref.id);
            EXPECT_EQ(processor.queries,1U); EXPECT_EQ(processor.seen,total);
            EXPECT_EQ(plan->latencySnapshot()->find(ref.id)->samples,ref.intrinsic);
            EXPECT_EQ(plan->pdcAnalysis()->find(ref.id)->cumulative,ref.outDelay);
            for (std::size_t sample=0;sample<total;++sample) {
                EXPECT_EQ(processor.outputLog[sample],expectedSample(ref.lane,sample,ref.outDelay,ref.outGain))<<"output n="<<sample;
                EXPECT_EQ(processor.mainLog[sample],expectedSample(ref.lane,sample,ref.mainDelay,ref.mainGain))<<"In n="<<sample;
                EXPECT_EQ(processor.auxLog[sample],expectedSample(ref.lane,sample,ref.auxDelay,ref.auxGain))<<"Aux n="<<sample;
            }
            result.logical.emplace_back(processor.outputLog.begin(),processor.outputLog.begin()+total);
        }
        EXPECT_EQ(plan->callbackDiagnostics(),0U);
    }

    /// @brief Test case: P01 P02 P03 stereo mono paths match every internal and physical impulse sample.
    TEST(DelayAudioPdc, P01P02P03StereoMonoPathsMatchEveryInternalAndPhysicalImpulseSample) {
        const std::array<FrameCount,7> sizes{0,1,2,7,128,0,7};
        for (const auto scenario : {Scenario::Zero,Scenario::Parallel,Scenario::Chain}) {
            SCOPED_TRACE(static_cast<int>(scenario)); Render result;
            ASSERT_NO_FATAL_FAILURE(run(scenario,SlotStrategy::Reuse,sizes,result));
        }
    }

    /// @brief Test case: fixed variable and non recycling renders are identical across every callback partition.
    TEST(DelayAudioPdc, FixedVariableAndNonRecyclingRendersAreIdenticalAcrossEveryCallbackPartition) {
        const std::array<FrameCount,1> fixed{128};
        const std::array<FrameCount,6> variable{1,0,2,7,128,0};
        for (const auto scenario : {Scenario::Zero,Scenario::Parallel,Scenario::Chain}) {
            SCOPED_TRACE(static_cast<int>(scenario)); Render a,b,c;
            ASSERT_NO_FATAL_FAILURE(run(scenario,SlotStrategy::Reuse,fixed,a));
            ASSERT_NO_FATAL_FAILURE(run(scenario,SlotStrategy::Reuse,variable,b));
            ASSERT_NO_FATAL_FAILURE(run(scenario,SlotStrategy::Unique,variable,c));
            EXPECT_EQ(a.physical,b.physical); EXPECT_EQ(a.physical,c.physical);
            EXPECT_EQ(a.logical,b.logical); EXPECT_EQ(a.logical,c.logical); EXPECT_LT(a.slots,c.slots);
        }
    }

    /// @brief Test case: P04 P05 three samples render the exact two reference blocks through A compiled plan.
    TEST(DelayAudioPdc, P04P05ThreeSamplesRenderTheExactTwoReferenceBlocksThroughACompiledPlan) {
        DAG graph; const auto source=graph.add<MonoPlugin>(0U,0U),sink=graph.add<MonoPlugin>(0U,noLane,0U);
        ASSERT_TRUE(source); ASSERT_TRUE(sink); const auto connection=edge(*source,*sink);
        ASSERT_TRUE(graph.connect(connection.to,connection.from)); ASSERT_TRUE(graph.markAsOutput(*sink));
        const std::array delay{ConnectionDelay{connection,3}};
        auto plan=graph.compile(BackendConfig{.maxFrames=4,.inputChannels=1,.outputChannels=1,.connectionDelays=delay}); ASSERT_TRUE(plan);
        std::array<float,4> input{1,2,3,4},output{};
        const std::array<const float*,1> inputs{input.data()}; const std::array outputs{output.data()};
        ASSERT_TRUE(plan->exec(BackendBlock{inputs,outputs,4})); EXPECT_EQ(output,(std::array<float,4>{0,0,0,1}));
        ASSERT_TRUE(plan->exec(BackendBlock{inputs,outputs,0}));
        input={5,6,7,8}; ASSERT_TRUE(plan->exec(BackendBlock{inputs,outputs,4}));
        EXPECT_EQ(output,(std::array<float,4>{2,3,4,5}));
    }

    /// @brief Test case: errors preserve A and boundary publication starts B with its own zero filled history.
    TEST(DelayAudioPdc, ErrorsPreserveAAndBoundaryPublicationStartsBWithItsOwnZeroFilledHistory) {
        DAG graph; PlanSwitcher switcher;
        const auto source=graph.add<MonoPlugin>(0U,0U),sink=graph.add<MonoPlugin>(0U,noLane,0U);
        ASSERT_TRUE(source); ASSERT_TRUE(sink); const auto connection=edge(*source,*sink);
        ASSERT_TRUE(graph.connect(connection.to,connection.from)); ASSERT_TRUE(graph.markAsOutput(*sink));
        const std::array delay{ConnectionDelay{connection,3}},huge{ConnectionDelay{connection,UINT64_MAX}};
        const BackendConfig cfg{.maxFrames=4,.inputChannels=1,.outputChannels=1,.connectionDelays=delay};
        ASSERT_TRUE(switcher.initialize(graph,cfg));
        std::array<float,4> input{1,2,3,4},output{};
        const std::array<const float*,1> inputs{input.data()}; const std::array outputs{output.data()};
        const auto callback=[&] { switcher.exec(mka::audio::AudioProcessContext{inputs,outputs,4,4,4}); };
        callback(); EXPECT_EQ(output,(std::array<float,4>{0,0,0,1}));
        auto invalid=cfg; invalid.connectionDelays=huge;
        EXPECT_EQ(switcher.prepareAndPublish(graph,invalid).error().code,ErrorCode::ArithmeticOverflow);
        invalid=cfg; invalid.memoryBudgetBytes=0;
        EXPECT_EQ(switcher.prepareAndPublish(graph,invalid).error().code,ErrorCode::MemoryBudgetExceeded);
        EXPECT_EQ(switcher.handshakeState(),HandshakeState::Idle);
        ASSERT_TRUE(switcher.prepareInactivePlan(graph,cfg)); // Ready B does not reset A.
        input={5,6,7,8}; callback(); EXPECT_EQ(output,(std::array<float,4>{2,3,4,5}));
        EXPECT_EQ(switcher.audioGeneration(),1U); ASSERT_TRUE(switcher.publishReadyPlan());
        input={9,10,11,12}; callback(); EXPECT_EQ(output,(std::array<float,4>{0,0,0,9}));
        EXPECT_EQ(switcher.audioGeneration(),2U); ASSERT_TRUE(switcher.retireAcknowledgedPlan());
        input={13,14,15,16}; callback(); EXPECT_EQ(output,(std::array<float,4>{10,11,12,13}));
    }

    /// @brief Test case: concurrent automatic stereo candidates keep shared DSP and private histories race free.
    TEST(DelayAudioPdc, ConcurrentAutomaticStereoCandidatesKeepSharedDSPAndPrivateHistoriesRaceFree) {
        Matrix model; ASSERT_TRUE(model.make(Scenario::Chain)); PlanSwitcher switcher;
        const BackendConfig cfg{.maxFrames=7,.inputChannels=2,.outputChannels=2,.sampleRate=48000};
        ASSERT_TRUE(switcher.initialize(model.graph,cfg));
        std::atomic<bool> running{true},audioOK{true}; std::atomic<unsigned> callbacks{0};
        std::thread audio([&] {
            using Samples=std::array<std::array<float,9>,2>;
            std::array<Samples,2> input{},output{};
            for (auto& bank : input) for (unsigned lane=0;lane<2;++lane) {
                bank[lane].fill(lane==0 ? 1.F : -2.F); bank[lane].front()=-999.F; bank[lane].back()=-999.F;
            }
            for (auto& bank : output) for (auto& lane : bank) lane.fill(-999.F);
            const std::array<FrameCount,4> sizes{0,1,2,7}; unsigned block=0; GenerationID previous=0;
            while (running.load(std::memory_order_acquire)) {
                const auto bank=block%2,frames=sizes[block%sizes.size()]; ++block;
                const std::array<const float*,2> inputs{input[bank][0].data()+1,input[bank][1].data()+1};
                const std::array outputs{output[bank][0].data()+1,output[bank][1].data()+1};
                switcher.exec(mka::audio::AudioProcessContext{inputs,outputs,frames,7,7});
                const auto generation=switcher.audioGeneration();
                bool valid=generation>=previous && generation<=1001; previous=generation;
                for (unsigned lane=0;lane<2;++lane) {
                    for (FrameCount f=0;f<frames;++f) {
                        const auto value=output[bank][lane][f+1];
                        valid=valid && (lane==0 ? value>=0.F && value<=2.F : value>=-4.F && value<=0.F);
                    }
                    valid=valid && input[bank][lane].front()==-999.F && input[bank][lane].back()==-999.F &&
                        output[bank][lane].front()==-999.F && output[bank][lane].back()==-999.F;
                }
                if (!valid) audioOK.store(false,std::memory_order_relaxed);
                callbacks.fetch_add(1,std::memory_order_relaxed);
            }
        });
        bool controlOK=true;
        for (unsigned generation=2;generation<=1001 && controlOK;++generation) {
            if (!switcher.prepareInactivePlan(model.graph,cfg)) { controlOK=false; break; }
            const auto* ready=switcher.readyPlan();
            controlOK=ready && ready->delayTaskCount()==4 && ready->preparedMemoryUsage().historyBytes==1408U*sizeof(float) &&
                ready->pdcAnalysis()->find(model.mainEdge(0))->samples==320 && ready->pdcAnalysis()->find(model.secondaryEdge(1))->samples==384;
            if (!controlOK || !switcher.publishReadyPlan()) { controlOK=false; break; }
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
            while (switcher.handshakeState()!=HandshakeState::Acknowledged) {
                if (std::chrono::steady_clock::now()>deadline) { controlOK=false; break; }
                std::this_thread::yield();
            }
            if (controlOK && !switcher.retireAcknowledgedPlan()) controlOK=false;
        }
        running.store(false,std::memory_order_release); audio.join();
        EXPECT_TRUE(controlOK); EXPECT_TRUE(audioOK.load()); EXPECT_GE(callbacks.load(),1000U);
        EXPECT_EQ(switcher.controlGeneration(),1001U);
        for (const auto& ref : model.references) EXPECT_EQ(model.processor(ref.id).queries,1U);
        ASSERT_TRUE(switcher.shutdownAfterBackendStopped());
    }
}
