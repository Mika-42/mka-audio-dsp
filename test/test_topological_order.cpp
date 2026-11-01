#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

import mka.audio.dsp.dag;
import mka.audio.dsp.topology;

namespace {
    using namespace mka::audio::dsp;

    struct OrderTrace {
        std::array<NodeID, 3> calls{};
        std::size_t count = 0;
    };

    class OrderNode final : public Node {
    public:
        explicit OrderNode(NodeID id, OrderTrace* trace = nullptr)
            : Node(id, Config{}), trace_(trace) {}

        void process(const ProcessContext& context) noexcept override {
            ++calls;
            if (trace_ != nullptr && trace_->count < trace_->calls.size())
                trace_->calls[trace_->count++] = id();
            for (const auto& output : context.backendOutputs)
                for (FrameCount frame = 0; frame < context.frames; ++frame)
                    output.buffer[frame] = static_cast<float>(id());
        }

        unsigned calls = 0;

    private:
        OrderTrace* trace_; // Test-owned events, never a backend/sample pointer.
    };

    static_assert(std::same_as<decltype(topologicalOrder(std::declval<const DAG&>())),
                               std::expected<std::vector<NodeID>, GraphError>>);
    static_assert(std::same_as<decltype(detail::topologicalOrder(
                                   std::declval<const detail::GraphSnapshot&>())),
                               std::expected<std::vector<NodeID>, GraphError>>);

    void expectOrder(const std::expected<std::vector<NodeID>, GraphError>& result,
                     const std::vector<NodeID>& expected) {
        ASSERT_TRUE(result);
        EXPECT_EQ(*result, expected);
    }

    void expectError(const std::expected<std::vector<NodeID>, GraphError>& result,
                     GraphError error) {
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, error.code);
    }

    TEST(StableTopologicalOrder, EmptyAndIsolatedNodesOccurExactlyOnceIncludingIdentityGaps) {
        DAG graph;
        ASSERT_NO_FATAL_FAILURE(expectOrder(topologicalOrder(graph), {}));
        auto empty = graph.compile();
        ASSERT_TRUE(empty);
        EXPECT_TRUE(empty->tasks().empty());
        EXPECT_EQ(empty->layerCount(), 0U);

        const auto first = graph.add<OrderNode>();
        const auto removed = graph.add<OrderNode>();
        const auto third = graph.add<OrderNode>();
        ASSERT_TRUE(first); ASSERT_TRUE(removed); ASSERT_TRUE(third);
        ASSERT_TRUE(graph.remove(*removed));
        const auto fourth = graph.add<OrderNode>();
        ASSERT_TRUE(fourth);
        const std::vector<NodeID> expected{*first, *third, *fourth};
        ASSERT_NO_FATAL_FAILURE(expectOrder(topologicalOrder(graph), expected));
        ASSERT_NO_FATAL_FAILURE(expectOrder(topologicalOrder(graph), expected));
        auto plan = graph.compile();
        ASSERT_TRUE(plan);
        ASSERT_EQ(plan->tasks().size(), expected.size());
        EXPECT_EQ(plan->layerCount(), expected.size());
        for (std::size_t i = 0; i < expected.size(); ++i) {
            EXPECT_EQ(plan->tasks()[i], graph.node(expected[i]));
            EXPECT_EQ(static_cast<OrderNode*>(graph.node(expected[i]))->calls, 0U);
        }
    }

    TEST(StableTopologicalOrder, NewlyAvailableSmallerIdentityPrecedesAnOlderReadyNodeAndDecidesLastWriter) {
        OrderTrace trace; // Outlives the DAG and its test processors.
        DAG graph;
        const auto sink = graph.add<OrderNode>(&trace);
        const auto source = graph.add<OrderNode>(&trace);
        const auto isolated = graph.add<OrderNode>(&trace);
        ASSERT_TRUE(sink); ASSERT_TRUE(source); ASSERT_TRUE(isolated);
        ASSERT_TRUE(graph.connect(encode(*sink, PortType::In, 65535),
                                  encode(*source, PortType::Out, 42)));
        ASSERT_TRUE(graph.markAsOutput(*isolated));
        ASSERT_TRUE(graph.markAsOutput(*sink));
        ASSERT_TRUE(graph.markAsOutput(*source));

        // Initially source(2) and isolated(3) are ready. Processing 2 makes 1
        // ready: global priority must choose 1 before 3, unlike breadth layers.
        const std::vector<NodeID> expected{*source, *sink, *isolated};
        ASSERT_NO_FATAL_FAILURE(expectOrder(topologicalOrder(graph), expected));
        auto plan = graph.compile(BackendConfig{.maxFrames = 1, .outputChannels = 1});
        ASSERT_TRUE(plan);
        ASSERT_EQ(plan->tasks().size(), expected.size());
        ASSERT_EQ(plan->layerCount(), expected.size());
        for (std::size_t i = 0; i < expected.size(); ++i) {
            EXPECT_EQ(plan->tasks()[i]->id(), expected[i]);
            ASSERT_EQ(plan->layer(i).size(), 1U);
            EXPECT_EQ(plan->layer(i).front(), plan->tasks()[i]);
            EXPECT_EQ(static_cast<OrderNode*>(graph.node(expected[i]))->calls, 0U);
        }
        EXPECT_EQ(trace.count, 0U); // Neither ordering nor compile executes DSP.
        float output = 99;
        const std::array outputs{&output};
        ASSERT_TRUE(plan->exec(BackendBlock{.outputs = outputs, .frames = 1}));
        EXPECT_EQ(trace.count, 3U);
        EXPECT_EQ(trace.calls, (std::array<NodeID, 3>{*source, *sink, *isolated}));
        EXPECT_EQ(output, static_cast<float>(*isolated));
        for (const auto id : expected)
            EXPECT_EQ(static_cast<OrderNode*>(graph.node(id))->calls, 1U);
    }

    TEST(StableTopologicalOrder, DiamondFanOutAndParallelInAuxEdgesIgnoreConnectionInsertionOrder) {
        DAG forward, reverse;
        std::array<NodeID, 5> ids{};
        for (auto& id : ids) {
            const auto a = forward.add<OrderNode>();
            const auto b = reverse.add<OrderNode>();
            ASSERT_TRUE(a); ASSERT_TRUE(b);
            ASSERT_EQ(*a, *b);
            id = *a;
        }
        const std::array edges{
            Connection{encode(ids[0], PortType::Out, 0), encode(ids[1], PortType::In, 0)},
            Connection{encode(ids[0], PortType::Out, 0), encode(ids[2], PortType::Aux, 65535)},
            Connection{encode(ids[1], PortType::Out, 7), encode(ids[3], PortType::In, 65535)},
            Connection{encode(ids[2], PortType::Out, 9), encode(ids[3], PortType::Aux, 0)},
            Connection{encode(ids[0], PortType::Out, 1), encode(ids[1], PortType::Aux, 1)}};
        for (const auto& edge : edges) ASSERT_TRUE(forward.connect(edge.to, edge.from));
        for (auto edge = edges.rbegin(); edge != edges.rend(); ++edge)
            ASSERT_TRUE(reverse.connect(edge->to, edge->from));
        ASSERT_NE(forward.connections().front(), reverse.connections().front());
        const std::vector<NodeID> expected(ids.begin(), ids.end());
        ASSERT_NO_FATAL_FAILURE(expectOrder(topologicalOrder(forward), expected));
        ASSERT_NO_FATAL_FAILURE(expectOrder(topologicalOrder(reverse), expected));
        auto first = forward.compile();
        auto second = reverse.compile();
        ASSERT_TRUE(first); ASSERT_TRUE(second);
        ASSERT_EQ(first->tasks().size(), expected.size());
        ASSERT_EQ(second->tasks().size(), expected.size());
        for (std::size_t i = 0; i < expected.size(); ++i) {
            EXPECT_EQ(first->tasks()[i]->id(), expected[i]);
            EXPECT_EQ(second->tasks()[i]->id(), expected[i]);
        }
    }

    TEST(StableTopologicalOrder, NonContiguousSnapshotIdentitiesAreIndependentOfNodeContainerOrder) {
        detail::GraphSnapshot snapshot;
        snapshot.nodes = {3, 9, 20, 50};
        snapshot.connections = {
            {encode(9, PortType::Out, 65535), encode(3, PortType::Aux, 7)},
            {encode(20, PortType::Out, 2), encode(50, PortType::In, 0)}};
        snapshot.outputs = {50};
        const std::vector<NodeID> expected{9, 3, 20, 50};
        do {
            ASSERT_NO_FATAL_FAILURE(expectOrder(detail::topologicalOrder(snapshot), expected));
            std::ranges::reverse(snapshot.connections);
            ASSERT_NO_FATAL_FAILURE(expectOrder(detail::topologicalOrder(snapshot), expected));
        } while (std::next_permutation(snapshot.nodes.begin(), snapshot.nodes.end()));
    }

    TEST(StableTopologicalOrder, CyclicSnapshotFixturesFailWithoutMutatingTheirOwnedValues) {
        for (const NodeID count : {1U, 2U, 4U}) {
            SCOPED_TRACE(count);
            detail::GraphSnapshot snapshot;
            for (NodeID id = 1; id <= count; ++id) snapshot.nodes.push_back(id);
            for (NodeID id = 1; id < count; ++id)
                snapshot.connections.push_back({encode(id, PortType::Out, 42),
                                                encode(id + 1, PortType::Aux, 65535)});
            snapshot.connections.push_back({encode(count, PortType::Out, 7),
                                            encode(1, PortType::In, 65535)});
            snapshot.outputs = {count};
            const auto before = snapshot;
            ASSERT_NO_FATAL_FAILURE(expectError(detail::topologicalOrder(snapshot), GraphError::CycleDetected));
            EXPECT_EQ(snapshot.nodes, before.nodes);
            EXPECT_EQ(snapshot.connections, before.connections);
            EXPECT_EQ(snapshot.outputs, before.outputs);
        }
    }

    TEST(StableTopologicalOrder, InvalidSnapshotFieldsReturnStructuredErrorsRatherThanPartialOrders) {
        detail::GraphSnapshot snapshot;
        snapshot.nodes = {1, 2};
        snapshot.connections = {{encode(3, PortType::Out, 0), encode(2, PortType::In, 0)}};
        ASSERT_NO_FATAL_FAILURE(expectError(detail::topologicalOrder(snapshot), GraphError::NodeNotFound));
        const ChannelID malformed = (ChannelID{1} << 32U) | (ChannelID{0x8001} << 16U);
        snapshot.connections = {{malformed, encode(2, PortType::In, 0)}};
        ASSERT_NO_FATAL_FAILURE(expectError(detail::topologicalOrder(snapshot), GraphError::InvalidPort));
        snapshot.connections = {{encode(1, PortType::In, 0), encode(2, PortType::In, 0)}};
        ASSERT_NO_FATAL_FAILURE(expectError(detail::topologicalOrder(snapshot), GraphError::InvalidDirection));
        snapshot.connections.clear();
        snapshot.nodes = {1, 1};
        ASSERT_NO_FATAL_FAILURE(expectError(detail::topologicalOrder(snapshot), GraphError::InvalidNodeIdentity));
        snapshot.nodes = {0, 2};
        ASSERT_NO_FATAL_FAILURE(expectError(detail::topologicalOrder(snapshot), GraphError::InvalidNodeIdentity));
    }
}
