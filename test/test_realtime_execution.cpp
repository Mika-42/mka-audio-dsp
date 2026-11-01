#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <thread>
#include <utility>
#include <gtest/gtest.h>
import mka.audio.dsp.dag;

namespace {
    using namespace mka::audio::dsp;
    using mka::audio::AudioProcessContext;
    struct Trace { std::array<NodeID,3> order{}; std::size_t count=0; };
    thread_local Trace* trace=nullptr;
    class RealtimeNode final : public Node {
    public:
        RealtimeNode(NodeID id, bool consumer=false, bool writer=true)
            : Node(id,consumer ? Config{.layoutInput={0},.layoutOutput={0}} : Config{.layoutOutput={0}}),
              consumer_(consumer),writer_(writer) {}
        void process(const ProcessContext& ctx) noexcept override {
            ++calls; frames=ctx.frames; physicalOutputs=ctx.backendOutputs.size();
            if (trace) trace->order[trace->count++]=id();
            if (!consumer_ && ctx.frames && !ctx.inputs.empty()) firstPhysicalSample=ctx.inputs[0].buffer[0];
            for (FrameCount i=0;i<ctx.frames;++i) {
                if (consumer_) inputIntact=inputIntact && ctx.inputs[0].buffer[i]==2.F;
                ctx.outputs[0].buffer[i]=2.F;
                if (writer_ && !ctx.backendOutputs.empty() && (!consumer_ || i%2==0))
                    ctx.backendOutputs[0].buffer[i]=consumer_ ? 5.F : 2.F;
            }
        }
        unsigned calls=0;
        FrameCount frames=0;
        std::size_t physicalOutputs=0;
        float firstPhysicalSample=0;
        bool inputIntact=true;
    private:
        bool consumer_,writer_;
    };

    TEST(RealtimeExecution, CanonicalCallbacksFollowCompiledOrderAndPerSampleLastWriterWins) {
        DAG graph;
        const auto source=graph.add<RealtimeNode>(), sink=graph.add<RealtimeNode>(true), observer=graph.add<RealtimeNode>(false,false);
        ASSERT_TRUE(source); ASSERT_TRUE(sink); ASSERT_TRUE(observer);
        ASSERT_TRUE(graph.connect(encode(*sink,PortType::In,0),encode(*source,PortType::Out,0)));
        ASSERT_TRUE(graph.markAsOutput(*source)); ASSERT_TRUE(graph.markAsOutput(*sink));
        auto plan=graph.compile(BackendConfig{.maxFrames=4,.inputChannels=6,.outputChannels=6}); ASSERT_TRUE(plan);
        std::array<std::array<float,4>,6> inputA{},inputB{};
        std::array<std::array<float,5>,6> outputA{},outputB{};
        for (auto& lane : inputA) lane.fill(3.F);
        for (auto& lane : inputB) lane.fill(7.F);
        for (std::size_t count : {0U,2U,6U})
            for (FrameCount frames : {0U,1U,4U})
                for (unsigned block=0;block<2;++block) {
                    std::array<const float*,6> inputs{}; std::array<float*,6> outputs{};
                    for (std::size_t lane=0;lane<6;++lane) {
                        inputs[lane]=(block ? inputB : inputA)[lane].data();
                        (block ? outputB : outputA)[lane].fill(99.F);
                        (block ? outputA : outputB)[lane].fill(-77.F);
                        outputs[lane]=(block ? outputB : outputA)[lane].data();
                    }
                    Trace event; trace=&event;
                    plan->exec(AudioProcessContext{std::span{inputs}.first(count),std::span{outputs}.first(count),frames});
                    trace=nullptr;
                    EXPECT_EQ(event.count,3U); EXPECT_EQ(event.order,(std::array<NodeID,3>{*source,*sink,*observer}));
                    for (auto id : {*source,*sink,*observer})
                        EXPECT_EQ(static_cast<RealtimeNode*>(graph.node(id))->frames,frames);
                    EXPECT_TRUE(static_cast<RealtimeNode*>(graph.node(*sink))->inputIntact);
                    EXPECT_EQ(static_cast<RealtimeNode*>(graph.node(*observer))->physicalOutputs,0U);
                    if (count && frames)
                        EXPECT_EQ(static_cast<RealtimeNode*>(graph.node(*source))->firstPhysicalSample,block ? 7.F : 3.F);
                    for (std::size_t lane=0;lane<count;++lane) {
                        for (FrameCount i=0;i<frames;++i)
                            EXPECT_EQ(outputs[lane][i],lane==0 ? (i%2==0 ? 5.F : 2.F) : 0.F);
                        EXPECT_EQ(outputs[lane][frames],99.F);
                    }
                    for (const auto& lane : block ? outputA : outputB)
                        for (const auto sample : lane) EXPECT_EQ(sample,-77.F);
                }
        EXPECT_EQ(static_cast<RealtimeNode*>(graph.node(*source))->calls,18U);
        EXPECT_EQ(plan->callbackDiagnostics(),0U);
    }

    TEST(RealtimeExecution, InvalidShapesSilenceOnlyAnnouncedWritableSamplesAndRunNoTask) {
        DAG graph;
        const auto id=graph.add<RealtimeNode>(); ASSERT_TRUE(id); ASSERT_TRUE(graph.markAsOutput(*id));
        auto plan=graph.compile(BackendConfig{.maxFrames=3,.inputChannels=2,.outputChannels=2}); ASSERT_TRUE(plan);
        const std::array<float,3> input{1,2,3};
        const std::array<const float*,3> inputs{input.data(),input.data(),input.data()};
        std::array<std::array<float,5>,3> storage{};
        const std::array<float*,3> outputs{storage[0].data()+1,storage[1].data()+1,storage[2].data()+1};
        const auto normalInputs=std::span{inputs}.first(2);
        const auto normalOutputs=std::span{outputs}.first(2);
        plan->exec(AudioProcessContext{normalInputs,normalOutputs,3});
        const auto* logical=plan->executionTasks()[0].context.outputs[0].buffer;
        const std::array<const float*,1> nullInput{nullptr};
        const std::array<float*,2> nullOutput{outputs[0],nullptr};
        struct Case { AudioProcessContext io; CallbackDiagnostic diagnostic; FrameCount cleared; };
        const std::array cases{
            Case{AudioProcessContext{normalInputs,normalOutputs,4,3,3},CallbackDiagnostic::FrameCapacityExceeded,3},
            Case{AudioProcessContext{inputs,normalOutputs,2},CallbackDiagnostic::ChannelCapacityExceeded,2},
            Case{AudioProcessContext{normalInputs,outputs,2},CallbackDiagnostic::ChannelCapacityExceeded,2},
            Case{AudioProcessContext{normalInputs,normalOutputs,2,1,2},CallbackDiagnostic::BufferCapacityExceeded,2},
            Case{AudioProcessContext{normalInputs,normalOutputs,2,2,1},CallbackDiagnostic::BufferCapacityExceeded,1},
            Case{AudioProcessContext{nullInput,normalOutputs,2},CallbackDiagnostic::NullBuffer,2},
            Case{AudioProcessContext{normalInputs,nullOutput,2},CallbackDiagnostic::NullBuffer,2}};
        for (std::size_t c=0;c<cases.size();++c) {
            SCOPED_TRACE(c);
            for (auto& lane : storage) lane.fill(99.F);
            plan->exec(cases[c].io);
            EXPECT_EQ(plan->takeCallbackDiagnostics(),static_cast<std::uint32_t>(cases[c].diagnostic));
            EXPECT_EQ(static_cast<RealtimeNode*>(graph.node(*id))->calls,1U);
            for (std::size_t lane=0;lane<3;++lane) {
                bool selected=false;
                for (auto* out : cases[c].io.outputs) selected=selected || out==outputs[lane];
                for (std::size_t i=0;i<5;++i)
                    EXPECT_EQ(storage[lane][i],selected && i>0 && i<=cases[c].cleared ? 0.F : 99.F);
            }
            for (FrameCount i=0;i<3;++i) EXPECT_EQ(logical[i],2.F);
            EXPECT_TRUE(plan->executionTasks()[0].context.backendOutputs.empty());
        }
        plan->exec(AudioProcessContext{nullInput,nullOutput,0});
        EXPECT_EQ(static_cast<RealtimeNode*>(graph.node(*id))->calls,2U);
        EXPECT_EQ(plan->callbackDiagnostics(),0U);
    }

    TEST(RealtimeExecution, UnpreparedPlansRejectWithBoundedSilenceAndDiagnostic) {
        ExecutionPlan plan;
        std::array<float,5> output{99,99,99,99,99}; const std::array outputs{output.data()+1};
        plan.exec(AudioProcessContext{.outputs=outputs,.frames=3,.outputCapacityFrames=2});
        EXPECT_EQ(output,(std::array<float,5>{99,0,0,99,99}));
        EXPECT_EQ(plan.takeCallbackDiagnostics(),static_cast<std::uint32_t>(CallbackDiagnostic::PlanNotPrepared));
        EXPECT_EQ(plan.callbackDiagnostics(),0U);
    }

    TEST(RealtimeExecution, DiagnosticCopiesMovesAndPreparationAreGenerationLocal) {
        DAG graph;
        const auto id=graph.add<RealtimeNode>(); ASSERT_TRUE(id);
        const BackendConfig config{.maxFrames=1};
        auto plan=graph.compile(config); ASSERT_TRUE(plan);
        plan->exec(AudioProcessContext{.frames=2});
        const auto error=static_cast<std::uint32_t>(CallbackDiagnostic::FrameCapacityExceeded);
        ExecutionPlan copy=*plan;
        ExecutionPlan moved=std::move(copy);
        EXPECT_EQ(copy.callbackDiagnostics(),0U); EXPECT_EQ(moved.callbackDiagnostics(),error);
        EXPECT_EQ(plan->takeCallbackDiagnostics(),error); EXPECT_EQ(moved.callbackDiagnostics(),error);
        const auto refused=moved.prepareBackend(BackendConfig{.maxFrames=1,.memoryBudgetBytes=0},{},{});
        ASSERT_FALSE(refused); EXPECT_EQ(moved.callbackDiagnostics(),error);
        ASSERT_TRUE(moved.prepareBackend(config,{},{})); EXPECT_EQ(moved.callbackDiagnostics(),0U);
    }

    TEST(RealtimeExecution, ControlCanDrainAtomicDiagnosticsDuringRejectedAudioCallbacks) {
        DAG graph;
        const auto id=graph.add<RealtimeNode>(); ASSERT_TRUE(id);
        auto plan=graph.compile(BackendConfig{.maxFrames=1}); ASSERT_TRUE(plan);
        std::atomic<bool> done{false};
        std::thread audio{[&] {
            for (unsigned i=0;i<1000;++i) plan->exec(AudioProcessContext{.frames=2});
            done.store(true,std::memory_order_release);
        }};
        std::uint32_t observed=0;
        while (!done.load(std::memory_order_acquire)) observed |= plan->takeCallbackDiagnostics();
        audio.join(); observed |= plan->takeCallbackDiagnostics();
        EXPECT_EQ(observed,static_cast<std::uint32_t>(CallbackDiagnostic::FrameCapacityExceeded));
        EXPECT_EQ(static_cast<RealtimeNode*>(graph.node(*id))->calls,0U);
    }
}
