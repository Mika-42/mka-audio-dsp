#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>
#include <utility>

#include <gtest/gtest.h>

import mka.audio.dsp.connection;
import mka.audio.dsp.dag;

namespace {
    using namespace mka::audio::dsp;

    class ConnectionNode final : public Node {
    public:
        explicit ConnectionNode(const NodeID id) : Node(id, Config{}) {}
        void process(const ProcessContext& context) noexcept override {
            inputCount = context.inputs.size();
            ++calls;
        }
        std::size_t inputCount = 0;
        unsigned calls = 0;
    };

    static_assert(std::is_aggregate_v<Connection>);
    static_assert(std::is_standard_layout_v<Connection>);
    static_assert(std::is_trivially_copyable_v<Connection>);
    static_assert(std::is_trivially_destructible_v<Connection>);
    static_assert(!std::is_polymorphic_v<Connection> && !std::is_abstract_v<Connection>);
    static_assert(std::same_as<decltype(Connection::from), ChannelID>);
    static_assert(std::same_as<decltype(Connection::to), ChannelID>);
    static_assert(std::same_as<detail::PlanConnection, Connection>);
    static_assert(noexcept(Connection{}));
    static_assert(noexcept(Connection{} == Connection{}));
    static_assert(Connection{} == Connection{0, 0});
    static_assert(std::same_as<decltype(std::declval<const DAG&>().connections()),
                               std::span<const Connection>>);
    static_assert(noexcept(std::declval<const DAG&>().connections()));
    static_assert(!std::is_assignable_v<
        decltype((std::declval<std::span<const Connection>>()[0].from)), ChannelID>);

    void expectError(const Result& result, const GraphError error) {
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error(), error);
    }

    TEST(ConnectionValue, DefaultsToZeroAndCopiesCanBeEditedIndependently) {
        const Connection empty;
        EXPECT_EQ(empty.from, 0U);
        EXPECT_EQ(empty.to, 0U);
        const Connection original{0x123456780001ABCDULL, 0x876543210002FFFFULL};
        Connection copy = original;
        EXPECT_EQ(copy, original);
        copy.from ^= ChannelID{1} << 63U;
        copy.to ^= ChannelID{1};
        EXPECT_EQ(original.from, 0x123456780001ABCDULL);
        EXPECT_EQ(original.to, 0x876543210002FFFFULL);
        EXPECT_NE(copy, original);
    }

    TEST(ConnectionValue, EqualityUsesBothFullWidthIdentitiesRatherThanNodePairs) {
        constexpr Connection edge{
            encode(maxNodeID, PortType::Out, maxChannelIndex),
            encode(NodeID{0x80000000U}, PortType::Aux, maxChannelIndex)};
        static_assert(edge == Connection{0xFFFFFFFF0001FFFFULL, 0x800000000002FFFFULL});
        EXPECT_EQ(edge, (Connection{edge.from, edge.to}));
        EXPECT_NE(edge, (Connection{edge.from ^ (ChannelID{1} << 63U), edge.to}));
        EXPECT_NE(edge, (Connection{edge.from, edge.to ^ (ChannelID{1} << 32U)}));
        EXPECT_NE(edge, (Connection{edge.from - 1, edge.to}));
        EXPECT_NE(edge, (Connection{edge.from, edge.to - 1}));
        EXPECT_NE(edge, (Connection{edge.from, encode(NodeID{0x80000000U}, PortType::In, maxChannelIndex)}));
        EXPECT_NE(edge, (Connection{edge.to, edge.from}));
        // A standalone value preserves bits; DAG insertion validates structure.
        const Connection arbitrary{std::numeric_limits<ChannelID>::max(), 0};
        EXPECT_EQ(arbitrary.from, std::numeric_limits<ChannelID>::max());
    }

    TEST(ConnectionStorage, SameNodesKeepDistinctChannelsAndPortsInInsertionOrderOutsideLayouts) {
        DAG graph;
        const auto source = graph.add<ConnectionNode>();
        const auto sink = graph.add<ConnectionNode>();
        ASSERT_TRUE(source);
        ASSERT_TRUE(sink);
        ASSERT_TRUE(graph.node(*source)->config().layoutOutput.empty());
        ASSERT_TRUE(graph.node(*sink)->config().layoutInput.empty());
        const std::array expected{
            Connection{encode(*source, PortType::Out, 7), encode(*sink, PortType::In, 42)},
            Connection{encode(*source, PortType::Out, 8), encode(*sink, PortType::In, 43)},
            Connection{encode(*source, PortType::Out, 65535), encode(*sink, PortType::Aux, 42)}};
        for (const auto& edge : expected) ASSERT_TRUE(graph.connect(edge.to, edge.from));
        const DAG& readOnly = graph;
        ASSERT_EQ(readOnly.connections().size(), expected.size());
        for (std::size_t i = 0; i < expected.size(); ++i) {
            EXPECT_EQ(readOnly.connections()[i], expected[i]);
            EXPECT_EQ(decodePort(readOnly.connections()[i].from), PortType::Out);
            EXPECT_TRUE(isInputPort(decodePort(readOnly.connections()[i].to)));
        }
        auto plan = graph.compile(BackendConfig{.maxFrames = 1});
        ASSERT_TRUE(plan); // Repeated Node pairs do not duplicate tasks/dependencies.
        ASSERT_EQ(plan->tasks().size(), 2U);
        EXPECT_EQ(plan->tasks()[0], graph.node(*source));
        EXPECT_EQ(plan->tasks()[1], graph.node(*sink));
    }

    TEST(ConnectionStorage, ExactDuplicateRefusalPreservesTheStoredValueAndAllowsOtherChannels) {
        DAG graph;
        const auto source = graph.add<ConnectionNode>();
        const auto sink = graph.add<ConnectionNode>();
        ASSERT_TRUE(source);
        ASSERT_TRUE(sink);
        const Connection edge{encode(*source, PortType::Out, 0), encode(*sink, PortType::In, 0)};
        ASSERT_TRUE(graph.connect(edge.to, edge.from));
        expectError(graph.connect(edge.to, edge.from), GraphError::ConnectionExists);
        ASSERT_EQ(graph.connections().size(), 1U);
        EXPECT_EQ(graph.connections()[0], edge);
        const Connection other{edge.from, encode(*sink, PortType::In, 1)};
        ASSERT_TRUE(graph.connect(other.to, other.from));
        ASSERT_EQ(graph.connections().size(), 2U);
        EXPECT_EQ(graph.connections()[0], edge);
        EXPECT_EQ(graph.connections()[1], other);
        Connection detachedCopy = graph.connections()[0];
        detachedCopy.to = other.to;
        EXPECT_EQ(graph.connections()[0], edge);
    }

    TEST(ConnectionStorage, FanOutSharesTheProducerButEveryDestinationHasOneOwner) {
        DAG graph;
        const auto source = graph.add<ConnectionNode>();
        const auto first = graph.add<ConnectionNode>();
        const auto second = graph.add<ConnectionNode>();
        ASSERT_TRUE(source);
        ASSERT_TRUE(first);
        ASSERT_TRUE(second);
        const auto out = encode(*source, PortType::Out, 65535);
        const auto in = encode(*first, PortType::In, 65535);
        const auto aux = encode(*second, PortType::Aux, 65535);
        ASSERT_TRUE(graph.connect(in, out));
        ASSERT_TRUE(graph.connect(aux, out));
        expectError(graph.connect(in, encode(*second, PortType::Out, 1)), GraphError::InputOccupied);
        ASSERT_EQ(graph.connections().size(), 2U);
        EXPECT_EQ(graph.connections()[0], (Connection{out, in}));
        EXPECT_EQ(graph.connections()[1], (Connection{out, aux}));
    }

    TEST(ConnectionStorage, OutputMarkersDoNotCreateHardwareEdgesAndZeroCannotNameABackendNode) {
        DAG graph;
        const auto source = graph.add<ConnectionNode>();
        const auto sink = graph.add<ConnectionNode>();
        ASSERT_TRUE(source);
        ASSERT_TRUE(sink);
        ASSERT_TRUE(graph.markAsOutput(*source));
        ASSERT_TRUE(graph.markAsOutput(*sink));
        EXPECT_TRUE(graph.connections().empty());
        expectError(graph.connect(encode(*sink, PortType::In, 0),
                                  encode(invalidNodeID, PortType::Out, 0)), GraphError::NodeNotFound);
        expectError(graph.connect(ChannelID{0}, encode(*source, PortType::Out, 0)), GraphError::NodeNotFound);
        EXPECT_TRUE(graph.connections().empty());
    }

    TEST(ConnectionStorage, NodeStorageGrowthAndUnrelatedRemovalDoNotChangeConnectionValues) {
        DAG graph;
        const auto source = graph.add<ConnectionNode>();
        const auto sink = graph.add<ConnectionNode>();
        ASSERT_TRUE(source);
        ASSERT_TRUE(sink);
        Node* const sourceAddress = graph.node(*source);
        Node* const sinkAddress = graph.node(*sink);
        const Connection edge{encode(*source, PortType::Out, 11), encode(*sink, PortType::Aux, 19)};
        ASSERT_TRUE(graph.connect(edge.to, edge.from));
        NodeID unrelated = invalidNodeID;
        for (unsigned i = 0; i < 128; ++i) {
            const auto added = graph.add<ConnectionNode>();
            ASSERT_TRUE(added);
            unrelated = *added;
            ASSERT_EQ(graph.connections().size(), 1U);
            EXPECT_EQ(graph.connections()[0], edge);
        }
        ASSERT_TRUE(graph.remove(unrelated));
        EXPECT_EQ(graph.node(*source), sourceAddress);
        EXPECT_EQ(graph.node(*sink), sinkAddress);
        ASSERT_EQ(graph.connections().size(), 1U);
        EXPECT_EQ(graph.connections()[0], edge);
    }

    TEST(ConnectionStorage, GrowingTheEdgeContainerPreservesEveryPairAndDetachedCopies) {
        DAG graph;
        const auto source = graph.add<ConnectionNode>();
        const auto sink = graph.add<ConnectionNode>();
        ASSERT_TRUE(source);
        ASSERT_TRUE(sink);
        Node* const sourceAddress = graph.node(*source);
        Node* const sinkAddress = graph.node(*sink);
        std::array<Connection, 128> expected;
        Connection detached;
        for (std::size_t i = 0; i < expected.size(); ++i) {
            expected[i] = {encode(*source, PortType::Out, 65535),
                           encode(*sink, PortType::In, static_cast<ChannelIndex>(i))};
            ASSERT_TRUE(graph.connect(expected[i].to, expected[i].from));
            if (i == 0) detached = graph.connections()[0]; // Copy, never a stored view.
        }
        ASSERT_EQ(graph.connections().size(), expected.size());
        for (std::size_t i = 0; i < expected.size(); ++i)
            EXPECT_EQ(graph.connections()[i], expected[i]);
        EXPECT_EQ(graph.node(*source), sourceAddress);
        EXPECT_EQ(graph.node(*sink), sinkAddress);
        EXPECT_EQ(detached, expected[0]);
        detached.to = expected.back().to;
        EXPECT_EQ(graph.connections()[0], expected[0]);
        EXPECT_NE(detached, graph.connections()[0]);
    }

    TEST(ConnectionStorage, LegacyWideFieldsAreRejectedBeforeNarrowingOrCreatingAliasedEdges) {
        DAG graph;
        const auto source = graph.add<ConnectionNode>();
        const auto sink = graph.add<ConnectionNode>();
        ASSERT_TRUE(source);
        ASSERT_TRUE(sink);
        expectError(graph.connect(Channel{*sink, 0, PortType::In},
                                  Channel{*source, 65536, PortType::Out}), GraphError::InvalidChannelIndex);
        expectError(graph.connect(Channel{*sink, 65536, PortType::Aux},
                                  Channel{*source, 0, PortType::Out}), GraphError::InvalidChannelIndex);
        const auto wideSource = (std::uint64_t{1} << 32U) + *source;
        const auto wideSink = (std::uint64_t{1} << 32U) + *sink;
        expectError(graph.connect(Channel{*sink, 0, PortType::In},
                                  Channel{wideSource, 0, PortType::Out}), GraphError::NodeNotFound);
        expectError(graph.connect(Channel{wideSink, 0, PortType::In},
                                  Channel{*source, 0, PortType::Out}), GraphError::NodeNotFound);
        EXPECT_TRUE(graph.connections().empty());
        ASSERT_TRUE(graph.connect(Channel{*sink, 65535, PortType::In},
                                  Channel{*source, 65535, PortType::Out}));
        ASSERT_EQ(graph.connections().size(), 1U);
        EXPECT_EQ(graph.connections()[0], (Connection{encode(*source, PortType::Out, 65535),
                                                     encode(*sink, PortType::In, 65535)}));
    }

    TEST(ConnectionStorage, DetachedValueRemainsUsableAfterItsGraphAndNodesAreDestroyed) {
        Connection detached;
        {
            DAG graph;
            const auto source = graph.add<ConnectionNode>();
            const auto sink = graph.add<ConnectionNode>();
            ASSERT_TRUE(source);
            ASSERT_TRUE(sink);
            ASSERT_TRUE(graph.connect(encode(*sink, PortType::Aux, 23), encode(*source, PortType::Out, 17)));
            ASSERT_EQ(graph.connections().size(), 1U);
            detached = graph.connections()[0];
        }
        EXPECT_EQ(detached.from, 0x0000000100010011ULL);
        EXPECT_EQ(detached.to, 0x0000000200020017ULL);
    }

    TEST(ConnectionStorage, EditingDesiredEdgesPreservesThePreviouslyPreparedPlanSnapshot) {
        DAG graph;
        const auto source = graph.add<ConnectionNode>();
        const auto sink = graph.add<ConnectionNode>();
        ASSERT_TRUE(source);
        ASSERT_TRUE(sink);
        const BackendConfig config{.maxFrames = 1, .inputChannels = 1};
        auto original = graph.compile(config);
        ASSERT_TRUE(original);
        const Connection edge{encode(*source, PortType::Out, 42), encode(*sink, PortType::In, 7)};
        ASSERT_TRUE(graph.connect(edge.to, edge.from));
        auto replacement = graph.compile(config);
        ASSERT_TRUE(replacement);
        const float sample = 9;
        const std::array inputs{&sample};
        auto* const consumer = static_cast<ConnectionNode*>(graph.node(*sink));
        ASSERT_TRUE(original->exec(BackendBlock{.inputs = inputs, .frames = 1}));
        EXPECT_EQ(consumer->inputCount, 1U); // Original graph had two sources.
        ASSERT_TRUE(replacement->exec(BackendBlock{.inputs = inputs, .frames = 1}));
        EXPECT_EQ(consumer->inputCount, 0U); // Incoming edge makes it a non-source.
        ASSERT_TRUE(original->exec(BackendBlock{.inputs = inputs, .frames = 1}));
        EXPECT_EQ(consumer->inputCount, 1U);
        EXPECT_EQ(consumer->calls, 3U);
    }
}
