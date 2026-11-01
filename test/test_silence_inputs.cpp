#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <gtest/gtest.h>

import mka.audio.dsp.dag;

namespace {
    using namespace mka::audio::dsp;
    using mka::audio::AudioProcessContext;
    static_assert(std::same_as<decltype(InputChannel::buffer),const float*>);

    class SilenceReader final : public Node {
    public:
        explicit SilenceReader(NodeID id, const Config& config=Config{
            .layoutInput={4,5},.layoutAuxiliary={2,3},.layoutOutput={0,1}}) : Node(id,config) {}
        void process(const ProcessContext& ctx) noexcept override {
            ++calls; inputCount=ctx.inputs.size(); auxiliaryCount=ctx.auxiliary.size();
            for (const auto& in : ctx.inputs)
                for (FrameCount i=0;i<ctx.frames;++i) allInputsZero=allInputsZero && in.buffer[i]==0.F;
            for (const auto& in : ctx.auxiliary)
                for (FrameCount i=0;i<ctx.frames;++i) allAuxiliaryZero=allAuxiliaryZero && in.buffer[i]==0.F;
            if (ctx.frames && !ctx.inputs.empty()) firstInputSample=ctx.inputs[0].buffer[0];
            for (const auto& out : ctx.outputs)
                for (FrameCount i=0;i<ctx.frames;++i) out.buffer[i]=7.F;
        }
        unsigned calls=0;
        std::size_t inputCount=0, auxiliaryCount=0;
        bool allInputsZero=true, allAuxiliaryZero=true;
        float firstInputSample=0;
    };

    TEST(SilenceInputs, UndeclaredDependenciesBindEveryUnconnectedDeclaredInAndAuxToOneConstSlot) {
        DAG graph;
        const auto source=graph.add<SilenceReader>(), first=graph.add<SilenceReader>(), second=graph.add<SilenceReader>();
        ASSERT_TRUE(source); ASSERT_TRUE(first); ASSERT_TRUE(second);
        for (const auto id : {*first,*second})
            ASSERT_TRUE(graph.connect(encode(id,PortType::Aux,65535),encode(*source,PortType::Out,65535)));
        constexpr FrameCount maximum=160;
        auto plan=graph.compile(BackendConfig{.maxFrames=maximum,.inputChannels=2,.outputChannels=2}); ASSERT_TRUE(plan);
        const auto tasks=plan->executionTasks(); ASSERT_EQ(tasks.size(),3U);
        const float* const silence=tasks[0].context.auxiliary[0].buffer; ASSERT_NE(silence,nullptr);
        for (std::size_t t=0;t<tasks.size();++t) {
            EXPECT_EQ(tasks[t].context.auxiliary.size(),2U);
            for (const auto& in : tasks[t].context.auxiliary) EXPECT_EQ(in.buffer,silence);
            for (const auto& in : tasks[t].context.inputs) EXPECT_EQ(in.buffer,silence);
            for (const auto& out : tasks[t].context.outputs) EXPECT_NE(out.buffer,silence);
        }
        EXPECT_TRUE(tasks[0].context.inputs.empty()); // Source uses backend, not Config input layouts.
        ASSERT_EQ(tasks[1].context.inputs.size(),2U);
        EXPECT_EQ(decodeChannel(tasks[1].context.inputs[0].channel),4U);
        std::array<float,maximum> left{},right{},output1{},output2{};
        left.fill(3.F); right.fill(4.F);
        const std::array<const float*,2> inputs{left.data(),right.data()};
        const std::array outputs{output1.data(),output2.data()};
        for (FrameCount frames : {0U,1U,128U,maximum,1U,maximum}) {
            output1.fill(99.F); output2.fill(99.F);
            plan->exec(AudioProcessContext{inputs,outputs,frames});
            for (FrameCount i=0;i<maximum;++i) EXPECT_EQ(silence[i],0.F);
            for (FrameCount i=0;i<frames;++i) { EXPECT_EQ(output1[i],0.F); EXPECT_EQ(output2[i],0.F); }
            EXPECT_EQ(tasks[1].context.inputs[0].buffer,silence);
        }
        const auto& root=*static_cast<SilenceReader*>(graph.node(*source));
        EXPECT_EQ(root.inputCount,2U); EXPECT_EQ(root.firstInputSample,3.F); EXPECT_TRUE(root.allAuxiliaryZero);
        for (const auto id : {*first,*second}) {
            const auto& reader=*static_cast<SilenceReader*>(graph.node(id));
            EXPECT_TRUE(reader.allInputsZero); EXPECT_TRUE(reader.allAuxiliaryZero); EXPECT_EQ(reader.calls,6U);
        }
    }

    TEST(SilenceInputs, EmptyGraphWithNoSourceSilencesOnlyTheRequestedPhysicalSamples) {
        DAG graph;
        auto plan=graph.compile(BackendConfig{.maxFrames=128,.outputChannels=2}); ASSERT_TRUE(plan);
        std::array<float,129> left{},right{};
        const std::array outputs{left.data(),right.data()};
        for (const FrameCount frames : {0U,1U,128U}) {
            left.fill(11.F); right.fill(12.F);
            plan->exec(AudioProcessContext{.outputs=outputs,.frames=frames});
            for (FrameCount i=0;i<frames;++i) { EXPECT_EQ(left[i],0.F); EXPECT_EQ(right[i],0.F); }
            EXPECT_EQ(left[frames],11.F); EXPECT_EQ(right[frames],12.F);
        }
        EXPECT_TRUE(plan->executionTasks().empty());
    }

    TEST(SilenceInputs, ZeroCapacityAndEmptyLayoutsExecuteZeroFramesWithoutSampleAccess) {
        DAG graph;
        const auto source=graph.add<SilenceReader>(Config{}), sink=graph.add<SilenceReader>();
        ASSERT_TRUE(source); ASSERT_TRUE(sink);
        ASSERT_TRUE(graph.connect(encode(*sink,PortType::Aux,65535),encode(*source,PortType::Out,65535)));
        auto plan=graph.compile(BackendConfig{}); ASSERT_TRUE(plan);
        plan->exec(AudioProcessContext{});
        EXPECT_EQ(static_cast<SilenceReader*>(graph.node(*source))->calls,1U);
        EXPECT_EQ(static_cast<SilenceReader*>(graph.node(*sink))->calls,1U);
        plan->exec(AudioProcessContext{.frames=1});
        EXPECT_EQ(static_cast<SilenceReader*>(graph.node(*sink))->calls,1U);
    }
}
