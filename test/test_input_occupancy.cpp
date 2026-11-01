#include <array>
#include <cstddef>
#include <span>
#include <vector>

#include <gtest/gtest.h>

import mka.audio.dsp.dag;

namespace {
    using namespace mka::audio::dsp;

    class OccupancyNode final : public Node {
    public:
        explicit OccupancyNode(const NodeID id) : Node(id, Config{}) {}
        void process(const ProcessContext& context) noexcept override {
            ++calls;
            inputCount = context.inputs.size();
            backendCount = context.backendOutputs.size();
        }
        unsigned calls = 0;
        std::size_t inputCount = 0;
        std::size_t backendCount = 0;
    };

    void expectError(const Result& result, const GraphError error) {
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, error.code);
    }

    void expectEdges(const DAG& graph, const std::span<const Connection> expected) {
        ASSERT_EQ(graph.connections().size(), expected.size());
        for (std::size_t i = 0; i < expected.size(); ++i)
            EXPECT_EQ(graph.connections()[i], expected[i]);
    }

    TEST(InputOccupancy, CompleteDestinationIdentityKeepsNodesPortsAndIndicesIndependent) {
        DAG graph;
        const auto source = graph.add<OccupancyNode>();
        const auto sink = graph.add<OccupancyNode>();
        const auto other = graph.add<OccupancyNode>();
        ASSERT_TRUE(source);
        ASSERT_TRUE(sink);
        ASSERT_TRUE(other);
        ASSERT_TRUE(graph.node(*sink)->config().layoutInput.empty());
        ASSERT_TRUE(graph.node(*sink)->config().layoutAuxiliary.empty());
        const auto out = encode(*source, PortType::Out, 65535);
        const std::array expected{
            Connection{out, encode(*sink, PortType::In, 0)},
            Connection{out, encode(*sink, PortType::Aux, 0)},
            Connection{out, encode(*sink, PortType::In, 1)},
            Connection{out, encode(*other, PortType::In, 0)}};
        for (const auto& edge : expected) ASSERT_TRUE(graph.connect(edge.to, edge.from));
        expectEdges(graph, expected);
        ASSERT_TRUE(graph.compile());
    }

    TEST(InputOccupancy, ASecondProducerIsRejectedForInAndAuxWithoutChangingTheOldPlan) {
        for (const auto port : {PortType::In, PortType::Aux}) {
            SCOPED_TRACE(static_cast<unsigned>(port));
            DAG graph;
            const auto first = graph.add<OccupancyNode>();
            const auto second = graph.add<OccupancyNode>();
            const auto sink = graph.add<OccupancyNode>();
            ASSERT_TRUE(first);
            ASSERT_TRUE(second);
            ASSERT_TRUE(sink);
            const auto destination = encode(*sink, port, 65535);
            const auto producer = encode(*first, PortType::Out, 42);
            ASSERT_TRUE(graph.connect(destination, producer));
            ASSERT_TRUE(graph.markAsOutput(*sink));
            const std::array expected{Connection{producer, destination}};
            auto plan = graph.compile(BackendConfig{.maxFrames = 1, .inputChannels = 1, .outputChannels = 1});
            ASSERT_TRUE(plan);
            const std::vector<Node*> tasks{plan->tasks().begin(), plan->tasks().end()};
            const auto* const firstAddress = graph.node(*first);
            const auto* const sinkAddress = graph.node(*sink);

            expectError(graph.connect(destination, encode(*second, PortType::Out, 7)),
                        GraphError::InputOccupied);
            expectEdges(graph, expected);
            EXPECT_EQ(graph.node(*first), firstAddress);
            EXPECT_EQ(graph.node(*sink), sinkAddress);
            ASSERT_EQ(plan->tasks().size(), tasks.size());
            for (std::size_t i = 0; i < tasks.size(); ++i) EXPECT_EQ(plan->tasks()[i], tasks[i]);
            const float input = 12;
            float output = 99;
            const std::array inputs{&input};
            const std::array outputs{&output};
            ASSERT_TRUE(plan->exec(BackendBlock{inputs, outputs, 1}));
            EXPECT_EQ(output, 0); // No test Node writes physical samples.
            EXPECT_EQ(static_cast<OccupancyNode*>(graph.node(*first))->inputCount, 1U);
            EXPECT_EQ(static_cast<OccupancyNode*>(graph.node(*sink))->inputCount, 0U);
            EXPECT_EQ(static_cast<OccupancyNode*>(graph.node(*sink))->backendCount, 1U);
        }
    }

    TEST(InputOccupancy, ExactDuplicatePrecedesOccupancyButAnotherOutputOfTheSameNodeIsOccupied) {
        DAG graph;
        const auto source = graph.add<OccupancyNode>();
        const auto sink = graph.add<OccupancyNode>();
        ASSERT_TRUE(source);
        ASSERT_TRUE(sink);
        const auto out = encode(*source, PortType::Out, 0);
        const auto destination = encode(*sink, PortType::Aux, 1);
        ASSERT_TRUE(graph.connect(destination, out));
        expectError(graph.connect(destination, out), GraphError::ConnectionExists);
        expectError(graph.connect(destination, encode(*source, PortType::Out, 1)),
                    GraphError::InputOccupied);
        const std::array expected{Connection{out, destination}};
        expectEdges(graph, expected);
        ASSERT_TRUE(graph.connect(encode(*sink, PortType::In, 1), out));
        EXPECT_EQ(graph.connections().size(), 2U);
    }

    TEST(InputOccupancy, DisconnectFreesOnlyTheRemovedDestinationAndKeepsOtherOccupancy) {
        DAG graph;
        const auto first = graph.add<OccupancyNode>();
        const auto second = graph.add<OccupancyNode>();
        const auto sink = graph.add<OccupancyNode>();
        const auto other = graph.add<OccupancyNode>();
        ASSERT_TRUE(first);
        ASSERT_TRUE(second);
        ASSERT_TRUE(sink);
        ASSERT_TRUE(other);
        const auto out = encode(*first, PortType::Out, 0);
        const auto in = encode(*sink, PortType::In, 0);
        const auto aux = encode(*sink, PortType::Aux, 0);
        const auto otherIn = encode(*other, PortType::In, 0);
        ASSERT_TRUE(graph.connect(in, out));
        ASSERT_TRUE(graph.connect(aux, out));
        ASSERT_TRUE(graph.connect(otherIn, out));
        // S02-05 removes the exact In edge while preserving this Node pair's Aux.
        const auto disconnected = graph.disconnect(in, out);
        ASSERT_TRUE(disconnected.has_value());
        EXPECT_TRUE(*disconnected);
        const std::array remaining{Connection{out, aux}, Connection{out, otherIn}};
        expectEdges(graph, remaining);
        const auto replacement = encode(*second, PortType::Out, 7);
        ASSERT_TRUE(graph.connect(in, replacement));
        expectError(graph.connect(aux, replacement), GraphError::InputOccupied);
        expectError(graph.connect(otherIn, replacement), GraphError::InputOccupied);
        EXPECT_EQ(graph.connections().size(), 3U);
    }

    TEST(InputOccupancy, RemovingProducersAndConsumersReleasesOnlyTheirIncidentDestinations) {
        DAG graph;
        const auto first = graph.add<OccupancyNode>();
        const auto second = graph.add<OccupancyNode>();
        const auto sink = graph.add<OccupancyNode>();
        const auto other = graph.add<OccupancyNode>();
        ASSERT_TRUE(first);
        ASSERT_TRUE(second);
        ASSERT_TRUE(sink);
        ASSERT_TRUE(other);
        const auto firstOut = encode(*first, PortType::Out, 0);
        const auto secondOut = encode(*second, PortType::Out, 0);
        const auto in = encode(*sink, PortType::In, 0);
        const auto aux = encode(*sink, PortType::Aux, 0);
        const auto otherIn = encode(*other, PortType::In, 0);
        ASSERT_TRUE(graph.connect(in, firstOut));
        ASSERT_TRUE(graph.connect(aux, secondOut));
        ASSERT_TRUE(graph.connect(otherIn, secondOut));
        ASSERT_TRUE(graph.remove(*first));
        const std::array afterProducerRemoval{Connection{secondOut, aux}, Connection{secondOut, otherIn}};
        expectEdges(graph, afterProducerRemoval);
        ASSERT_TRUE(graph.connect(in, secondOut));
        ASSERT_TRUE(graph.remove(*sink));
        const std::array afterConsumerRemoval{Connection{secondOut, otherIn}};
        expectEdges(graph, afterConsumerRemoval);
        const auto replacement = graph.add<OccupancyNode>();
        ASSERT_TRUE(replacement);
        EXPECT_GT(*replacement, *other);
        ASSERT_TRUE(graph.connect(encode(*replacement, PortType::In, 0), secondOut));
        expectError(graph.connect(otherIn, encode(*replacement, PortType::Out, 0)),
                    GraphError::InputOccupied);
        EXPECT_EQ(graph.connections().size(), 2U);
    }
}
