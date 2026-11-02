#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <vector>
#include <gtest/gtest.h>
import mka.audio.dsp.plan_switcher;
#include "pdc_render_fixture.hpp"

namespace {
    using namespace mka::audio::dsp;
    using namespace pdc_test;
    struct ResourceGraph {
        DAG graph;
        NodeID fast=0,middle=0,slow=0,join=0;
        Result make() {
            const auto a=graph.add<UniformDelayNode>(0U),b=graph.add<UniformDelayNode>(255U);
            const auto c=graph.add<UniformDelayNode>(256U);
            const auto d=graph.add<UniformDelayNode>(0U,Config{.layoutInput={0,1},.layoutAuxiliary={0},.layoutOutput={0}},0U);
            if (!a || !b || !c || !d) return std::unexpected(Error{ErrorCode::PrepareFailed});
            fast=*a; middle=*b; slow=*c; join=*d;
            for (const auto e : {edge(fast,join),edge(middle,join,PortType::In,1),edge(slow,join,PortType::Aux)})
                if (const auto result=connect(graph,e); !result) return result;
            return graph.markAsOutput(join);
        }
    };
    const BackendConfig cfg{.maxFrames=4,.inputChannels=1,.outputChannels=1,.sampleRate=48000};

    TEST(PdcBudget, ZeroOneAnd256DelaysFitIndependentFrameAndHistoryCapacities) {
        ResourceGraph p; ASSERT_TRUE(p.make());
        auto config=cfg; config.maxDelaySamples=256;
        auto plan=p.graph.compile(config); ASSERT_TRUE(plan);
        EXPECT_EQ(plan->pdcAnalysis()->find(edge(p.fast,p.join))->samples,256U);
        EXPECT_EQ(plan->pdcAnalysis()->find(edge(p.middle,p.join,PortType::In,1))->samples,1U);
        EXPECT_EQ(plan->pdcAnalysis()->find(edge(p.slow,p.join,PortType::Aux))->samples,0U);
        EXPECT_EQ(plan->delayTaskCount(),2U);
        EXPECT_EQ(plan->preparedMemoryUsage().historyBytes,257U*sizeof(float));
        std::array<float,4> input{},output{};
        const std::array<const float*,1> inputs{input.data()}; const std::array outputs{output.data()};
        for (unsigned block=0;block<70;++block) {
            for (unsigned f=0;f<4;++f) input[f]=static_cast<float>(block*4+f+1);
            ASSERT_TRUE(plan->exec(BackendBlock{inputs,outputs,4}));
            for (unsigned f=0;f<4;++f) {
                const auto t=block*4+f;
                EXPECT_EQ(output[f],t<256 ? 0.F : 3.F*static_cast<float>(t-256+1));
            }
        }
    }

    TEST(PdcBudget, ExactAggregateBudgetIncludesTablesTemporariesSilenceAndEveryHistory) {
        ResourceGraph p; ASSERT_TRUE(p.make()); auto plan=p.graph.compile(cfg); ASSERT_TRUE(plan);
        const auto usage=plan->preparedMemoryUsage();
        EXPECT_EQ(usage.historyBytes,257U*sizeof(float)); EXPECT_EQ(usage.silenceBytes,4U*sizeof(float));
        EXPECT_EQ(usage.slotBytes,plan->bufferSlotCount()*4U*sizeof(float));
        EXPECT_GT(usage.metadataBytes,0U);
        EXPECT_EQ(usage.totalBytes,usage.slotBytes+usage.silenceBytes+usage.historyBytes+usage.metadataBytes+usage.paddingBytes);
        auto exact=cfg; exact.memoryBudgetBytes=usage.totalBytes;
        auto accepted=p.graph.compile(exact); ASSERT_TRUE(accepted);
        EXPECT_EQ(accepted->preparedMemoryUsage().totalBytes,usage.totalBytes);
        --exact.memoryBudgetBytes;
        const auto refused=p.graph.compile(exact); ASSERT_FALSE(refused);
        EXPECT_EQ(refused.error().code,ErrorCode::MemoryBudgetExceeded);
        EXPECT_EQ(plan->preparedMemoryUsage().totalBytes,usage.totalBytes);
        EXPECT_EQ(plan->pdcAnalysis()->find(p.join)->cumulative,256U);
    }

    TEST(PdcBudget, RejectedCandidatesPreserveActiveGenerationAndItsRunningHistory) {
        ResourceGraph p; ASSERT_TRUE(p.make()); PlanSwitcher switcher;
        ASSERT_TRUE(switcher.initialize(p.graph,cfg));
        const auto* active=switcher.slotAddress(0);
        const auto* samples=context(*active,p.fast).outputs[0].buffer;
        std::array<float,4> input{},output{};
        const std::array<const float*,1> inputs{input.data()}; const std::array outputs{output.data()};
        for (unsigned block=0;block<80;++block) {
            if (block==70 || block==71) {
                auto rejected=cfg;
                if (block==70) rejected.maxDelaySamples=255;
                else rejected.memoryBudgetBytes=active->preparedMemoryUsage().totalBytes-1;
                const auto failed=switcher.prepareAndPublish(p.graph,rejected);
                ASSERT_FALSE(failed); EXPECT_EQ(failed.error().code,ErrorCode::MemoryBudgetExceeded);
                if (block==70) EXPECT_EQ(failed.error().channel,edge(p.fast,p.join).to);
                EXPECT_EQ(switcher.handshakeState(),HandshakeState::Idle);
                EXPECT_EQ(switcher.readyPlan(),nullptr);
                EXPECT_EQ(context(*active,p.fast).outputs[0].buffer,samples);
            }
            for (unsigned f=0;f<4;++f) input[f]=static_cast<float>(block*4+f+1);
            switcher.exec(mka::audio::AudioProcessContext{inputs,outputs,4,4,4});
            for (unsigned f=0;f<4;++f) {
                const auto t=block*4+f;
                EXPECT_EQ(output[f],t<256 ? 0.F : 3.F*static_cast<float>(t-256+1));
            }
            EXPECT_EQ(switcher.audioGeneration(),1U);
        }
    }

    TEST(PdcBudget, SampleQuotaAndPlatformSizeProductsAreCheckedWithoutAllocatingHugeHistories) {
        constexpr auto sizeMax=std::numeric_limits<std::size_t>::max();
        constexpr auto sampleMax=std::numeric_limits<std::uint64_t>::max();
        static_assert(noexcept(checkedDelayBytes(0)));
        EXPECT_EQ(BackendConfig{}.maxDelaySamples,sampleMax);
        EXPECT_EQ(checkedDelayBytes(0,0).value(),0U);
        EXPECT_EQ(checkedDelayBytes(1,1).value(),sizeof(float));
        EXPECT_EQ(checkedDelayBytes(1,0).error().code,ErrorCode::MemoryBudgetExceeded);
        EXPECT_EQ(checkedDelayBytes(sizeMax/sizeof(float)).value(),(sizeMax/sizeof(float))*sizeof(float));
        EXPECT_EQ(checkedDelayBytes(sizeMax/sizeof(float)+1).error().code,ErrorCode::ArithmeticOverflow);
        EXPECT_EQ(checkedDelayBytes(sampleMax).error().code,ErrorCode::ArithmeticOverflow);
        const std::array<std::uint64_t,2> sumOverflow{sizeMax/sizeof(float),1};
        EXPECT_EQ(measurePlanMemory({.delaySamples=sumOverflow}).error().code,ErrorCode::ArithmeticOverflow);
        if constexpr (sizeof(std::size_t)<sizeof(std::uint64_t))
            EXPECT_EQ(checkedDelayBytes(std::uint64_t{sizeMax}+1).error().code,ErrorCode::ArithmeticOverflow);
        // On 64-bit size_t every uint64 count fits; its byte product still may not.
        const auto near=checkedLatencyAdd(sampleMax-256,256); ASSERT_TRUE(near);
        EXPECT_EQ(*near,sampleMax);
        EXPECT_EQ(checkedLatencyAdd(*near,1).error().code,ErrorCode::LatencyOverflow);
    }

    TEST(PdcBudget, UnrepresentableExplicitHistoryIsRejectedBeforeReplacingPreparedAutomaticState) {
        ResourceGraph p; ASSERT_TRUE(p.make()); auto plan=p.graph.compile(cfg); ASSERT_TRUE(plan);
        const auto* original=plan->pdcAnalysis(); const auto bytes=plan->preparedMemoryUsage().totalBytes;
        const std::array huge{ConnectionDelay{edge(p.fast,p.join),std::numeric_limits<std::uint64_t>::max()}};
        auto config=cfg; config.connectionDelays=huge;
        const auto failed=plan->prepareBackend(config,p.graph.connections(),p.graph.outputNodes());
        ASSERT_FALSE(failed); EXPECT_EQ(failed.error().code,ErrorCode::ArithmeticOverflow);
        EXPECT_EQ(plan->pdcAnalysis(),original); EXPECT_EQ(plan->preparedMemoryUsage().totalBytes,bytes);
        EXPECT_EQ(failed.error().channel,edge(p.fast,p.join).to);
    }
}
