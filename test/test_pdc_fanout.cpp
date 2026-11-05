/**
 * @file test_pdc_fanout.cpp
 * @brief Tests for pdc fanout.
 * @author Mika
 */
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>
#include <gtest/gtest.h>
import mka.audio.dsp.dag;
#include "pdc_render_fixture.hpp"

namespace {
    using namespace mka::audio::dsp;
    using namespace pdc_test;
    /// @brief Return the expected delayed ramp, including the initial zero history.
    float delayedRamp(std::size_t t,std::size_t delay) {
        return t<delay ? 0.F : static_cast<float>(t-delay+1);
    }

    /// @brief Test case: ten branch histories and late direct reader preserve shared source sample exactly.
    TEST(PdcFanout, TenBranchHistoriesAndLateDirectReaderPreserveSharedSourceSampleExactly) {
        for (const auto strategy : {SlotStrategy::Reuse,SlotStrategy::Unique}) {
            SCOPED_TRACE(strategy==SlotStrategy::Reuse ? "Reuse" : "Unique");
            DAG graph; const auto source=graph.add<UniformDelayNode>(0U); ASSERT_TRUE(source);
            std::array<NodeID,10> slow{},joins{};
            for (unsigned i=0;i<10;++i) {
                const auto id=graph.add<UniformDelayNode>(i+1); ASSERT_TRUE(id); slow[i]=*id;
                ASSERT_TRUE(connect(graph,edge(*source,*id)));
            }
            for (unsigned i=0;i<10;++i) {
                const auto id=graph.add<UniformDelayNode>(0U); ASSERT_TRUE(id); joins[i]=*id;
                ASSERT_TRUE(connect(graph,edge(*source,*id)));
                ASSERT_TRUE(connect(graph,edge(slow[i],*id,PortType::Aux)));
            }
            const auto late=graph.add<UniformDelayNode>(0U); ASSERT_TRUE(late);
            ASSERT_TRUE(connect(graph,edge(*source,*late)));
            const std::vector<Connection> before(graph.connections().begin(),graph.connections().end());
            auto plan=graph.compile(BackendConfig{.maxFrames=4,.inputChannels=1,.slotStrategy=strategy}); ASSERT_TRUE(plan);
            EXPECT_EQ(plan->tasks().size(),22U); EXPECT_EQ(plan->delayTaskCount(),10U);
            EXPECT_EQ(plan->preparedMemoryUsage().historyBytes,55U*sizeof(float));
            EXPECT_EQ(plan->pdcAnalysis()->edges.size(),31U);
            EXPECT_EQ(context(*plan,*late).inputs[0].buffer,context(*plan,*source).outputs[0].buffer);
            // One Out per Node: the initial signal identity is its task index.
            const auto rootSignal=*plan->taskIndexOf(*source),lateSignal=*plan->taskIndexOf(*late);
            EXPECT_EQ(plan->signalLifetimes()[rootSignal].lastUse,plan->signalLifetimes()[lateSignal].producedAt);
            EXPECT_NE(plan->signalSlots()[rootSignal],plan->signalSlots()[lateSignal]);
            std::array<float,6> input{}; input.fill(-999.F);
            const std::array<const float*,1> inputs{input.data()+1};
            constexpr std::array<FrameCount,8> sizes{0,1,4,2,4,3,1,4};
            std::size_t t=0;
            for (unsigned block=0;block<24;++block) {
                const auto frames=sizes[block%sizes.size()];
                for (FrameCount f=0;f<frames;++f) input[f+1]=static_cast<float>(t+f+1);
                const auto beforeInput=input;
                ASSERT_TRUE(plan->exec(BackendBlock{.inputs=inputs,.frames=frames}));
                EXPECT_EQ(input,beforeInput); t+=frames;
            }
            for (unsigned branch=0;branch<10;++branch) {
                SCOPED_TRACE(::testing::Message()<<"NodeID="<<joins[branch]<<" D="<<branch+1);
                EXPECT_EQ(plan->pdcAnalysis()->find(edge(*source,joins[branch]))->samples,branch+1);
                EXPECT_EQ(plan->pdcAnalysis()->find(edge(slow[branch],joins[branch],PortType::Aux))->samples,0U);
                for (std::size_t sample=0;sample<t;++sample) {
                    EXPECT_EQ(node(graph,joins[branch]).mainLog[sample],delayedRamp(sample,branch+1));
                    EXPECT_EQ(node(graph,joins[branch]).auxLog[sample],delayedRamp(sample,branch+1));
                    EXPECT_EQ(node(graph,joins[branch]).producedLog[sample],2.F*delayedRamp(sample,branch+1));
                }
            }
            for (std::size_t sample=0;sample<t;++sample) {
                EXPECT_EQ(node(graph,*late).mainLog[sample],static_cast<float>(sample+1));
                EXPECT_EQ(node(graph,*source).producedLog[sample],static_cast<float>(sample+1));
            }
            EXPECT_EQ((std::vector<Connection>{graph.connections().begin(),graph.connections().end()}),before);
        }
    }

    /// @brief Test case: auxiliary three aligns with main eight while ignored and unproduced ports stay silent.
    TEST(PdcFanout, AuxiliaryThreeAlignsWithMainEightWhileIgnoredAndUnproducedPortsStaySilent) {
        DAG graph;
        const auto fast=graph.add<UniformDelayNode>(3U),slow=graph.add<UniformDelayNode>(8U);
        const auto ignored=graph.add<UniformDelayNode>(512U,Config{});
        const auto join=graph.add<UniformDelayNode>(0U,Config{.layoutInput={0},.layoutAuxiliary={0,1,2},.layoutOutput={0}},0U);
        ASSERT_TRUE(fast); ASSERT_TRUE(slow); ASSERT_TRUE(ignored); ASSERT_TRUE(join);
        const auto compensated=edge(*fast,*join,PortType::Aux);
        const auto undeclared=edge(*fast,*join,PortType::In,9);
        const auto unproduced=edge(*ignored,*join,PortType::Aux,1);
        for (const auto connection : {compensated,edge(*slow,*join),undeclared,unproduced})
            ASSERT_TRUE(connect(graph,connection));
        ASSERT_TRUE(graph.markAsOutput(*join));
        auto plan=graph.compile(BackendConfig{.maxFrames=4,.inputChannels=1,.outputChannels=1}); ASSERT_TRUE(plan);
        ASSERT_EQ(plan->pdcAnalysis()->edges.size(),2U);
        EXPECT_EQ(plan->pdcAnalysis()->find(compensated)->samples,5U);
        EXPECT_EQ(plan->pdcAnalysis()->find(undeclared),nullptr);
        EXPECT_EQ(plan->pdcAnalysis()->find(unproduced),nullptr);
        EXPECT_EQ(plan->pdcAnalysis()->find(*join)->arrival,8U);
        EXPECT_EQ(plan->signalLifetimes().size(),4U); // Three logical outputs and one positive delay.
        EXPECT_EQ(plan->delayTaskCount(),1U);
        EXPECT_EQ(context(*plan,*join).inputs[0].buffer,context(*plan,*slow).outputs[0].buffer);
        std::array<float,4> input{},output{};
        const std::array<const float*,1> inputs{input.data()}; const std::array outputs{output.data()};
        std::size_t t=0;
        for (const FrameCount frames : {4U,0U,1U,3U,4U,4U}) {
            for (FrameCount f=0;f<frames;++f) input[f]=static_cast<float>(t+f+1);
            ASSERT_TRUE(plan->exec(BackendBlock{inputs,outputs,frames}));
            for (FrameCount f=0;f<frames;++f) EXPECT_EQ(output[f],2.F*delayedRamp(t+f,8));
            t+=frames;
        }
        for (const auto* silent : {context(*plan,*join).auxiliary[1].buffer,context(*plan,*join).auxiliary[2].buffer})
            for (unsigned f=0;f<4;++f) EXPECT_EQ(silent[f],0.F);
        EXPECT_EQ(graph.connections().size(),4U);
    }

    /// @brief Test case: four node diamond compensates only the short branch without implicit mix nodes.
    TEST(PdcFanout, FourNodeDiamondCompensatesOnlyTheShortBranchWithoutImplicitMixNodes) {
        DAG graph;
        const auto source=graph.add<UniformDelayNode>(0U),left=graph.add<UniformDelayNode>(3U);
        const auto right=graph.add<UniformDelayNode>(8U),mix=graph.add<UniformDelayNode>(0U,ports(),0U);
        ASSERT_TRUE(source); ASSERT_TRUE(left); ASSERT_TRUE(right); ASSERT_TRUE(mix);
        for (const auto e : {edge(*source,*left),edge(*source,*right),edge(*left,*mix),edge(*right,*mix,PortType::Aux)})
            ASSERT_TRUE(connect(graph,e));
        ASSERT_TRUE(graph.markAsOutput(*mix));
        auto plan=graph.compile(BackendConfig{.maxFrames=4,.inputChannels=1,.outputChannels=1}); ASSERT_TRUE(plan);
        EXPECT_EQ(plan->tasks().size(),4U); EXPECT_EQ(plan->delayTaskCount(),1U);
        std::array<float,4> input{1,0,0,0},output{};
        const std::array<const float*,1> inputs{input.data()}; const std::array outputs{output.data()};
        for (unsigned block=0;block<4;++block) {
            ASSERT_TRUE(plan->exec(BackendBlock{inputs,outputs,4}));
            for (unsigned f=0;f<4;++f) EXPECT_EQ(output[f],block*4+f==8 ? 2.F : 0.F);
            input.fill(0);
        }
    }
}
