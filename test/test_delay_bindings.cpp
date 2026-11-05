/**
 * @file test_delay_bindings.cpp
 * @brief Tests for delay bindings.
 * @author Mika
 */
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
    /// @brief Collects numeric callback, binding or lifetime observations for the fixture.
    struct Trace { /** Borrowed callback context active only during a synchronous render call. */ const mka::audio::AudioProcessContext* current=nullptr; /** Accumulated result of numeric context, order or binding observations. */ bool correct=true; /** Number of processor invocations observed by the fixture. */ unsigned calls=0; };
    /// @brief Routes logical samples to physical lanes while recording callback observations.
    class Router final : public Node {
    public:
        /// @brief Borrow binding observations and select whether this instance performs the final write.
        Router(NodeID id,Trace& trace,bool last) : Node(id,ports()),trace_(trace),last_(last) {}
        /// @brief Observe direct physical bindings, sum main/Aux signals and perform the selected final writes.
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
        /// @brief Borrowed lifetime or binding trace; it must outlive the fixture processor.
        Trace& trace_;
        /// @brief Whether this fixture instance performs the final physical write.
        const bool last_;
    };
    /// @brief Owns a fixture DAG and the node identities used to build its signal paths.
    struct Graph {
        /// @brief Externally observed lifetime, order or context data used by this fixture.
        Trace trace;
        /// @brief Owned desired DAG or detached graph values used by this fixture.
        DAG graph;
        NodeID fast=0 /**< Fast retained by the fixture. */,
            slow=0 /**< Identity of the desired delayed-branch processor. */,
            route=0 /**< Route retained by the fixture. */,
            last=0 /**< Generation observed at the final processing observation in a callback. */,
            side=0 /**< Side retained by the fixture. */;
        /// @brief Build the selected fixture topology and mark its physical-output processors.
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
    /// @brief Owns sample buffers and callback views that remain stable for one render call.
    struct Bank {
        /// @brief Owned input sample storage used by the fixture.
        std::array<float,9> input{};
        /// @brief Owned output sample storage or selected output-node identity.
        std::array<std::array<float,9>,4> output{};
        /// @brief Input channel descriptors or physical sample pointers used by this state.
        std::array<const float*,1> inputs{input.data()+1};
        /// @brief Output channel descriptors or physical sample pointers used by this state.
        std::array<float*,4> outputs{};
        /// @brief Bind stable callback pointers to the owned sample arrays and their guarded ranges.
        Bank() { input.fill(-999.F); for (unsigned c=0;c<4;++c) outputs[c]=output[c].data()+1; reset(); }
        /// @brief Disable copying to preserve stable ownership and borrowed addresses.
        Bank(const Bank&)=delete;
        /// @brief Disable copying to preserve stable ownership and borrowed addresses.
        Bank& operator=(const Bank&)=delete;
        /// @brief Restore sample guards or observation counters before the next fixture invocation.
        void reset() { for (auto& c : output) c.fill(-999.F); }
        /// @brief Borrow the selected physical buffers with explicit frame and channel capacities.
        mka::audio::AudioProcessContext io(FrameCount frames) { return {inputs,outputs,frames,7,7}; }
        /// @brief Initialize the selected input samples and reset output guards before rendering.
        void fill(std::size_t n,FrameCount frames) {
            for (FrameCount f=0;f<frames;++f) input[f+1]=static_cast<float>(n+f+1);
            reset();
        }
    };
    /// @brief Prepared backend shape and quotas shared by this fixture's cases.
    constexpr BackendConfig cfg{.maxFrames=7,.inputChannels=1,.outputChannels=4,.sampleRate=48000};
    /// @brief Compute the independent delayed-ramp sample, including initial silence.
    float delayed(std::size_t n,std::size_t delay,float gain=1.F) { return n<delay ? 0.F : gain*static_cast<float>(n-delay+1); }
    /// @brief Compare routed samples and untouched guard regions with the delayed reference stream.
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

    /// @brief Test case: main auxiliary and output successor stay bound while hardware banks and frame counts change.
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

    /// @brief Test case: ready generation owns distinct compensated buffers and failed candidate cannot rebind active.
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

    /// @brief Test case: rejected and empty callbacks do not advance the compensation or plugin histories.
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
