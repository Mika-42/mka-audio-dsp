#include <array>
#include <cstddef>
#include <gtest/gtest.h>
import mka.audio.dsp.dag;

namespace {
    using namespace mka::audio::dsp;
    using mka::audio::AudioProcessContext;
    enum class WriteMode { Full,Partial,None };
    class PartialProducer final : public Node {
    public:
        PartialProducer(NodeID id,float initial=90.F)
            : Node(id,Config{.layoutInput={0},.layoutAuxiliary={0},.layoutOutput={0,1}}),base(initial) {}
        void process(const ProcessContext& ctx) noexcept override {
            ++calls;
            for (FrameCount i=0;i<ctx.frames;++i) {
                if (mode==WriteMode::Full) {
                    ctx.outputs[0].buffer[i]=base+static_cast<float>(i);
                    ctx.outputs[1].buffer[i]=base+100.F+static_cast<float>(i);
                } else if (mode==WriteMode::Partial) {
                    if (i<ctx.frames/2) ctx.outputs[0].buffer[i]=8.F;
                    if (i%2) ctx.outputs[1].buffer[i]=9.F;
                }
            }
        }
        WriteMode mode=WriteMode::Full;
        float base;
        unsigned calls=0;
    };
    class OutputObserver final : public Node {
    public:
        OutputObserver(NodeID id,std::size_t lane)
            : Node(id,Config{.layoutInput={0},.layoutAuxiliary={0},.layoutOutput={0}}),lane_(lane) {}
        void process(const ProcessContext& ctx) noexcept override {
            ++calls;
            for (FrameCount i=0;i<ctx.frames;++i) {
                const auto a=ctx.inputs[0].buffer[i], b=ctx.auxiliary[0].buffer[i];
                ctx.outputs[0].buffer[i]=a+b;
                ctx.backendOutputs[lane_].buffer[i]=a;
                ctx.backendOutputs[lane_+1].buffer[i]=b;
            }
        }
        unsigned calls=0;
    private:
        std::size_t lane_;
    };
    void wire(DAG& graph,NodeID source,NodeID sink) {
        ASSERT_TRUE(graph.connect(encode(sink,PortType::In,0),encode(source,PortType::Out,0)));
        ASSERT_TRUE(graph.connect(encode(sink,PortType::Aux,0),encode(source,PortType::Out,1)));
        ASSERT_TRUE(graph.markAsOutput(sink));
    }

    TEST(LogicalOutputClearing, ReusedMultiOutputStorageHasZerosInEveryUnwrittenSampleAcrossCallbacks) {
        DAG graph;
        const auto producer=graph.add<PartialProducer>(), first=graph.add<OutputObserver>(0), second=graph.add<OutputObserver>(2);
        ASSERT_TRUE(producer); ASSERT_TRUE(first); ASSERT_TRUE(second);
        ASSERT_NO_FATAL_FAILURE(wire(graph,*producer,*first));
        ASSERT_NO_FATAL_FAILURE(wire(graph,*producer,*second));
        constexpr FrameCount maximum=128;
        auto plan=graph.compile(BackendConfig{.maxFrames=maximum,.outputChannels=5}); ASSERT_TRUE(plan);
        const auto tasks=plan->executionTasks();
        const auto* out0=tasks[0].context.outputs[0].buffer;
        const auto* out1=tasks[0].context.outputs[1].buffer;
        const auto* silence=tasks[0].context.auxiliary[0].buffer;
        EXPECT_NE(out0,out1); EXPECT_NE(out0,silence); EXPECT_NE(out1,silence);
        EXPECT_EQ(tasks[1].context.inputs[0].buffer,out0);
        EXPECT_EQ(tasks[2].context.inputs[0].buffer,out0);
        std::array<std::array<float,maximum+1>,5> hardware{};
        const std::array outputs{hardware[0].data(),hardware[1].data(),hardware[2].data(),hardware[3].data(),hardware[4].data()};
        auto& node=*static_cast<PartialProducer*>(graph.node(*producer));
        for (FrameCount frames : {0U,1U,maximum})
            for (WriteMode mode : {WriteMode::Full,WriteMode::Partial,WriteMode::None}) {
                node.mode=mode; // Control, between synchronous callbacks.
                for (auto& lane : hardware) lane.fill(-500.F);
                plan->exec(AudioProcessContext{.outputs=outputs,.frames=frames});
                for (FrameCount i=0;i<frames;++i) {
                    const float a=mode==WriteMode::Full ? 90.F+static_cast<float>(i) :
                        (mode==WriteMode::Partial && i<frames/2 ? 8.F : 0.F);
                    const float b=mode==WriteMode::Full ? 190.F+static_cast<float>(i) :
                        (mode==WriteMode::Partial && i%2 ? 9.F : 0.F);
                    EXPECT_EQ(out0[i],a); EXPECT_EQ(out1[i],b);
                    EXPECT_EQ(hardware[0][i],a); EXPECT_EQ(hardware[2][i],a);
                    EXPECT_EQ(hardware[1][i],b); EXPECT_EQ(hardware[3][i],b);
                    EXPECT_EQ(hardware[4][i],0.F);
                }
                for (const auto& lane : hardware) EXPECT_EQ(lane[frames],-500.F);
                for (FrameCount i=0;i<maximum;++i) EXPECT_EQ(silence[i],0.F);
                EXPECT_EQ(tasks[0].context.outputs[0].buffer,out0);
            }
        EXPECT_EQ(node.calls,9U);
    }

    TEST(LogicalOutputClearing, NonSourceOutputsAreClearedWithoutDestroyingStillLivePredecessorFanOut) {
        DAG graph;
        const auto source=graph.add<PartialProducer>(90.F), middle=graph.add<PartialProducer>(300.F);
        const auto middleSink=graph.add<OutputObserver>(0), rootSink=graph.add<OutputObserver>(2);
        ASSERT_TRUE(source); ASSERT_TRUE(middle); ASSERT_TRUE(middleSink); ASSERT_TRUE(rootSink);
        ASSERT_TRUE(graph.connect(encode(*middle,PortType::In,0),encode(*source,PortType::Out,0)));
        ASSERT_NO_FATAL_FAILURE(wire(graph,*middle,*middleSink));
        ASSERT_NO_FATAL_FAILURE(wire(graph,*source,*rootSink));
        auto plan=graph.compile(BackendConfig{.maxFrames=8,.outputChannels=4}); ASSERT_TRUE(plan);
        const auto tasks=plan->executionTasks();
        EXPECT_EQ(tasks[1].context.inputs[0].buffer,tasks[0].context.outputs[0].buffer);
        EXPECT_NE(tasks[1].context.outputs[0].buffer,tasks[1].context.inputs[0].buffer);
        std::array<std::array<float,8>,4> hardware{};
        const std::array outputs{hardware[0].data(),hardware[1].data(),hardware[2].data(),hardware[3].data()};
        auto& processor=*static_cast<PartialProducer*>(graph.node(*middle));
        for (WriteMode mode : {WriteMode::Full,WriteMode::None}) {
            processor.mode=mode;
            plan->exec(AudioProcessContext{.outputs=outputs,.frames=8});
            for (std::size_t i=0;i<8;++i) {
                EXPECT_EQ(hardware[0][i],mode==WriteMode::Full ? 300.F+static_cast<float>(i) : 0.F);
                EXPECT_EQ(hardware[1][i],mode==WriteMode::Full ? 400.F+static_cast<float>(i) : 0.F);
                EXPECT_EQ(hardware[2][i],90.F+static_cast<float>(i));
                EXPECT_EQ(hardware[3][i],190.F+static_cast<float>(i));
            }
        }
        EXPECT_EQ(plan->callbackDiagnostics(),0U);
    }
}
