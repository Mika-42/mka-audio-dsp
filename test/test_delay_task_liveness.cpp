#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <map>
#include <span>
#include <vector>
#include <gtest/gtest.h>
import mka.audio.dsp.dag;
#include "pdc_render_fixture.hpp"

namespace {
    using namespace mka::audio::dsp;
    using namespace pdc_test;

    void compareAllLifetimes(const ExecutionPlan& plan,std::span<const Connection> edges) {
        const auto absent=std::numeric_limits<std::size_t>::max();
        const auto sequence=plan.operationSchedule();
        std::vector<SignalLifetime> expected(plan.tasks().size()+plan.delayTaskCount(),{absent,absent});
        std::map<NodeID,std::size_t> nodes;
        std::map<ChannelID,std::size_t> delays;
        for (std::size_t i=0;i<sequence.size();++i) {
            const auto& op=sequence[i]; ASSERT_EQ(op.outputCount,1U);
            ASSERT_LT(op.outputSignal,expected.size()); EXPECT_EQ(expected[op.outputSignal].producedAt,absent);
            expected[op.outputSignal]={i,i};
            if (op.kind==OperationKind::Node) nodes.emplace(op.node,i);
            else delays.emplace(op.connection.to,i);
        }
        for (const auto edge : edges) {
            SCOPED_TRACE(::testing::Message()<<"from="<<edge.from<<" to="<<edge.to);
            const auto source=sequence[nodes.at(decodeNode(edge.from))].outputSignal;
            const auto consumer=nodes.at(decodeNode(edge.to));
            if (const auto delayed=delays.find(edge.to); delayed!=delays.end()) {
                const auto& op=sequence[delayed->second]; EXPECT_EQ(op.connection,edge); EXPECT_EQ(op.sourceSignal,source);
                expected[source].lastUse=std::max(expected[source].lastUse,delayed->second);
                expected[op.outputSignal].lastUse=std::max(expected[op.outputSignal].lastUse,consumer);
            } else expected[source].lastUse=std::max(expected[source].lastUse,consumer);
        }
        const auto actual=plan.signalLifetimes(); ASSERT_EQ(actual.size(),expected.size());
        for (SignalID s=0;s<expected.size();++s) {
            SCOPED_TRACE(::testing::Message()<<"SignalID="<<s<<" producedAt="<<expected[s].producedAt<<" lastUse="<<expected[s].lastUse);
            EXPECT_EQ(actual[s].producedAt,expected[s].producedAt); EXPECT_EQ(actual[s].lastUse,expected[s].lastUse);
            ASSERT_LT(plan.signalSlots()[s],plan.bufferSlotCount());
            for (SignalID other=0;other<s;++other)
                if (plan.signalSlots()[s]==plan.signalSlots()[other])
                    EXPECT_TRUE(expected[s].lastUse<expected[other].producedAt || expected[other].lastUse<expected[s].producedAt)
                        <<"overlapping SignalID="<<other;
        }
    }

    struct Rendered { std::array<std::vector<float>,3> channels; std::uint32_t slots=0; };
    float ramp(std::size_t n,std::size_t delay,float gain=1.F) {
        return n<delay ? 0.F : gain*static_cast<float>(n-delay+1);
    }
    void renderAndVerify(SlotStrategy strategy,Rendered& result) {
        DAG graph;
        const auto root=graph.add<UniformDelayNode>(0U),fast=graph.add<UniformDelayNode>(3U),slow=graph.add<UniformDelayNode>(8U);
        const auto mix=graph.add<UniformDelayNode>(0U,ports(),1U),second=graph.add<UniformDelayNode>(0U);
        const auto sink=graph.add<UniformDelayNode>(0U,ports(),0U),late=graph.add<UniformDelayNode>(0U,ports(),2U);
        ASSERT_TRUE(root); ASSERT_TRUE(fast); ASSERT_TRUE(slow); ASSERT_TRUE(mix); ASSERT_TRUE(second); ASSERT_TRUE(sink); ASSERT_TRUE(late);
        const std::array edges{edge(*root,*fast),edge(*root,*slow),edge(*fast,*mix),edge(*slow,*mix,PortType::Aux),
            edge(*root,*second),edge(*fast,*second,PortType::Aux),edge(*mix,*sink),edge(*root,*sink,PortType::Aux),
            edge(*second,*late),edge(*root,*late,PortType::Aux)};
        for (const auto e : edges) ASSERT_TRUE(connect(graph,e));
        ASSERT_TRUE(graph.markAsOutput(*mix)); ASSERT_TRUE(graph.markAsOutput(*sink)); ASSERT_TRUE(graph.markAsOutput(*late));
        const BackendConfig cfg{.maxFrames=128,.inputChannels=1,.outputChannels=3,.sampleRate=48000,.slotStrategy=strategy};
        auto plan=graph.compile(cfg); ASSERT_TRUE(plan);
        ASSERT_NO_FATAL_FAILURE(compareAllLifetimes(*plan,edges));
        EXPECT_EQ(plan->delayTaskCount(),4U); EXPECT_EQ(plan->preparedMemoryUsage().historyBytes,19U*sizeof(float));
        EXPECT_EQ(plan->signalLifetimes().size(),11U); // Seven Node Out + four Delay Out, no history/silence.
        const auto* silence=context(*plan,*fast).auxiliary[0].buffer;
        for (const auto& task : plan->executionTasks()) for (const auto& out : task.context.outputs) EXPECT_NE(out.buffer,silence);
        std::size_t lastRootRead=0,lastFastRead=0;
        SignalID rootSignal=0,fastSignal=0;
        for (const auto& op : plan->operationSchedule()) if (op.kind==OperationKind::Node) {
            if (op.node==*root) rootSignal=op.outputSignal;
            if (op.node==*fast) fastSignal=op.outputSignal;
        }
        for (std::size_t i=0;i<plan->operationSchedule().size();++i) {
            const auto& op=plan->operationSchedule()[i];
            if (op.kind==OperationKind::Delay && op.sourceSignal==rootSignal) lastRootRead=i;
            if (op.kind==OperationKind::Node && op.node==*second) lastFastRead=i;
        }
        EXPECT_EQ(plan->signalLifetimes()[rootSignal].lastUse,lastRootRead);
        EXPECT_EQ(plan->signalLifetimes()[fastSignal].lastUse,lastFastRead);
        constexpr std::size_t total=386;
        for (auto& c : result.channels) c.resize(total); result.slots=plan->bufferSlotCount();
        std::array<float,130> input{}; input.fill(-999.F);
        std::array<std::array<float,130>,3> output{}; std::array<float*,3> outputs{};
        for (unsigned c=0;c<3;++c) outputs[c]=output[c].data()+1;
        const std::array<const float*,1> inputs{input.data()+1};
        constexpr std::array<FrameCount,7> sizes{0,1,7,128,2,0,7};
        std::size_t n=0,block=0; bool failedCandidate=false;
        const auto* contexts=plan->executionTasks().data();
        while (n<total) {
            if (n>=129 && !failedCandidate) {
                auto rejected=cfg; rejected.memoryBudgetBytes=plan->preparedMemoryUsage().totalBytes-1;
                const auto refused=graph.compile(rejected); ASSERT_FALSE(refused);
                EXPECT_EQ(refused.error().code,ErrorCode::MemoryBudgetExceeded);
                EXPECT_EQ(plan->executionTasks().data(),contexts); failedCandidate=true;
            }
            const auto frames=static_cast<FrameCount>(std::min<std::size_t>(sizes[block++%sizes.size()],total-n));
            for (FrameCount f=0;f<frames;++f) input[f+1]=static_cast<float>(n+f+1);
            const auto before=input; for (auto& c : output) c.fill(-999.F);
            ASSERT_TRUE(plan->exec(BackendBlock{inputs,outputs,frames})); EXPECT_EQ(input,before);
            for (unsigned c=0;c<3;++c) {
                for (FrameCount f=0;f<frames;++f) {
                    const auto expected=c==0 ? ramp(n+f,8,3) : c==1 ? ramp(n+f,8,2) : ramp(n+f,3,3);
                    EXPECT_EQ(output[c][f+1],expected)<<"lane="<<c<<" n="<<n+f;
                    result.channels[c][n+f]=output[c][f+1];
                }
                EXPECT_EQ(output[c].front(),-999.F);
                for (std::size_t f=frames+1;f<output[c].size();++f) EXPECT_EQ(output[c][f],-999.F);
            }
            n+=frames;
        }
        EXPECT_TRUE(failedCandidate);
        for (std::size_t t=0;t<total;++t) {
            EXPECT_EQ(node(graph,*root).producedLog[t],ramp(t,0));
            EXPECT_EQ(node(graph,*fast).producedLog[t],ramp(t,3));
            EXPECT_EQ(node(graph,*slow).producedLog[t],ramp(t,8));
            EXPECT_EQ(node(graph,*mix).producedLog[t],ramp(t,8,2));
            EXPECT_EQ(node(graph,*second).producedLog[t],ramp(t,3,2));
            EXPECT_EQ(node(graph,*sink).mainLog[t],ramp(t,8,2)); EXPECT_EQ(node(graph,*sink).auxLog[t],ramp(t,8));
            EXPECT_EQ(node(graph,*late).mainLog[t],ramp(t,3,2)); EXPECT_EQ(node(graph,*late).auxLog[t],ramp(t,3));
        }
        for (unsigned f=0;f<128;++f) EXPECT_EQ(silence[f],0.F);
        ASSERT_NO_FATAL_FAILURE(compareAllLifetimes(*plan,edges));
    }

    TEST(DelayTaskLiveness, RecycledAndUniquePoolsMatchAnIndependentSampleAndLastReaderOracle) {
        Rendered recycled,unique;
        ASSERT_NO_FATAL_FAILURE(renderAndVerify(SlotStrategy::Reuse,recycled));
        ASSERT_NO_FATAL_FAILURE(renderAndVerify(SlotStrategy::Unique,unique));
        EXPECT_LT(recycled.slots,unique.slots); EXPECT_EQ(unique.slots,11U);
        for (unsigned c=0;c<3;++c) EXPECT_EQ(recycled.channels[c],unique.channels[c]);
    }

    TEST(DelayTaskLiveness, RejectsPrematureReleaseSameOperationAliasAndMalformedDelayProductions) {
        const std::vector<SignalOperation> ops{{OperationKind::Node,{}, {0}},
            {OperationKind::Delay,{0},{1}},{OperationKind::Node,{1},{2}}};
        const std::vector<SignalLifetime> life{{0,1},{1,2},{2,2}};
        const SlotAssignment slots{{0,1,2},3,2};
        static_assert(noexcept(validateDelayLifetimes(std::span<const SignalOperation>{},
            std::span<const SignalLifetime>{},slots)));
        EXPECT_TRUE(validateDelayLifetimes(ops,life,slots));
        auto brokenLife=life; brokenLife[0].lastUse=0;
        EXPECT_EQ(validateDelayLifetimes(ops,brokenLife,slots).error().code,ErrorCode::InvalidParameter);
        brokenLife=life; brokenLife[0].producedAt=1; EXPECT_FALSE(validateDelayLifetimes(ops,brokenLife,slots));
        brokenLife=life; brokenLife[1].producedAt=0; EXPECT_FALSE(validateDelayLifetimes(ops,brokenLife,slots));
        brokenLife=life; brokenLife[1].lastUse=0; EXPECT_FALSE(validateDelayLifetimes(ops,brokenLife,slots));
        auto brokenSlots=slots; brokenSlots.slotOf[1]=0; EXPECT_FALSE(validateDelayLifetimes(ops,life,brokenSlots));
        brokenSlots=slots; brokenSlots.slotOf[0]=3; EXPECT_FALSE(validateDelayLifetimes(ops,life,brokenSlots));
        brokenSlots=slots; brokenSlots.slotOf[1]=3; EXPECT_FALSE(validateDelayLifetimes(ops,life,brokenSlots));
        brokenSlots=slots; brokenSlots.slotOf.pop_back(); EXPECT_FALSE(validateDelayLifetimes(ops,life,brokenSlots));
        auto malformed=ops; malformed[1].reads.clear(); EXPECT_FALSE(validateDelayLifetimes(malformed,life,slots));
        malformed=ops; malformed[1].writes.push_back(2); EXPECT_FALSE(validateDelayLifetimes(malformed,life,slots));
        malformed=ops; malformed[1].reads[0]=3; EXPECT_FALSE(validateDelayLifetimes(malformed,life,slots));
        malformed=ops; malformed[1].writes[0]=3; EXPECT_FALSE(validateDelayLifetimes(malformed,life,slots));
        EXPECT_TRUE(validateDelayLifetimes({}, {}, {}));
    }
}
