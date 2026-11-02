#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <type_traits>
#include <vector>
#include <gtest/gtest.h>
import mka.audio.dsp.plan_switcher;
#include "pdc_render_fixture.hpp"

namespace {
    using namespace mka::audio::dsp;
    using namespace pdc_test;
    static_assert(std::is_same_v<decltype(InputChannel{}.buffer),const float*>);
    struct Trace { const mka::audio::AudioProcessContext* current=nullptr; bool correct=true; unsigned calls=0; };
    class Router final : public Node {
    public:
        Router(NodeID id,Trace& trace,bool last) : Node(id,ports()),trace_(trace),last_(last) {}
        void process(const ProcessContext& ctx) noexcept override {
            ++trace_.calls;
            const auto* io=trace_.current;
            trace_.correct=trace_.correct && io && ctx.backendOutputs.size()==4 && io->frames==ctx.frames;
            if (io) for (std::size_t c=0;c<ctx.backendOutputs.size();++c) {
                trace_.correct=trace_.correct && ctx.backendOutputs[c].buffer==io->outputs[c] && ctx.backendOutputs[c].channel==0;
                for (const auto& logical : ctx.outputs)
                    trace_.correct=trace_.correct && logical.buffer!=ctx.backendOutputs[c].buffer;
            }
            for (FrameCount f=0;f<ctx.frames;++f) {
                const float value=ctx.inputs[0].buffer[f]+ctx.auxiliary[0].buffer[f];
                ctx.outputs[0].buffer[f]=value;
                if (last_) {
                    ctx.backendOutputs[1].buffer[f]=value;
                    if (f%2) ctx.backendOutputs[2].buffer[f]=value+200.F;
                } else {
                    ctx.backendOutputs[0].buffer[f]=value;
                    ctx.backendOutputs[2].buffer[f]=value+100.F;
                }
            }
        }
    private:
        Trace& trace_;
        const bool last_;
    };
    struct Graph {
        Trace trace;
        DAG graph;
        NodeID fast=0,slow=0,route=0,last=0,side=0;
        Result make() {
            const auto a=graph.add<UniformDelayNode>(3U),b=graph.add<UniformDelayNode>(8U,ports(),UINT32_MAX,2.F);
            const auto c=graph.add<Router>(trace,false),d=graph.add<Router>(trace,true),e=graph.add<UniformDelayNode>(0U,ports(),3U);
            if (!a || !b || !c || !d || !e) return std::unexpected(Error{ErrorCode::PrepareFailed});
            fast=*a; slow=*b; route=*c; last=*d; side=*e;
            for (const auto connection : {edge(fast,route),edge(slow,route,PortType::Aux),edge(route,last),edge(fast,side)})
                if (const auto result=connect(graph,connection); !result) return result;
            for (const auto id : {route,last,side})
                if (const auto result=graph.markAsOutput(id); !result) return result;
            return {};
        }
    };
    struct Bank {
        std::array<float,9> input{};
        std::array<std::array<float,9>,4> output{};
        std::array<const float*,1> inputs{input.data()+1};
        std::array<float*,4> outputs{};
        Bank() { input.fill(-999.F); for (unsigned c=0;c<4;++c) outputs[c]=output[c].data()+1; reset(); }
        Bank(const Bank&)=delete;
        Bank& operator=(const Bank&)=delete;
        void reset() { for (auto& c : output) c.fill(-999.F); }
        mka::audio::AudioProcessContext io(FrameCount frames) { return {inputs,outputs,frames,7,7}; }
        void fill(std::size_t n,FrameCount frames) {
            for (FrameCount f=0;f<frames;++f) input[f+1]=static_cast<float>(n+f+1);
            reset();
        }
    };
    constexpr BackendConfig cfg{.maxFrames=7,.inputChannels=1,.outputChannels=4,.sampleRate=48000};
    float delayed(std::size_t n,std::size_t delay,float gain=1.F) { return n<delay ? 0.F : gain*static_cast<float>(n-delay+1); }
    void checkSamples(const Bank& bank,std::size_t n,FrameCount frames) {
        for (unsigned c=0;c<4;++c) {
            for (FrameCount f=0;f<frames;++f) {
                const auto aligned=delayed(n+f,8,3);
                const auto expected=c==3 ? delayed(n+f,3) : c==2 ? aligned+(f%2 ? 200.F : 100.F) : aligned;
                EXPECT_EQ(bank.output[c][f+1],expected)<<"lane="<<c<<" n="<<n+f;
            }
            EXPECT_EQ(bank.output[c].front(),-999.F);
            for (std::size_t f=frames+1;f<bank.output[c].size();++f) EXPECT_EQ(bank.output[c][f],-999.F);
        }
    }

    TEST(DelayBindings, MainAuxiliaryAndOutputSuccessorStayBoundWhileHardwareBanksAndFrameCountsChange) {
        Graph p; ASSERT_TRUE(p.make()); auto plan=p.graph.compile(cfg); ASSERT_TRUE(plan);
        EXPECT_EQ(plan->pdcAnalysis()->find(edge(p.fast,p.route))->samples,5U);
        EXPECT_EQ(plan->pdcAnalysis()->find(edge(p.slow,p.route,PortType::Aux))->samples,0U);
        EXPECT_EQ(plan->delayTaskCount(),1U);
        const auto* main=context(*plan,p.route).inputs.data(); const auto* aux=context(*plan,p.route).auxiliary.data();
        const auto* delayedBuffer=main[0].buffer;
        EXPECT_NE(delayedBuffer,context(*plan,p.fast).outputs[0].buffer);
        EXPECT_EQ(aux[0].buffer,context(*plan,p.slow).outputs[0].buffer);
        EXPECT_EQ(main[0].channel,encode(p.route,PortType::In,0)); EXPECT_EQ(aux[0].channel,encode(p.route,PortType::Aux,0));
        EXPECT_EQ(context(*plan,p.last).inputs[0].buffer,context(*plan,p.route).outputs[0].buffer);
        EXPECT_EQ(context(*plan,p.side).inputs[0].buffer,context(*plan,p.fast).outputs[0].buffer);
        std::array<Bank,2> banks;
        std::size_t n=0; constexpr std::array<FrameCount,8> sizes{4,7,0,1,2,7,3,7};
        for (unsigned block=0;block<24;++block) {
            const auto frames=sizes[block%sizes.size()]; auto& bank=banks[block%2]; bank.fill(n,frames);
            const auto before=bank.input; const auto other=banks[(block+1)%2].output;
            auto io=bank.io(frames); p.trace.current=&io; plan->exec(io); p.trace.current=nullptr;
            checkSamples(bank,n,frames); EXPECT_EQ(bank.input,before); EXPECT_EQ(banks[(block+1)%2].output,other);
            EXPECT_EQ(context(*plan,p.route).inputs.data(),main); EXPECT_EQ(context(*plan,p.route).auxiliary.data(),aux);
            EXPECT_EQ(context(*plan,p.route).inputs[0].buffer,delayedBuffer);
            EXPECT_TRUE(context(*plan,p.fast).inputs.empty()); EXPECT_TRUE(context(*plan,p.slow).inputs.empty());
            for (const auto& task : plan->executionTasks()) { EXPECT_TRUE(task.context.backendOutputs.empty()); EXPECT_EQ(task.context.frames,0U); }
            if (block==12) {
                auto limited=cfg; limited.maxDelaySamples=4;
                const auto rejected=plan->prepareBackend(limited,p.graph.connections(),p.graph.outputNodes());
                ASSERT_FALSE(rejected); EXPECT_EQ(rejected.error().code,ErrorCode::MemoryBudgetExceeded);
                EXPECT_EQ(context(*plan,p.route).inputs.data(),main);
            }
            n+=frames;
        }
        EXPECT_TRUE(p.trace.correct);
    }

    TEST(DelayBindings, ReadyGenerationOwnsDistinctCompensatedBuffersAndFailedCandidateCannotRebindActive) {
        Graph p; ASSERT_TRUE(p.make()); PlanSwitcher switcher; ASSERT_TRUE(switcher.initialize(p.graph,cfg));
        const auto* active=switcher.slotAddress(0); const auto* bound=context(*active,p.route).inputs[0].buffer;
        Bank bank; std::size_t n=0;
        for (unsigned block=0;block<2;++block) {
            bank.fill(n,7); auto io=bank.io(7); p.trace.current=&io; switcher.exec(io); p.trace.current=nullptr;
            checkSamples(bank,n,7); n+=7;
        }
        ASSERT_TRUE(switcher.prepareInactivePlan(p.graph,cfg)); const auto* b=switcher.readyPlan(); ASSERT_NE(b,nullptr);
        EXPECT_NE(context(*b,p.route).inputs[0].buffer,bound);
        EXPECT_NE(context(*b,p.fast).outputs[0].buffer,context(*active,p.fast).outputs[0].buffer);
        EXPECT_EQ(context(*b,p.route).auxiliary[0].buffer,context(*b,p.slow).outputs[0].buffer);
        EXPECT_EQ(context(*b,p.last).inputs[0].buffer,context(*b,p.route).outputs[0].buffer);
        for (const auto* internal : std::array<const float*,2>{
            context(*b,p.fast).outputs[0].buffer,context(*b,p.route).inputs[0].buffer}) {
            EXPECT_NE(internal,bank.input.data()+1);
            for (const auto* physical : bank.outputs) EXPECT_NE(internal,physical);
        }
        ASSERT_TRUE(switcher.discardReadyPlan()); auto refused=cfg; refused.memoryBudgetBytes=0;
        EXPECT_EQ(switcher.prepareAndPublish(p.graph,refused).error().code,ErrorCode::MemoryBudgetExceeded);
        EXPECT_EQ(context(*active,p.route).inputs[0].buffer,bound);
        bank.fill(n,7); auto io=bank.io(7); p.trace.current=&io; switcher.exec(io); p.trace.current=nullptr;
        checkSamples(bank,n,7); EXPECT_TRUE(p.trace.correct); EXPECT_EQ(switcher.audioGeneration(),1U);
    }

    TEST(DelayBindings, RejectedAndEmptyCallbacksDoNotAdvanceTheCompensationOrPluginHistories) {
        Graph p; ASSERT_TRUE(p.make()); auto plan=p.graph.compile(cfg); ASSERT_TRUE(plan); Bank bank;
        std::size_t n=0;
        for (unsigned block=0;block<2;++block) {
            bank.fill(n,7); auto io=bank.io(7); p.trace.current=&io; plan->exec(io); p.trace.current=nullptr; n+=7;
        }
        const auto calls=p.trace.calls; const auto seen=node(p.graph,p.fast).seen;
        bank.reset(); auto invalid=bank.io(8); plan->exec(invalid);
        EXPECT_EQ(p.trace.calls,calls); EXPECT_EQ(node(p.graph,p.fast).seen,seen);
        EXPECT_EQ(plan->takeCallbackDiagnostics(),static_cast<std::uint32_t>(CallbackDiagnostic::FrameCapacityExceeded));
        for (const auto& c : bank.output) {
            EXPECT_EQ(c.front(),-999.F); EXPECT_EQ(c.back(),-999.F);
            for (unsigned f=1;f<=7;++f) EXPECT_EQ(c[f],0.F);
        }
        bank.reset(); auto empty=bank.io(0); p.trace.current=&empty; plan->exec(empty); p.trace.current=nullptr;
        EXPECT_EQ(node(p.graph,p.fast).seen,seen); EXPECT_EQ(plan->callbackDiagnostics(),0U);
        for (const auto& c : bank.output) for (const auto sample : c) EXPECT_EQ(sample,-999.F);
        bank.fill(n,7); auto io=bank.io(7); p.trace.current=&io; plan->exec(io); p.trace.current=nullptr;
        checkSamples(bank,n,7); EXPECT_TRUE(p.trace.correct);
    }
}
