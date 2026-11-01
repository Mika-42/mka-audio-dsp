#include <array>
#include <cstddef>
#include <expected>
#include <optional>
#include <span>
#include <vector>

#include <gtest/gtest.h>

import mka.audio.dsp.dag;

namespace {
    using namespace mka::audio::dsp;

    class LayoutNode final : public Node {
    public:
        explicit LayoutNode(NodeID id, const Config& config = {}) : Node(id, config) {}

        void process(const ProcessContext& context) noexcept override {
            inputCount = context.inputs.size();
            backendCount = context.backendOutputs.size();
            for (FrameCount frame = 0; frame < context.frames; ++frame) {
                const float sample = (context.inputs.empty() ? 0.F : context.inputs[0].buffer[frame]) +
                                     static_cast<float>(id());
                for (const auto& output : context.outputs) output.buffer[frame] = sample;
                for (const auto& output : context.backendOutputs) output.buffer[frame] = sample;
            }
        }

        std::size_t inputCount = 0;
        std::size_t backendCount = 0; // Numeric observations; no retained callback pointers.
    };

    Config sparseConfig() {
        return Config{.layoutInput = {2, 6}, .layoutAuxiliary = {2, 6}, .layoutOutput = {2, 6}};
    }
    Config unitConfig() {
        return Config{.layoutInput = {0}, .layoutAuxiliary = {0}, .layoutOutput = {0}};
    }

    void expectError(const Result& result, GraphError error) {
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error(), error);
    }

    void expectEdges(const DAG& graph, std::span<const Connection> expected) {
        ASSERT_EQ(graph.connections().size(), expected.size());
        for (std::size_t i = 0; i < expected.size(); ++i)
            EXPECT_EQ(graph.connections()[i], expected[i]);
    }

    void expectConfig(const Node& node, const Config& expected) {
        EXPECT_EQ(node.config().layoutInput, expected.layoutInput);
        EXPECT_EQ(node.config().layoutAuxiliary, expected.layoutAuxiliary);
        EXPECT_EQ(node.config().layoutOutput, expected.layoutOutput);
    }

    TEST(UndeclaredChannels, EmptyAndSparseLayoutsAllowOutInAndAuxAtZeroAnd65535) {
        const std::array configs{Config{}, sparseConfig()};
        for (std::size_t producerLayout = 0; producerLayout < configs.size(); ++producerLayout) {
            for (std::size_t consumerLayout = 0; consumerLayout < configs.size(); ++consumerLayout) {
                SCOPED_TRACE(producerLayout);
                SCOPED_TRACE(consumerLayout);
                DAG graph;
                const auto a = graph.add<LayoutNode>(configs[producerLayout]);
                const auto b = graph.add<LayoutNode>(configs[consumerLayout]);
                ASSERT_TRUE(a); ASSERT_TRUE(b);
                const std::array expected{
                    Connection{encode(*a, PortType::Out, 0), encode(*b, PortType::In, 0)},
                    Connection{encode(*a, PortType::Out, 65535), encode(*b, PortType::Aux, 65535)},
                    Connection{encode(*a, PortType::Out, 65535), encode(*b, PortType::In, 65535)}};
                for (const auto& edge : expected) ASSERT_TRUE(graph.connect(edge.to, edge.from));
                ASSERT_NO_FATAL_FAILURE(expectEdges(graph, expected));
                ASSERT_TRUE(graph.compile(BackendConfig{.maxFrames = 1}));
            }
        }
    }

    TEST(UndeclaredChannels, ConfigReplacementPreservesIdentityAddressEdgesAndOutputMarkers) {
        DAG graph;
        const auto a = graph.add<LayoutNode>();
        const auto b = graph.add<LayoutNode>();
        ASSERT_TRUE(a); ASSERT_TRUE(b);
        Node* const producer = graph.node(*a);
        Node* const consumer = graph.node(*b);
        const std::array edges{
            Connection{encode(*a, PortType::Out, 0), encode(*b, PortType::In, 65535)},
            Connection{encode(*a, PortType::Out, 65535), encode(*b, PortType::Aux, 0)}};
        for (const auto& edge : edges) ASSERT_TRUE(graph.connect(edge.to, edge.from));
        ASSERT_TRUE(graph.markAsOutput(*b));
        Config caller = sparseConfig();
        ASSERT_TRUE(graph.setConfig(*a, caller));
        ASSERT_TRUE(graph.setConfig(*b, caller));
        caller.layoutInput.clear();
        caller.layoutOutput[0] = 42;
        ASSERT_NO_FATAL_FAILURE(expectConfig(*producer, sparseConfig()));
        ASSERT_NO_FATAL_FAILURE(expectConfig(*consumer, sparseConfig()));
        ASSERT_NO_FATAL_FAILURE(expectEdges(graph, edges));
        ASSERT_TRUE(graph.setConfig(*a, unitConfig()));
        ASSERT_TRUE(graph.setConfig(*b, Config{}));
        ASSERT_NO_FATAL_FAILURE(expectConfig(*producer, unitConfig()));
        ASSERT_NO_FATAL_FAILURE(expectConfig(*consumer, Config{}));
        EXPECT_EQ(graph.node(*a), producer);
        EXPECT_EQ(graph.node(*b), consumer);
        EXPECT_EQ(producer->id(), *a);
        EXPECT_EQ(consumer->id(), *b);
        ASSERT_NO_FATAL_FAILURE(expectEdges(graph, edges));
        ASSERT_EQ(graph.outputNodes().size(), 1U);
        EXPECT_EQ(graph.outputNodes()[0], *b);
        ASSERT_TRUE(graph.compile(BackendConfig{.maxFrames = 1, .outputChannels = 1}));
    }

    TEST(UndeclaredChannels, EveryPlanBorrowProtectsTheConfigAndExistingPreparedRouting) {
        DAG graph;
        const auto a = graph.add<LayoutNode>(unitConfig());
        const auto b = graph.add<LayoutNode>(unitConfig());
        ASSERT_TRUE(a); ASSERT_TRUE(b);
        auto* const producer = static_cast<LayoutNode*>(graph.node(*a));
        const Connection edge{encode(*a, PortType::Out, 0), encode(*b, PortType::In, 0)};
        ASSERT_TRUE(graph.connect(edge.to, edge.from));
        ASSERT_TRUE(graph.markAsOutput(*b));
        const BackendConfig config{.maxFrames = 1, .inputChannels = 1, .outputChannels = 1};
        auto active = graph.compile(config);
        ASSERT_TRUE(active);
        std::optional<ExecutionPlan> ready{*active};
        expectError(graph.setConfig(*a, sparseConfig()), GraphError::NodeInUse);
        ASSERT_NO_FATAL_FAILURE(expectConfig(*producer, unitConfig()));
        const std::array expected{edge};
        ASSERT_NO_FATAL_FAILURE(expectEdges(graph, expected));
        const float input = 2;
        float output = 99;
        const std::array inputs{&input};
        const std::array outputs{&output};
        ASSERT_TRUE(active->exec(BackendBlock{inputs, outputs, 1}));
        EXPECT_EQ(output, 5);
        active = std::unexpected(GraphError::NodeNotFound);
        expectError(graph.setConfig(*a, sparseConfig()), GraphError::NodeInUse);
        ASSERT_TRUE(ready->exec(BackendBlock{inputs, outputs, 1}));
        EXPECT_EQ(output, 5);
        ready.reset(); // All callbacks and plan borrows have quiesced on control.
        ASSERT_TRUE(graph.setConfig(*a, sparseConfig()));
        EXPECT_EQ(graph.node(*a), producer);
        ASSERT_NO_FATAL_FAILURE(expectEdges(graph, expected));
        auto replacement = graph.compile(config);
        ASSERT_TRUE(replacement); // Out(0) need not be produced by the new Config.
        ASSERT_TRUE(replacement->exec(BackendBlock{inputs, outputs, 1}));
        EXPECT_EQ(output, 2); // Consumer reads logical silence, not backend input.
        EXPECT_EQ(producer->inputCount, 1U); // Source still sees all physical inputs.
    }

    TEST(UndeclaredChannels, InvalidConfigOrMissingIdentityLeavesConfigAndStructureUnchanged) {
        DAG graph;
        const auto a = graph.add<LayoutNode>(sparseConfig());
        const auto b = graph.add<LayoutNode>();
        ASSERT_TRUE(a); ASSERT_TRUE(b);
        Node* const address = graph.node(*a);
        const Connection edge{encode(*a, PortType::Out, 65535), encode(*b, PortType::In, 65535)};
        ASSERT_TRUE(graph.connect(edge.to, edge.from));
        ASSERT_TRUE(graph.markAsOutput(*b));
        const Config duplicate{.layoutInput = {2, 2}};
        Config excessive;
        excessive.layoutOutput.assign(maxLayoutChannels + 1, ChannelIndex{0});
        expectError(graph.setConfig(*a, duplicate), GraphError::DuplicateChannelIndex);
        expectError(graph.setConfig(*a, excessive), GraphError::LayoutCapacityExceeded);
        expectError(graph.setConfig(invalidNodeID, unitConfig()), GraphError::NodeNotFound);
        expectError(graph.setConfig(maxNodeID, unitConfig()), GraphError::NodeNotFound);
        ASSERT_TRUE(graph.setConfig(*a, graph.node(*a)->config())); // Alias-safe copy before commit.
        EXPECT_EQ(graph.node(*a), address);
        ASSERT_NO_FATAL_FAILURE(expectConfig(*address, sparseConfig()));
        const std::array expected{edge};
        ASSERT_NO_FATAL_FAILURE(expectEdges(graph, expected));
        ASSERT_EQ(graph.outputNodes().size(), 1U);
        EXPECT_EQ(graph.outputNodes()[0], *b);
        ASSERT_TRUE(graph.compile(BackendConfig{.maxFrames = 1}));
    }

    TEST(UndeclaredChannels, UndeclaredIndicesStillObeyPortsOccupancyAndAcyclicity) {
        DAG graph;
        const auto a = graph.add<LayoutNode>(sparseConfig());
        const auto b = graph.add<LayoutNode>(sparseConfig());
        const auto c = graph.add<LayoutNode>(sparseConfig());
        ASSERT_TRUE(a); ASSERT_TRUE(b); ASSERT_TRUE(c);
        const Connection existing{encode(*a, PortType::Out, 0), encode(*b, PortType::In, 65535)};
        ASSERT_TRUE(graph.connect(existing.to, existing.from));
        ASSERT_TRUE(graph.markAsOutput(*c));
        struct Candidate { ChannelID to; ChannelID from; GraphError error; };
        const auto malformed = (ChannelID{*a} << 32U) | (ChannelID{256} << 16U);
        const std::array rejected{
            Candidate{existing.to, encode(*c, PortType::Out, 65535), GraphError::InputOccupied},
            Candidate{existing.to, existing.from, GraphError::ConnectionExists},
            Candidate{encode(*a, PortType::Aux, 65535), encode(*b, PortType::Out, 65535), GraphError::CycleDetected},
            Candidate{encode(*c, PortType::In, 0), encode(*a, PortType::In, 0), GraphError::InvalidDirection},
            Candidate{encode(*c, PortType::Aux, 65535), encode(maxNodeID, PortType::Out, 65535), GraphError::NodeNotFound},
            Candidate{encode(*c, PortType::In, 0), malformed, GraphError::InvalidPort}};
        const std::array expected{existing};
        for (const auto& edge : rejected) {
            SCOPED_TRACE(static_cast<int>(edge.error));
            expectError(graph.connect(edge.to, edge.from), edge.error);
            ASSERT_NO_FATAL_FAILURE(expectEdges(graph, expected));
            ASSERT_NO_FATAL_FAILURE(expectConfig(*graph.node(*a), sparseConfig()));
            ASSERT_EQ(graph.outputNodes().size(), 1U);
            EXPECT_EQ(graph.outputNodes()[0], *c);
        }
        const auto disconnected = graph.disconnect(existing.to, existing.from);
        ASSERT_TRUE(disconnected);
        ASSERT_TRUE(*disconnected);
        ASSERT_TRUE(graph.connect(encode(*a, PortType::Aux, 65535), encode(*b, PortType::Out, 65535)));
    }
}
