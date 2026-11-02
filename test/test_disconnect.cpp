#include <array>
#include <concepts>
#include <expected>
#include <utility>
#include <vector>
#include <gtest/gtest.h>

import mka.audio.dsp.dag;

namespace {
    using namespace mka::audio::dsp;
    class DisconnectNode final : public Node {
    public:
        explicit DisconnectNode(NodeID id) : Node(id, Config{.layoutInput={0}, .layoutOutput={0}}) {}
        void process(const ProcessContext& ctx) noexcept override {
            for (FrameCount i=0; i<ctx.frames; ++i) {
                const float sample = (ctx.inputs.empty() ? 0.F : ctx.inputs[0].buffer[i]) + id();
                if (!ctx.outputs.empty()) ctx.outputs[0].buffer[i] = sample;
                if (!ctx.backendOutputs.empty()) ctx.backendOutputs[0].buffer[i] = sample;
            }
        }
    };
    static_assert(std::same_as<decltype(std::declval<DAG&>().disconnect(ChannelID{},ChannelID{})),
                               std::expected<bool,GraphError>>);
    void expectRemoved(const std::expected<bool,GraphError>& result, bool value) {
        ASSERT_TRUE(result);
        EXPECT_EQ(*result,value);
    }

    TEST(ExactDisconnect, RemovesOnlyTheNamedChannelAndAuxWhilePreservingOtherEdges) {
        DAG graph;
        auto a=graph.add<DisconnectNode>(); auto b=graph.add<DisconnectNode>();
        auto c=graph.add<DisconnectNode>();
        ASSERT_TRUE(a); ASSERT_TRUE(b); ASSERT_TRUE(c);
        const auto out=encode(*a,PortType::Out,0);
        const auto in=encode(*b,PortType::In,0);
        const auto aux=encode(*b,PortType::Aux,0);
        const auto high=encode(*b,PortType::In,65535);
        ASSERT_TRUE(graph.connect(in,out)); ASSERT_TRUE(graph.connect(aux,out));
        ASSERT_TRUE(graph.connect(high,out));
        // An absent exact pair does not erase another output of the same Node.
        expectRemoved(graph.disconnect(in,encode(*a,PortType::Out,1)),false);
        ASSERT_EQ(graph.connections().size(),3U);
        expectRemoved(graph.disconnect(aux,out),true);
        expectRemoved(graph.disconnect(aux,out),false);
        ASSERT_EQ(graph.connections().size(),2U);
        EXPECT_EQ(graph.connections()[0],(Connection{out,in}));
        EXPECT_EQ(graph.connections()[1],(Connection{out,high}));
        ASSERT_TRUE(graph.connect(aux,encode(*c,PortType::Out,42)));
        expectRemoved(graph.disconnect(high,out),true);
        EXPECT_EQ(graph.connections().size(),2U);
    }

    TEST(ExactDisconnect, AbsentPairIsSuccessfulFalseOnEmptyGraphWithLiveEndpoints) {
        DAG graph;
        auto a=graph.add<DisconnectNode>(); auto b=graph.add<DisconnectNode>();
        ASSERT_TRUE(a); ASSERT_TRUE(b);
        const auto in=encode(*b,PortType::In,65535), out=encode(*a,PortType::Out,65535);
        expectRemoved(graph.disconnect(in,out),false);
        expectRemoved(graph.disconnect(in,out),false);
        ASSERT_TRUE(graph.connect(in,out));
        expectRemoved(graph.disconnect(in,out),true);
        expectRemoved(graph.disconnect(in,out),false);
        EXPECT_TRUE(graph.connections().empty());
    }

    TEST(ExactDisconnect, MalformedDirectionsAndMissingNodesPreserveAllEdges) {
        DAG graph;
        auto a=graph.add<DisconnectNode>(); auto b=graph.add<DisconnectNode>();
        ASSERT_TRUE(a); ASSERT_TRUE(b);
        const auto out=encode(*a,PortType::Out,0), in=encode(*b,PortType::In,0);
        ASSERT_TRUE(graph.connect(in,out));
        struct Case { ChannelID destination,source; GraphError error; };
        const auto malformed=(ChannelID{*a}<<32U)|(ChannelID{0x8001}<<16U);
        for (const auto& item : std::array{
            Case{out,in,GraphError::InvalidDirection},
            Case{in,malformed,GraphError::InvalidDirection},
            Case{malformed,out,GraphError::InvalidDirection},
            Case{encode(maxNodeID,PortType::In,0),out,GraphError::NodeNotFound},
            Case{in,encode(maxNodeID,PortType::Out,0),GraphError::NodeNotFound}}) {
            auto result=graph.disconnect(item.destination,item.source);
            ASSERT_FALSE(result); EXPECT_EQ(result.error().code,item.error.code);
            ASSERT_EQ(graph.connections().size(),1U);
            EXPECT_EQ(graph.connections()[0],(Connection{out,in}));
        }
    }

    TEST(ExactDisconnect, ExistingPlanKeepsItsRoutingAfterAnExactRemoval) {
        DAG graph;
        auto a=graph.add<DisconnectNode>(); auto b=graph.add<DisconnectNode>();
        ASSERT_TRUE(a); ASSERT_TRUE(b);
        const auto out=encode(*a,PortType::Out,0), in=encode(*b,PortType::In,0);
        ASSERT_TRUE(graph.connect(in,out)); ASSERT_TRUE(graph.markAsOutput(*b));
        const BackendConfig config{.maxFrames=1,.inputChannels=1,.outputChannels=1};
        auto old=graph.compile(config); ASSERT_TRUE(old);
        expectRemoved(graph.disconnect(in,out),true);
        auto replacement=graph.compile(config); ASSERT_TRUE(replacement);
        const float input=2; float output=99;
        const std::array inputs{&input}; const std::array outputs{&output};
        ASSERT_TRUE(old->exec(BackendBlock{inputs,outputs,1})); EXPECT_EQ(output,5);
        ASSERT_TRUE(replacement->exec(BackendBlock{inputs,outputs,1})); EXPECT_EQ(output,4);
        ASSERT_TRUE(old->exec(BackendBlock{inputs,outputs,1})); EXPECT_EQ(output,5);
        EXPECT_EQ(graph.connections().size(),0U);
    }
}
