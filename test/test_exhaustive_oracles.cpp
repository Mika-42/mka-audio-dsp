#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "four_node_oracle.hpp"

import mka.audio.dsp.dag;

namespace {
    using namespace mka::audio::dsp;
    namespace reference = four_node_oracle;

    class ExhaustiveNode final : public Node {
    public:
        explicit ExhaustiveNode(NodeID id) : Node(id, Config{}) {}
        void process(const ProcessContext&) noexcept override { ++processCalls; }
        unsigned processCalls = 0;
    };

    /** Mapping a reference decision is observation, never a production prediction. */
    [[nodiscard]] auto expectedError(reference::Decision decision) {
        switch (decision) {
            case reference::Decision::InvalidPort: return GraphError::InvalidPort;
            case reference::Decision::InvalidDirection: return GraphError::InvalidDirection;
            case reference::Decision::NodeNotFound: return GraphError::NodeNotFound;
            case reference::Decision::ConnectionExists: return GraphError::ConnectionExists;
            case reference::Decision::InputOccupied: return GraphError::InputOccupied;
            case reference::Decision::CycleDetected: return GraphError::CycleDetected;
            case reference::Decision::Accepted: break;
        }
        ADD_FAILURE() << "Accepted has no error code";
        return GraphError::InvalidDirection;
    }

    struct AttemptCounts {
        std::size_t total = 0;
        std::array<std::size_t, 7> decisions{};
    };

    /**
     * @brief Owners and detached values local to one four-Node baseline.
     * @details The old plan retires before its owning DAG. No test calls process,
     * prepares backend samples or retains collection spans across mutations.
     */
    struct Baseline {
        DAG graph;
        std::array<Node*, reference::nodeCount> addresses{};
        std::vector<NodeID> oldOrder;
        std::optional<ExecutionPlan> oldPlan;
    };

    void initialize(Baseline& state, const reference::Graph& model) {
        for (std::size_t i = 0; i < model.nodes.size(); ++i) {
            const auto added = state.graph.add<ExhaustiveNode>();
            ASSERT_TRUE(added);
            ASSERT_EQ(*added, model.nodes[i]);
            state.addresses[i] = state.graph.node(*added);
        }
        for (const auto& edge : model.edges) ASSERT_TRUE(state.graph.connect(edge.to, edge.from));
        ASSERT_TRUE(state.graph.markAsOutput(model.nodes.back()));
        ASSERT_TRUE(state.graph.markAsOutput(model.nodes.front()));
        state.oldOrder = model.lexicographicOrder();
        ASSERT_EQ(state.oldOrder.size(), reference::nodeCount);
        auto compiled = state.graph.compile();
        ASSERT_TRUE(compiled);
        state.oldPlan.emplace(std::move(*compiled));
    }

    /** Compare exact edges and destination occupancy, not just edge counts. */
    void expectState(const Baseline& state, const reference::Graph& expected) {
        ASSERT_EQ(state.graph.connections().size(), expected.edges.size());
        // A simple four-Node DAG has <=6 arcs; the one candidate may add a
        // parallel logical relation, hence at most seven stored edge values.
        ASSERT_LE(expected.edges.size(), 7U);
        std::array<ChannelID, 7> actualOccupancy{}, expectedOccupancy{};
        for (std::size_t i = 0; i < expected.edges.size(); ++i) {
            EXPECT_EQ(state.graph.connections()[i].from, expected.edges[i].from);
            EXPECT_EQ(state.graph.connections()[i].to, expected.edges[i].to);
            actualOccupancy[i] = state.graph.connections()[i].to;
            expectedOccupancy[i] = expected.edges[i].to;
        }
        auto actualEnd = actualOccupancy.begin() + expected.edges.size();
        auto expectedEnd = expectedOccupancy.begin() + expected.edges.size();
        std::sort(actualOccupancy.begin(), actualEnd);
        std::sort(expectedOccupancy.begin(), expectedEnd);
        EXPECT_EQ(actualOccupancy, expectedOccupancy);
        EXPECT_TRUE(std::adjacent_find(actualOccupancy.begin(), actualEnd) == actualEnd);
        ASSERT_EQ(state.graph.outputNodes().size(), 2U);
        EXPECT_EQ(state.graph.outputNodes()[0], expected.nodes.front());
        EXPECT_EQ(state.graph.outputNodes()[1], expected.nodes.back());
        for (std::size_t i = 0; i < expected.nodes.size(); ++i) {
            ASSERT_EQ(state.graph.node(expected.nodes[i]), state.addresses[i]);
            EXPECT_EQ(static_cast<const ExhaustiveNode*>(state.addresses[i])->processCalls, 0U);
        }
        ASSERT_TRUE(state.oldPlan);
        ASSERT_EQ(state.oldPlan->tasks().size(), state.oldOrder.size());
        for (std::size_t i = 0; i < state.oldOrder.size(); ++i)
            EXPECT_EQ(state.oldPlan->tasks()[i]->id(), state.oldOrder[i]);
    }

    void expectOrder(const DAG& graph, const reference::Graph& model) {
        const auto expected = model.lexicographicOrder();
        ASSERT_EQ(expected.size(), reference::nodeCount);
        const auto actual = topologicalOrder(graph);
        ASSERT_TRUE(actual);
        EXPECT_EQ(*actual, expected);
    }

    void verifyCandidate(Baseline& state, const reference::Graph& model,
                         const reference::Edge& candidate, AttemptCounts& counts) {
        SCOPED_TRACE(::testing::Message() << "from=0x" << std::hex << candidate.from
                                        << " to=0x" << candidate.to);
        const auto prediction = model.predict(candidate);
        ++counts.total;
        ++counts.decisions[static_cast<std::size_t>(prediction)];
        const auto actual = state.graph.connect(candidate.to, candidate.from);
        if (prediction == reference::Decision::Accepted) {
            ASSERT_TRUE(actual);
            auto augmented = model;
            augmented.edges.push_back(candidate);
            ASSERT_NO_FATAL_FAILURE(expectState(state, augmented));
            ASSERT_NO_FATAL_FAILURE(expectOrder(state.graph, augmented));
            const auto removed = state.graph.disconnect(candidate.to, candidate.from);
            ASSERT_TRUE(removed);
            ASSERT_TRUE(*removed);
        } else {
            ASSERT_FALSE(actual);
            ASSERT_EQ(actual.error(), expectedError(prediction));
        }
        ASSERT_NO_FATAL_FAILURE(expectState(state, model));
        ASSERT_NO_FATAL_FAILURE(expectOrder(state.graph, model));
    }

    TEST(ExhaustiveDAGOracles, EveryStructuralChannelPartitionAndOrderMatchesIndependentModels) {
        AttemptCounts counts;
        unsigned graphs = 0;
        constexpr std::array<std::uint16_t, 2> sourceIndices{0, 65535};
        constexpr std::array<std::uint16_t, 2> destinationPorts{reference::in, reference::aux};
        constexpr std::array<std::uint16_t, 6> destinationIndices{0, 10, 11, 12, 13, 65535};
        for (unsigned mask = 0; mask < reference::maskCount; ++mask) {
            const auto model = reference::fromMask(static_cast<std::uint16_t>(mask));
            if (!model.acyclic()) continue;
            ++graphs;
            SCOPED_TRACE(::testing::Message() << "seed=0 mask=" << mask);
            Baseline state;
            ASSERT_NO_FATAL_FAILURE(initialize(state, model));
            ASSERT_NO_FATAL_FAILURE(expectState(state, model));
            ASSERT_NO_FATAL_FAILURE(expectOrder(state.graph, model));
            for (const auto source : model.nodes)
                for (const auto destination : model.nodes)
                    for (const auto sourceIndex : sourceIndices)
                        for (const auto destinationPort : destinationPorts)
                            for (const auto destinationIndex : destinationIndices) {
                                const reference::Edge candidate{
                                    reference::channel(source, reference::out, sourceIndex),
                                    reference::channel(destination, destinationPort, destinationIndex)};
                                ASSERT_NO_FATAL_FAILURE(verifyCandidate(state, model, candidate, counts));
                            }
        }
        EXPECT_EQ(graphs, 543U);
        EXPECT_EQ(counts.total, std::size_t{543} * 16 * 2 * 2 * 6);
        for (const auto decision : {reference::Decision::Accepted, reference::Decision::ConnectionExists,
                                    reference::Decision::InputOccupied, reference::Decision::CycleDetected})
            EXPECT_GT(counts.decisions[static_cast<std::size_t>(decision)], 0U);
    }

    TEST(ExhaustiveDAGOracles, AllOfficialDirectionsMalformedPortsAndMissingNodesPreserveRefusedState) {
        AttemptCounts counts;
        unsigned graphs = 0;
        constexpr std::array<std::uint16_t, 3> official{reference::in, reference::out, reference::aux};
        constexpr std::array<std::uint16_t, 4> malformed{3, 256, 0x8001, 65535};
        constexpr std::array<reference::NodeId, 2> missing{
            0, std::numeric_limits<reference::NodeId>::max()};
        for (unsigned mask = 0; mask < reference::maskCount; ++mask) {
            const auto model = reference::fromMask(static_cast<std::uint16_t>(mask));
            if (!model.acyclic()) continue;
            ++graphs;
            SCOPED_TRACE(::testing::Message() << "seed=0 mask=" << mask);
            Baseline state;
            ASSERT_NO_FATAL_FAILURE(initialize(state, model));
            for (const auto source : model.nodes)
                for (const auto destination : model.nodes)
                    for (const auto sourcePort : official)
                        for (const auto destinationPort : official) {
                            const reference::Edge candidate{
                                reference::channel(source, sourcePort, 0),
                                reference::channel(destination, destinationPort, 65535)};
                            ASSERT_NO_FATAL_FAILURE(verifyCandidate(state, model, candidate, counts));
                        }
            for (const auto port : malformed) {
                const reference::Edge sourceMalformed{
                    reference::channel(model.nodes.front(), port, 65535),
                    reference::channel(model.nodes.back(), reference::in, 65535)};
                const reference::Edge destinationMalformed{
                    reference::channel(model.nodes.front(), reference::out, 65535),
                    reference::channel(model.nodes.back(), port, 65535)};
                ASSERT_NO_FATAL_FAILURE(verifyCandidate(state, model, sourceMalformed, counts));
                ASSERT_NO_FATAL_FAILURE(verifyCandidate(state, model, destinationMalformed, counts));
            }
            for (const auto id : missing) {
                const reference::Edge sourceMissing{
                    reference::channel(id, reference::out, 65535),
                    reference::channel(model.nodes.back(), reference::in, 65535)};
                const reference::Edge destinationMissing{
                    reference::channel(model.nodes.front(), reference::out, 65535),
                    reference::channel(id, reference::aux, 65535)};
                ASSERT_NO_FATAL_FAILURE(verifyCandidate(state, model, sourceMissing, counts));
                ASSERT_NO_FATAL_FAILURE(verifyCandidate(state, model, destinationMissing, counts));
            }
            if (!model.edges.empty()) {
                const auto duplicate = model.edges.front();
                const reference::Edge occupied{
                    reference::channel(reference::nodeOf(duplicate.from), reference::out, 65535),
                    duplicate.to};
                ASSERT_NO_FATAL_FAILURE(verifyCandidate(state, model, duplicate, counts));
                ASSERT_NO_FATAL_FAILURE(verifyCandidate(state, model, occupied, counts));
            }
        }
        EXPECT_EQ(graphs, 543U);
        // 144 official combinations + eight malformed + four missing per DAG,
        // with duplicate/occupancy witnesses for every nonempty baseline.
        EXPECT_EQ(counts.total, std::size_t{543} * 156 + std::size_t{542} * 2);
        for (const auto hits : counts.decisions) EXPECT_GT(hits, 0U);
    }
}
