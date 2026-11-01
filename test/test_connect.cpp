#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

import mka.audio.dsp.dag;

namespace {
    using namespace mka::audio::dsp;

    class ConnectNode final : public Node {
    public:
        explicit ConnectNode(const NodeID id)
            : Node(id, Config{.layoutInput = {0}, .layoutAuxiliary = {0}, .layoutOutput = {0}}) {}

        void process(const ProcessContext& context) noexcept override {
            ++calls;
            backendCount = context.backendOutputs.size();
            for (FrameCount frame = 0; frame < context.frames; ++frame) {
                const float input = context.inputs.empty() ? 0.F : context.inputs[0].buffer[frame];
                const float auxiliary = context.auxiliary.empty() ? 0.F : context.auxiliary[0].buffer[frame];
                const float result = input + auxiliary + static_cast<float>(id());
                if (!context.outputs.empty()) context.outputs[0].buffer[frame] = result;
                if (!context.backendOutputs.empty()) context.backendOutputs[0].buffer[frame] = result;
            }
        }

        unsigned calls = 0;
        std::size_t backendCount = 0; // Numeric observations only, no retained callback pointers.
    };

    static_assert(std::same_as<decltype(std::declval<DAG&>().connect(ChannelID{}, ChannelID{})),
                               std::expected<void, GraphError>>);
    constexpr std::array semanticErrors{GraphError::NodeNotFound, GraphError::InvalidPort,
        GraphError::InvalidDirection, GraphError::ConnectionExists, GraphError::InputOccupied,
        GraphError::CycleDetected};
    static_assert([] {
        for (std::size_t a = 0; a < semanticErrors.size(); ++a)
            for (std::size_t b = a + 1; b < semanticErrors.size(); ++b)
                if (semanticErrors[a] == semanticErrors[b]) return false;
        return true;
    }());

    constexpr ChannelID raw(const NodeID id, const std::uint16_t port, const ChannelIndex index = 0) {
        return (ChannelID{id} << 32U) | (ChannelID{port} << 16U) | ChannelID{index};
    }

    void expectError(const Result& result, const GraphError error) {
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error(), error);
    }

    void expectEdges(const DAG& graph, const std::span<const Connection> expected) {
        ASSERT_EQ(graph.connections().size(), expected.size());
        for (std::size_t i = 0; i < expected.size(); ++i)
            EXPECT_EQ(graph.connections()[i], expected[i]);
    }

    TEST(ConnectContract, DestinationComesFirstAndInvertedArgumentsDoNotCreateAnEdge) {
        DAG graph;
        const auto source = graph.add<ConnectNode>();
        const auto sink = graph.add<ConnectNode>();
        ASSERT_TRUE(source);
        ASSERT_TRUE(sink);
        const auto out = encode(*source, PortType::Out, 65535);
        const auto in = encode(*sink, PortType::In, 65535);
        expectError(graph.connect(out, in), GraphError::InvalidDirection);
        EXPECT_TRUE(graph.connections().empty());
        ASSERT_TRUE(graph.connect(in, out));
        const std::array expected{Connection{out, in}};
        expectEdges(graph, expected);
        expectError(graph.connect(out, in), GraphError::InvalidDirection);
        expectEdges(graph, expected);
    }

    TEST(ConnectTransaction, EverySemanticRefusalPreservesDesiredEdgesAddressesAndTheOldAudioPlan) {
        DAG graph;
        const auto a = graph.add<ConnectNode>();
        const auto b = graph.add<ConnectNode>();
        const auto c = graph.add<ConnectNode>();
        ASSERT_TRUE(a);
        ASSERT_TRUE(b);
        ASSERT_TRUE(c);
        const std::array addresses{graph.node(*a), graph.node(*b), graph.node(*c)};
        ASSERT_TRUE(graph.connect(encode(*b, PortType::In, 0), encode(*a, PortType::Out, 0)));
        ASSERT_TRUE(graph.connect(encode(*c, PortType::Aux, 0), encode(*b, PortType::Out, 0)));
        ASSERT_TRUE(graph.markAsOutput(*c));
        const std::vector<Connection> before{graph.connections().begin(), graph.connections().end()};
        const BackendConfig config{.maxFrames = 1, .inputChannels = 1, .outputChannels = 1};
        auto oldPlan = graph.compile(config);
        ASSERT_TRUE(oldPlan);
        const std::vector<Node*> oldTasks{oldPlan->tasks().begin(), oldPlan->tasks().end()};
        struct RejectedEdge { ChannelID destination; ChannelID source; GraphError error; };
        const std::array cases{
            RejectedEdge{encode(*c, PortType::In, 42), encode(maxNodeID, PortType::Out, 0), GraphError::NodeNotFound},
            RejectedEdge{encode(*c, PortType::In, 42), raw(*a, 0x1001), GraphError::InvalidPort},
            RejectedEdge{encode(*c, PortType::In, 42), encode(*a, PortType::In, 0), GraphError::InvalidDirection},
            RejectedEdge{encode(*b, PortType::In, 0), encode(*a, PortType::Out, 0), GraphError::ConnectionExists},
            RejectedEdge{encode(*b, PortType::In, 0), encode(*c, PortType::Out, 0), GraphError::InputOccupied},
            RejectedEdge{encode(*a, PortType::Aux, 0), encode(*c, PortType::Out, 0), GraphError::CycleDetected}};
        const float input = 2;
        float output = 99;
        const std::array inputs{&input};
        const std::array outputs{&output};
        for (const auto& edge : cases) {
            SCOPED_TRACE(static_cast<int>(edge.error));
            expectError(graph.connect(edge.destination, edge.source), edge.error);
            expectEdges(graph, before);
            EXPECT_EQ(graph.node(*a), addresses[0]);
            EXPECT_EQ(graph.node(*b), addresses[1]);
            EXPECT_EQ(graph.node(*c), addresses[2]);
            ASSERT_EQ(oldPlan->tasks().size(), oldTasks.size());
            for (std::size_t i = 0; i < oldTasks.size(); ++i)
                EXPECT_EQ(oldPlan->tasks()[i], oldTasks[i]);
            ASSERT_TRUE(oldPlan->exec(BackendBlock{inputs, outputs, 1}));
            EXPECT_EQ(output, 8); // A: 2+1, B: 3+2, C: aux 5+3.
            auto sameDesiredGraph = graph.compile(config);
            ASSERT_TRUE(sameDesiredGraph);
            ASSERT_TRUE(sameDesiredGraph->exec(BackendBlock{inputs, outputs, 1}));
            EXPECT_EQ(output, 8);
            EXPECT_EQ(static_cast<ConnectNode*>(addresses[0])->backendCount, 0U);
            EXPECT_EQ(static_cast<ConnectNode*>(addresses[1])->backendCount, 0U);
            EXPECT_EQ(static_cast<ConnectNode*>(addresses[2])->backendCount, 1U);
        }
    }

    TEST(ConnectCycles, AuxAndUndeclaredChannelsParticipateInIndirectCyclesAndSelfLoops) {
        DAG graph;
        const auto a = graph.add<ConnectNode>();
        const auto b = graph.add<ConnectNode>();
        const auto c = graph.add<ConnectNode>();
        ASSERT_TRUE(a);
        ASSERT_TRUE(b);
        ASSERT_TRUE(c);
        ASSERT_TRUE(graph.connect(encode(*b, PortType::Aux, 65535), encode(*a, PortType::Out, 42)));
        ASSERT_TRUE(graph.connect(encode(*c, PortType::In, 7), encode(*b, PortType::Out, 19)));
        const std::vector<Connection> before{graph.connections().begin(), graph.connections().end()};
        expectError(graph.connect(encode(*a, PortType::In, 65535), encode(*c, PortType::Out, 11)),
                    GraphError::CycleDetected);
        expectError(graph.connect(encode(*c, PortType::Aux, 99), encode(*c, PortType::Out, 0)),
                    GraphError::CycleDetected);
        expectEdges(graph, before);
        // A forward shortcut is legal, and failed cycles did not reserve its destination.
        ASSERT_TRUE(graph.connect(encode(*c, PortType::In, 8), encode(*a, PortType::Out, 42)));
        EXPECT_EQ(graph.connections().size(), 3U);
        ASSERT_TRUE(graph.compile());
    }

    TEST(ConnectErrors, PortThenDirectionThenExistenceThenDuplicateAndOccupancyHaveStablePriority) {
        DAG graph;
        const auto a = graph.add<ConnectNode>();
        const auto b = graph.add<ConnectNode>();
        ASSERT_TRUE(a);
        ASSERT_TRUE(b);
        expectError(graph.connect(encode(maxNodeID, PortType::Out, 0), raw(invalidNodeID, 0xFFFF)),
                    GraphError::InvalidPort);
        expectError(graph.connect(encode(maxNodeID, PortType::Out, 0), encode(invalidNodeID, PortType::In, 0)),
                    GraphError::InvalidDirection);
        expectError(graph.connect(encode(maxNodeID, PortType::In, 0), encode(*a, PortType::Out, 0)),
                    GraphError::NodeNotFound);
        ASSERT_TRUE(graph.connect(encode(*b, PortType::In, 0), encode(*a, PortType::Out, 0)));
        // Duplicate also occupies its destination; that exact-pair error is more precise.
        expectError(graph.connect(encode(*b, PortType::In, 0), encode(*a, PortType::Out, 0)),
                    GraphError::ConnectionExists);
        // This candidate both occupies B's input and introduces a self-loop.
        expectError(graph.connect(encode(*b, PortType::In, 0), encode(*b, PortType::Out, 1)),
                    GraphError::InputOccupied);
        const std::array expected{Connection{encode(*a, PortType::Out, 0), encode(*b, PortType::In, 0)}};
        expectEdges(graph, expected);
    }

    TEST(ConnectDecoding, UndefinedPortBitsAreNeverNormalizedToValidLowPortBits) {
        DAG graph;
        const auto source = graph.add<ConnectNode>();
        const auto sink = graph.add<ConnectNode>();
        ASSERT_TRUE(source);
        ASSERT_TRUE(sink);
        const auto out = encode(*source, PortType::Out, 65535);
        const auto in = encode(*sink, PortType::In, 65535);
        constexpr std::array<std::uint16_t, 6> malformed{3, 0x0100, 0x0101, 0x0102, 0x8001, 0xFFFF};
        for (const auto port : malformed) {
            SCOPED_TRACE(port);
            EXPECT_EQ(static_cast<std::uint16_t>(decodePort(raw(*source, port, 65535))), port);
            expectError(graph.connect(in, raw(*source, port, 65535)), GraphError::InvalidPort);
            expectError(graph.connect(raw(*sink, port, 65535), out), GraphError::InvalidPort);
            EXPECT_TRUE(graph.connections().empty());
        }
        expectError(graph.connect(in, encode(*source | NodeID{0x80000000U}, PortType::Out, 65535)),
                    GraphError::NodeNotFound);
        EXPECT_TRUE(graph.connections().empty());
        ASSERT_TRUE(graph.connect(in, out));
    }

    TEST(ConnectLegacyAdapter, UsesDestinationFirstAndSeparatesInvalidPortFromInvalidDirection) {
        DAG graph;
        const auto source = graph.add<ConnectNode>();
        const auto sink = graph.add<ConnectNode>();
        ASSERT_TRUE(source);
        ASSERT_TRUE(sink);
        const Channel out{*source, 65535, PortType::Out};
        const Channel aux{*sink, 65535, PortType::Aux};
        expectError(graph.connect(out, aux), GraphError::InvalidDirection);
        expectError(graph.connect(aux, Channel{*source, 0, static_cast<PortType>(0x8001)}),
                    GraphError::InvalidPort);
        EXPECT_TRUE(graph.connections().empty());
        ASSERT_TRUE(graph.connect(aux, out));
        const std::array expected{Connection{encode(*source, PortType::Out, 65535),
                                             encode(*sink, PortType::Aux, 65535)}};
        expectEdges(graph, expected);
    }
}
