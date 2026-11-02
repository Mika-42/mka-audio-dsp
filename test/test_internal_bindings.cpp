#include <array>
#include <cstddef>
#include <gtest/gtest.h>
import mka.audio.dsp.dag;

namespace {
    using namespace mka::audio::dsp;
    using mka::audio::AudioProcessContext;
    class BindingNode final : public Node {
    public:
        BindingNode(NodeID id, bool producer, std::size_t lane=0)
            : Node(id,producer ? Config{.layoutOutput={0,1}} :
                Config{.layoutInput={3},.layoutAuxiliary={4},.layoutOutput={0}}),
              producer_(producer),lane_(lane) {}
        void process(const ProcessContext& context) noexcept override {
            ++calls;
            for (FrameCount i=0;i<context.frames;++i) {
                float result;
                if (producer_) {
                    const float input=context.inputs.empty() ? value : context.inputs[0].buffer[i];
                    result=input+1.F;
                    context.outputs[0].buffer[i]=result;
                    context.outputs[1].buffer[i]=input*2.F;
                } else {
                    result=context.inputs[0].buffer[i]+context.auxiliary[0].buffer[i];
                    context.outputs[0].buffer[i]=result;
                }
                if (lane_<context.backendOutputs.size()) context.backendOutputs[lane_].buffer[i]=result;
            }
        }
        float value=0;
        unsigned calls=0;
    private:
        bool producer_;
        std::size_t lane_;
    };

    TEST(InternalBindings, FanOutAndAuxiliaryShareOnlyTheIntendedProducerStorage) {
        DAG graph;
        const auto source=graph.add<BindingNode>(true), first=graph.add<BindingNode>(false,0), second=graph.add<BindingNode>(false,1);
        ASSERT_TRUE(source); ASSERT_TRUE(first); ASSERT_TRUE(second);
        ASSERT_TRUE(graph.connect(encode(*first,PortType::In,3),encode(*source,PortType::Out,0)));
        ASSERT_TRUE(graph.connect(encode(*first,PortType::Aux,4),encode(*source,PortType::Out,1)));
        ASSERT_TRUE(graph.connect(encode(*second,PortType::Aux,4),encode(*source,PortType::Out,0)));
        ASSERT_TRUE(graph.connect(encode(*second,PortType::Aux,65535),encode(*source,PortType::Out,65535)));
        for (auto id : {*source,*first,*second}) ASSERT_TRUE(graph.markAsOutput(id));
        auto plan=graph.compile(BackendConfig{.maxFrames=4,.inputChannels=1,.outputChannels=2}); ASSERT_TRUE(plan);
        const auto tasks=plan->executionTasks(); ASSERT_EQ(tasks.size(),3U);
        const auto* firstInput=tasks[1].context.inputs.data();
        const auto* producer=tasks[0].context.outputs[0].buffer;
        EXPECT_EQ(tasks[1].context.inputs[0].buffer,producer);
        EXPECT_EQ(tasks[2].context.auxiliary[0].buffer,producer);
        EXPECT_EQ(tasks[1].context.auxiliary[0].buffer,tasks[0].context.outputs[1].buffer);
        EXPECT_NE(tasks[2].context.inputs[0].buffer,producer);
        ASSERT_EQ(tasks[2].context.auxiliary.size(),1U); // Undeclared edges create no extra descriptors.
        EXPECT_EQ(tasks[1].context.inputs[0].channel,encode(*first,PortType::In,3));
        // S05 permits reuse AFTER the last reader; only live intervals must
        // have distinct outputs. The source's two fan-outs stay intact.
        const auto life=plan->signalLifetimes();
        ASSERT_EQ(life.size(),4U);
        EXPECT_EQ(life[0],(SignalLifetime{0,2}));
        EXPECT_EQ(life[1],(SignalLifetime{0,1}));
        std::array<const float*,4> buffers{tasks[0].context.outputs[0].buffer,
            tasks[0].context.outputs[1].buffer,tasks[1].context.outputs[0].buffer,
            tasks[2].context.outputs[0].buffer};
        for (SignalID a=0;a<4;++a) for (SignalID b=a+1;b<4;++b)
            if (life[a].producedAt<=life[b].lastUse && life[b].producedAt<=life[a].lastUse)
                EXPECT_NE(buffers[a],buffers[b]);
        EXPECT_LT(plan->bufferSlotCount(),buffers.size());
        const std::array<float,4> input{2,3,4,5},alternate{7,8,9,10};
        std::array<float,4> left{},right{},alternateLeft{},alternateRight{};
        for (unsigned block=0;block<2;++block) {
            const std::array<const float*,1> inputs{block ? alternate.data() : input.data()};
            const std::array outputs{block ? alternateLeft.data() : left.data(),block ? alternateRight.data() : right.data()};
            plan->exec(AudioProcessContext{inputs,outputs,4});
            EXPECT_EQ(tasks[1].context.inputs.data(),firstInput);
            EXPECT_EQ(tasks[0].context.outputs[0].buffer,producer);
            for (std::size_t i=0;i<4;++i) {
                EXPECT_EQ(outputs[0][i],3.F*inputs[0][i]+1.F);
                EXPECT_EQ(outputs[1][i],inputs[0][i]+1.F);
                EXPECT_EQ(producer[i],inputs[0][i]+1.F); // Physical writers never overwrite the logical slot.
            }
        }
        ExecutionPlan copy=*plan;
        const auto cloned=copy.executionTasks();
        EXPECT_NE(cloned[0].context.outputs[0].buffer,producer);
        EXPECT_EQ(cloned[1].context.inputs[0].buffer,cloned[0].context.outputs[0].buffer);
        EXPECT_EQ(cloned[2].context.auxiliary[0].buffer,cloned[0].context.outputs[0].buffer);
    }

    TEST(InternalBindings, DesiredGraphRewiringNeverChangesAnAlreadyPreparedBinding) {
        DAG graph;
        const auto a=graph.add<BindingNode>(true), b=graph.add<BindingNode>(true), sink=graph.add<BindingNode>(false);
        ASSERT_TRUE(a); ASSERT_TRUE(b); ASSERT_TRUE(sink);
        static_cast<BindingNode*>(graph.node(*a))->value=10;
        static_cast<BindingNode*>(graph.node(*b))->value=20;
        const auto destination=encode(*sink,PortType::In,3);
        ASSERT_TRUE(graph.connect(destination,encode(*a,PortType::Out,0)));
        ASSERT_TRUE(graph.markAsOutput(*sink));
        auto old=graph.compile(BackendConfig{.maxFrames=1,.outputChannels=1}); ASSERT_TRUE(old);
        const auto index=*old->taskIndexOf(*sink);
        const auto* bound=old->executionTasks()[index].context.inputs[0].buffer;
        ASSERT_TRUE(graph.disconnect(destination,encode(*a,PortType::Out,0)));
        ASSERT_TRUE(graph.connect(destination,encode(*b,PortType::Out,0)));
        const auto added=graph.add<BindingNode>(true); ASSERT_TRUE(added);
        float output=99; const std::array outputs{&output};
        old->exec(AudioProcessContext{.outputs=outputs,.frames=1}); EXPECT_EQ(output,11.F);
        EXPECT_EQ(old->executionTasks()[index].context.inputs[0].buffer,bound);
        EXPECT_FALSE(old->references(*added));
        auto next=graph.compile(BackendConfig{.maxFrames=1,.outputChannels=1}); ASSERT_TRUE(next);
        next->exec(AudioProcessContext{.outputs=outputs,.frames=1}); EXPECT_EQ(output,21.F);
        old->exec(AudioProcessContext{.outputs=outputs,.frames=1}); EXPECT_EQ(output,11.F);
    }

    TEST(InternalBindings, DuplicateAndOccupiedDestinationsAreDistinctAndPreserveTheTarget) {
        DAG graph;
        const auto a=graph.add<BindingNode>(true), b=graph.add<BindingNode>(true), sink=graph.add<BindingNode>(false);
        ASSERT_TRUE(a); ASSERT_TRUE(b); ASSERT_TRUE(sink);
        const Connection edge{encode(*a,PortType::Out,0),encode(*sink,PortType::Aux,4)};
        ASSERT_TRUE(graph.connect(edge.to,edge.from));
        auto plan=graph.compile(BackendConfig{.maxFrames=1}); ASSERT_TRUE(plan);
        const auto tasks=plan->executionTasks();
        const auto* old=tasks[2].context.auxiliary[0].buffer;
        const std::array duplicate{edge,edge};
        auto result=plan->prepareBackend(BackendConfig{},duplicate,{});
        ASSERT_FALSE(result); EXPECT_EQ(result.error(),(Error{ErrorCode::ConnectionExists,*sink,edge.to}));
        const std::array occupied{edge,Connection{encode(*b,PortType::Out,0),edge.to}};
        result=plan->prepareBackend(BackendConfig{},occupied,{});
        ASSERT_FALSE(result); EXPECT_EQ(result.error(),(Error{ErrorCode::InputOccupied,*sink,edge.to}));
        EXPECT_EQ(plan->executionTasks().data(),tasks.data());
        EXPECT_EQ(tasks[2].context.auxiliary[0].buffer,old); EXPECT_TRUE(plan->isBackendPrepared());
    }
}
