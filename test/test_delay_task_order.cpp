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
    std::size_t nodeOperation(const ExecutionPlan& plan,NodeID id) {
        const auto sequence=plan.operationSchedule();
        const auto found=std::find_if(sequence.begin(),sequence.end(),[id](const auto& op) {
            return op.kind==OperationKind::Node && op.node==id;
        });
        return static_cast<std::size_t>(found-sequence.begin());
    }
    void verifyOrder(const ExecutionPlan& plan,Connection edge,std::uint64_t delay) {
        SCOPED_TRACE(::testing::Message()<<"from="<<edge.from<<" to="<<edge.to<<" D="<<delay);
        const auto sequence=plan.operationSchedule();
        const auto from=nodeOperation(plan,decodeNode(edge.from)),to=nodeOperation(plan,decodeNode(edge.to));
        ASSERT_LT(from,sequence.size()); ASSERT_LT(to,sequence.size()); EXPECT_LT(from,to);
        std::size_t matches=0;
        for (std::size_t i=0;i<sequence.size();++i) {
            const auto& op=sequence[i];
            if (op.kind!=OperationKind::Delay || op.connection!=edge) continue;
            ++matches; EXPECT_EQ(op.node,invalidNodeID); EXPECT_EQ(op.delaySamples,delay);
            EXPECT_LT(from,i); EXPECT_LT(i,to); EXPECT_EQ(op.outputCount,1U);
            EXPECT_EQ(op.sourceSignal,sequence[from].outputSignal); // Fixtures produce one Out.
            EXPECT_NE(op.outputSignal,op.sourceSignal);
            EXPECT_NE(plan.signalSlots()[op.outputSignal],plan.signalSlots()[op.sourceSignal]);
            EXPECT_EQ(plan.signalLifetimes()[op.outputSignal].producedAt,i);
            EXPECT_EQ(plan.signalLifetimes()[op.outputSignal].lastUse,to);
        }
        EXPECT_EQ(matches,delay==0 ? 0U : 1U);
    }

    TEST(DelayTaskOrder, ChainOneThreeAnd512HasStrictProducerDelayConsumerOrderAndExactPersistentRender) {
        DAG graph; std::array<NodeID,4> ids{};
        for (unsigned i=0;i<4;++i) {
            const auto id=graph.add<UniformDelayNode>(0U,ports(),i==3 ? 0U : UINT32_MAX); ASSERT_TRUE(id); ids[i]=*id;
        }
        const std::array edges{edge(ids[0],ids[1]),edge(ids[1],ids[2]),edge(ids[2],ids[3])};
        for (const auto e : edges) ASSERT_TRUE(connect(graph,e));
        ASSERT_TRUE(graph.markAsOutput(ids[3]));
        const std::array delays{ConnectionDelay{edges[0],1},ConnectionDelay{edges[1],3},ConnectionDelay{edges[2],512}};
        const std::vector<Connection> desired(graph.connections().begin(),graph.connections().end());
        auto plan=graph.compile(BackendConfig{.maxFrames=128,.inputChannels=1,.outputChannels=1,.connectionDelays=delays}); ASSERT_TRUE(plan);
        ASSERT_EQ(plan->operationSchedule().size(),7U); EXPECT_EQ(plan->tasks().size(),4U);
        for (unsigned i=0;i<3;++i) ASSERT_NO_FATAL_FAILURE(verifyOrder(*plan,edges[i],delays[i].samples));
        EXPECT_EQ(plan->preparedMemoryUsage().historyBytes,516U*sizeof(float));
        const std::vector<PlanOperationInfo> schedule(plan->operationSchedule().begin(),plan->operationSchedule().end());
        std::array<float,130> input{},output{}; input.fill(-999.F);
        const std::array<const float*,1> inputs{input.data()+1}; const std::array outputs{output.data()+1};
        const std::array<FrameCount,6> sizes{0,1,2,7,128,0};
        std::size_t n=0,block=0;
        while (n<640) {
            const auto frames=static_cast<FrameCount>(std::min<std::size_t>(sizes[block++%sizes.size()],640-n));
            for (FrameCount f=0;f<frames;++f) input[f+1]=static_cast<float>(n+f+1);
            output.fill(-999.F); const auto before=input;
            ASSERT_TRUE(plan->exec(BackendBlock{inputs,outputs,frames}));
            for (FrameCount f=0;f<frames;++f)
                EXPECT_EQ(output[f+1],n+f<516 ? 0.F : static_cast<float>(n+f-516+1))<<"n="<<n+f;
            EXPECT_EQ(input,before); EXPECT_EQ(output.front(),-999.F);
            for (std::size_t f=frames+1;f<output.size();++f) EXPECT_EQ(output[f],-999.F);
            n+=frames;
        }
        EXPECT_TRUE(std::equal(schedule.begin(),schedule.end(),plan->operationSchedule().begin()));
        EXPECT_EQ((std::vector<Connection>{graph.connections().begin(),graph.connections().end()}),desired);
        for (const auto id : ids) EXPECT_EQ(node(graph,id).seen,640U);
    }

    TEST(DelayTaskOrder, DiamondCompensatesAuxiliaryAfterItsProducerAndLeavesTheLongestMainPathDirect) {
        DAG graph;
        const auto source=graph.add<UniformDelayNode>(0U),a=graph.add<UniformDelayNode>(1U);
        const auto b=graph.add<UniformDelayNode>(4U),mix=graph.add<UniformDelayNode>(0U,ports(),0U);
        ASSERT_TRUE(source); ASSERT_TRUE(a); ASSERT_TRUE(b); ASSERT_TRUE(mix);
        const auto fast=edge(*a,*mix,PortType::Aux),slow=edge(*b,*mix);
        for (const auto e : {edge(*source,*a),edge(*source,*b),fast,slow}) ASSERT_TRUE(connect(graph,e));
        ASSERT_TRUE(graph.markAsOutput(*mix));
        auto plan=graph.compile(BackendConfig{.maxFrames=7,.inputChannels=1,.outputChannels=1}); ASSERT_TRUE(plan);
        ASSERT_NO_FATAL_FAILURE(verifyOrder(*plan,fast,3));
        ASSERT_NO_FATAL_FAILURE(verifyOrder(*plan,slow,0));
        EXPECT_EQ(plan->operationSchedule().size(),5U);
        EXPECT_EQ(context(*plan,*mix).inputs[0].buffer,context(*plan,*b).outputs[0].buffer);
        EXPECT_NE(context(*plan,*mix).auxiliary[0].buffer,context(*plan,*a).outputs[0].buffer);
        std::array<float,7> input{},output{};
        const std::array<const float*,1> inputs{input.data()}; const std::array outputs{output.data()};
        std::size_t n=0;
        for (const FrameCount frames : {1U,2U,7U,0U,4U,7U}) {
            input.fill(0); if (n==0 && frames) input[0]=1.F;
            ASSERT_TRUE(plan->exec(BackendBlock{inputs,outputs,frames}));
            for (FrameCount f=0;f<frames;++f) EXPECT_EQ(output[f],n+f==4 ? 2.F : 0.F);
            n+=frames;
        }
    }

    TEST(DelayTaskOrder, SharedOutHasDistinctOneThreeAnd512EdgeProductionsAndALateDirectReader) {
        DAG graph; const auto source=graph.add<UniformDelayNode>(0U); ASSERT_TRUE(source);
        std::array<NodeID,3> slow{},joins{}; constexpr std::array<LatencySamples,3> delay{1,3,512};
        for (unsigned i=0;i<3;++i) {
            const auto id=graph.add<UniformDelayNode>(delay[i]); ASSERT_TRUE(id); slow[i]=*id;
            ASSERT_TRUE(connect(graph,edge(*source,*id)));
        }
        for (unsigned i=0;i<3;++i) {
            const auto id=graph.add<UniformDelayNode>(0U,ports(),i); ASSERT_TRUE(id); joins[i]=*id;
            ASSERT_TRUE(connect(graph,edge(*source,*id))); ASSERT_TRUE(connect(graph,edge(slow[i],*id,PortType::Aux)));
            ASSERT_TRUE(graph.markAsOutput(*id));
        }
        const auto late=graph.add<UniformDelayNode>(0U,ports(),3U); ASSERT_TRUE(late);
        ASSERT_TRUE(connect(graph,edge(*source,*late))); ASSERT_TRUE(graph.markAsOutput(*late));
        auto plan=graph.compile(BackendConfig{.maxFrames=128,.inputChannels=1,.outputChannels=4}); ASSERT_TRUE(plan);
        for (unsigned i=0;i<3;++i) ASSERT_NO_FATAL_FAILURE(verifyOrder(*plan,edge(*source,joins[i]),delay[i]));
        EXPECT_EQ(plan->delayTaskCount(),3U); EXPECT_EQ(plan->operationSchedule().size(),11U);
        std::array<float,128> input{}; std::array<std::array<float,128>,4> rendered{};
        std::array<float*,4> outputs{}; for (unsigned c=0;c<4;++c) outputs[c]=rendered[c].data();
        const std::array<const float*,1> inputs{input.data()};
        for (unsigned block=0;block<6;++block) {
            for (unsigned f=0;f<128;++f) input[f]=static_cast<float>(block*128+f+1);
            ASSERT_TRUE(plan->exec(BackendBlock{inputs,outputs,128}));
            for (unsigned c=0;c<4;++c) for (unsigned f=0;f<128;++f) {
                const auto n=block*128+f;
                const float expected=c==3 ? static_cast<float>(n+1) :
                    (n<delay[c] ? 0.F : 2.F*static_cast<float>(n-delay[c]+1));
                EXPECT_EQ(rendered[c][f],expected)<<"lane="<<c<<" n="<<n;
            }
        }
    }

    TEST(DelayTaskOrder, CandidateFailureLeavesActiveScheduleAndHistoryUntouched) {
        DAG graph; PlanSwitcher switcher;
        const auto source=graph.add<UniformDelayNode>(0U),sink=graph.add<UniformDelayNode>(0U,ports(),0U);
        ASSERT_TRUE(source); ASSERT_TRUE(sink); const auto connection=edge(*source,*sink);
        ASSERT_TRUE(connect(graph,connection)); ASSERT_TRUE(graph.markAsOutput(*sink));
        const std::array first{ConnectionDelay{connection,3}},second{ConnectionDelay{connection,512}};
        BackendConfig cfg{.maxFrames=4,.inputChannels=1,.outputChannels=1,.connectionDelays=first};
        ASSERT_TRUE(switcher.initialize(graph,cfg)); const auto* active=switcher.slotAddress(0);
        const std::vector<PlanOperationInfo> before(active->operationSchedule().begin(),active->operationSchedule().end());
        const auto* pointer=active->operationSchedule().data();
        std::array<float,4> input{1,2,3,4},output{};
        const std::array<const float*,1> inputs{input.data()}; const std::array outputs{output.data()};
        switcher.exec(mka::audio::AudioProcessContext{inputs,outputs,4,4,4});
        EXPECT_EQ(output,(std::array<float,4>{0,0,0,1}));
        cfg.connectionDelays=second; cfg.memoryBudgetBytes=0;
        const auto rejected=switcher.prepareAndPublish(graph,cfg); ASSERT_FALSE(rejected);
        EXPECT_EQ(rejected.error().code,ErrorCode::MemoryBudgetExceeded);
        EXPECT_EQ(active->operationSchedule().data(),pointer);
        EXPECT_TRUE(std::equal(before.begin(),before.end(),active->operationSchedule().begin()));
        input={5,6,7,8}; switcher.exec(mka::audio::AudioProcessContext{inputs,outputs,4,4,4});
        EXPECT_EQ(output,(std::array<float,4>{2,3,4,5})); EXPECT_EQ(switcher.audioGeneration(),1U);
        EXPECT_EQ(switcher.handshakeState(),HandshakeState::Idle);
    }
}
