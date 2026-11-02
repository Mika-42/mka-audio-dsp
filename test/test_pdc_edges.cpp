#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <vector>
#include <gtest/gtest.h>
import mka.audio.dsp.dag;
#include "pdc_render_fixture.hpp"

namespace {
    using namespace mka::audio::dsp;
    using namespace pdc_test;
    void checkDelay(const PdcAnalysis& analysis,Connection connection,std::uint64_t expected) {
        SCOPED_TRACE(::testing::Message()<<"from="<<connection.from<<" to="<<connection.to<<" D="<<expected);
        const auto* value=analysis.find(connection); ASSERT_NE(value,nullptr);
        EXPECT_EQ(value->samples,expected);
    }

    TEST(PdcEdges, ThreeAndEightProduceFiveAndZeroForExactMainAndAuxiliaryEdges) {
        DAG graph;
        const auto fast=graph.add<UniformDelayNode>(3U),slow=graph.add<UniformDelayNode>(8U);
        const auto join=graph.add<UniformDelayNode>(7U); ASSERT_TRUE(fast); ASSERT_TRUE(slow); ASSERT_TRUE(join);
        const auto a=edge(*fast,*join),b=edge(*slow,*join,PortType::Aux);
        ASSERT_TRUE(connect(graph,a)); ASSERT_TRUE(connect(graph,b));
        const std::vector<Connection> before(graph.connections().begin(),graph.connections().end());
        auto plan=graph.compile(BackendConfig{.maxFrames=4,.inputChannels=1}); ASSERT_TRUE(plan);
        checkDelay(*plan->pdcAnalysis(),a,5); checkDelay(*plan->pdcAnalysis(),b,0);
        EXPECT_EQ(plan->pdcAnalysis()->find(*join)->arrival,8U);
        EXPECT_EQ(plan->pdcAnalysis()->find(*join)->cumulative,15U);
        EXPECT_EQ(plan->delayTaskCount(),1U); EXPECT_EQ(plan->preparedMemoryUsage().historyBytes,20U);
        EXPECT_EQ(context(*plan,*join).auxiliary[0].buffer,context(*plan,*slow).outputs[0].buffer);
        EXPECT_NE(context(*plan,*join).inputs[0].buffer,context(*plan,*fast).outputs[0].buffer);
        EXPECT_EQ((std::vector<Connection>{graph.connections().begin(),graph.connections().end()}),before);
        EXPECT_EQ(plan->tasks().size(),3U); // Internal delay is not a public Node.
    }

    TEST(PdcEdges, ImpulsesAlignAcrossTwoFourFrameCallbacksAtTheLogicalJunction) {
        DAG graph;
        const auto fast=graph.add<UniformDelayNode>(1U),slow=graph.add<UniformDelayNode>(6U);
        const auto join=graph.add<UniformDelayNode>(0U,ports(),0U);
        ASSERT_TRUE(fast); ASSERT_TRUE(slow); ASSERT_TRUE(join);
        ASSERT_TRUE(connect(graph,edge(*fast,*join))); ASSERT_TRUE(connect(graph,edge(*slow,*join,PortType::Aux)));
        ASSERT_TRUE(graph.markAsOutput(*join));
        auto plan=graph.compile(BackendConfig{.maxFrames=4,.inputChannels=1,.outputChannels=1}); ASSERT_TRUE(plan);
        std::array<float,4> input{1,0,0,0},output{};
        const std::array<const float*,1> inputs{input.data()}; const std::array outputs{output.data()};
        ASSERT_TRUE(plan->exec(BackendBlock{inputs,outputs,4}));
        EXPECT_EQ(output,(std::array<float,4>{0,0,0,0}));
        input.fill(0); ASSERT_TRUE(plan->exec(BackendBlock{inputs,outputs,4}));
        EXPECT_EQ(output,(std::array<float,4>{0,0,2,0}));
        for (std::size_t t=0;t<8;++t) {
            EXPECT_EQ(node(graph,*join).mainLog[t],t==6 ? 1.F : 0.F);
            EXPECT_EQ(node(graph,*join).auxLog[t],t==6 ? 1.F : 0.F);
        }
    }

    TEST(PdcEdges, ConsumerIntrinsicDelayIsCountedAndRenderedOnceAfterInputAlignment) {
        DAG graph;
        const auto fast=graph.add<UniformDelayNode>(3U),slow=graph.add<UniformDelayNode>(8U);
        const auto join=graph.add<UniformDelayNode>(7U,ports(),0U);
        ASSERT_TRUE(fast); ASSERT_TRUE(slow); ASSERT_TRUE(join);
        ASSERT_TRUE(connect(graph,edge(*fast,*join))); ASSERT_TRUE(connect(graph,edge(*slow,*join,PortType::Aux)));
        ASSERT_TRUE(graph.markAsOutput(*join));
        auto plan=graph.compile(BackendConfig{.maxFrames=4,.inputChannels=1,.outputChannels=1}); ASSERT_TRUE(plan);
        std::array<float,4> input{},output{};
        const std::array<const float*,1> inputs{input.data()}; const std::array outputs{output.data()};
        for (unsigned block=0;block<5;++block) {
            input.fill(0); if (block==0) input[0]=1;
            ASSERT_TRUE(plan->exec(BackendBlock{inputs,outputs,4}));
            for (unsigned f=0;f<4;++f) EXPECT_EQ(output[f],block*4+f==15 ? 2.F : 0.F);
        }
        EXPECT_EQ(node(graph,*join).mainLog[8],1.F); EXPECT_EQ(node(graph,*join).auxLog[8],1.F);
    }

    TEST(PdcEdges, ZeroCompensationUsesDirectPointersWithoutDelayOperationsOrHistory) {
        DAG graph;
        const auto a=graph.add<UniformDelayNode>(8U),b=graph.add<UniformDelayNode>(8U),join=graph.add<UniformDelayNode>(0U);
        ASSERT_TRUE(a); ASSERT_TRUE(b); ASSERT_TRUE(join);
        ASSERT_TRUE(connect(graph,edge(*a,*join))); ASSERT_TRUE(connect(graph,edge(*b,*join,PortType::Aux)));
        auto plan=graph.compile(BackendConfig{.maxFrames=4}); ASSERT_TRUE(plan);
        EXPECT_EQ(plan->delayTaskCount(),0U); EXPECT_EQ(plan->preparedMemoryUsage().historyBytes,0U);
        EXPECT_EQ(context(*plan,*a).outputs[0].buffer,context(*plan,*join).inputs[0].buffer);
        EXPECT_EQ(context(*plan,*b).outputs[0].buffer,context(*plan,*join).auxiliary[0].buffer);
        const std::array explicitZero{ConnectionDelay{edge(*a,*join),0}};
        BackendConfig cfg{.maxFrames=4,.connectionDelays=explicitZero};
        ASSERT_TRUE(plan->prepareBackend(cfg,graph.connections(),graph.outputNodes()));
        EXPECT_EQ(plan->delayTaskCount(),0U);
    }

    TEST(PdcEdges, SameOutputReceivesDifferentCompensationOnTwoExactFanoutConnections) {
        DAG graph;
        const auto source=graph.add<UniformDelayNode>(0U),slow3=graph.add<UniformDelayNode>(3U),slow8=graph.add<UniformDelayNode>(8U);
        const auto v=graph.add<UniformDelayNode>(0U),w=graph.add<UniformDelayNode>(0U);
        ASSERT_TRUE(source); ASSERT_TRUE(slow3); ASSERT_TRUE(slow8); ASSERT_TRUE(v); ASSERT_TRUE(w);
        const auto uv=edge(*source,*v),uw=edge(*source,*w,PortType::Aux);
        for (auto e : {uv,uw,edge(*slow3,*v,PortType::Aux),edge(*slow8,*w)}) ASSERT_TRUE(connect(graph,e));
        auto plan=graph.compile(BackendConfig{.maxFrames=4,.inputChannels=1}); ASSERT_TRUE(plan);
        checkDelay(*plan->pdcAnalysis(),uv,3); checkDelay(*plan->pdcAnalysis(),uw,8);
        EXPECT_EQ(plan->delayTaskCount(),2U);
        EXPECT_EQ(plan->preparedMemoryUsage().historyBytes,(3U+8U)*sizeof(float));
        EXPECT_EQ(plan->signalLifetimes().size(),plan->tasks().size()+2);
        // Non-overlapping branch temporaries may reuse a PCM slot. Their
        // histories and produced signals must still remain independent.
        std::array<float,4> input{}; const std::array<const float*,1> inputs{input.data()};
        for (unsigned block=0;block<4;++block) {
            for (unsigned f=0;f<4;++f) input[f]=static_cast<float>(block*4+f+1);
            ASSERT_TRUE(plan->exec(BackendBlock{.inputs=inputs,.frames=4}));
        }
        for (std::size_t t=0;t<16;++t) {
            EXPECT_EQ(node(graph,*v).mainLog[t],t>=3 ? static_cast<float>(t-3+1) : 0.F);
            EXPECT_EQ(node(graph,*w).auxLog[t],t>=8 ? static_cast<float>(t-8+1) : 0.F);
        }
    }

    TEST(PdcEdges, LargeIntrinsicDifferenceStaysWideInExactEdgeTable) {
        const auto maximum=std::numeric_limits<LatencySamples>::max();
        const LatencySnapshot snapshot{48000,{{1,0},{2,maximum},{3,maximum},{4,0}}};
        const std::array<NodeID,4> order{1,2,3,4};
        const std::array connections{edge(2,3),edge(1,4),edge(3,4,PortType::Aux)};
        const auto pdc=analyzeLatencies(snapshot,order,connections); ASSERT_TRUE(pdc);
        checkDelay(*pdc,edge(1,4),std::uint64_t{maximum}*2);
        EXPECT_GT(pdc->find(edge(1,4))->samples,maximum);
    }
}
