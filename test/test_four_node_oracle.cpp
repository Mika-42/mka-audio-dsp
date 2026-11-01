#include <array>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <type_traits>

#include <gtest/gtest.h>

#include "four_node_oracle.hpp"

import mka.audio.dsp.dag;

namespace {
    using namespace mka::audio::dsp;
    namespace reference = four_node_oracle;

    static_assert(std::is_same_v<reference::NodeId, NodeID>);
    static_assert(std::is_same_v<reference::ChannelId, ChannelID>);
    static_assert(std::is_same_v<std::underlying_type_t<reference::Decision>, std::uint16_t>);

    class OracleNode final : public Node {
    public:
        explicit OracleNode(NodeID id) : Node(id, Config{}) {}
        void process(const ProcessContext&) noexcept override { ++processCalls; }
        unsigned processCalls = 0;
    };

    [[nodiscard]] GraphError expectedError(reference::Decision decision) {
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

    void expectMetadata(const DAG& graph, const reference::Graph& model,
                        const std::array<Node*, reference::nodeCount>& addresses) {
        ASSERT_EQ(graph.outputNodes().size(), 2U);
        EXPECT_EQ(graph.outputNodes()[0], model.nodes.front());
        EXPECT_EQ(graph.outputNodes()[1], model.nodes.back());
        for (std::size_t i = 0; i < model.nodes.size(); ++i) {
            ASSERT_EQ(graph.node(model.nodes[i]), addresses[i]);
            EXPECT_EQ(static_cast<const OracleNode*>(addresses[i])->processCalls, 0U);
        }
        EXPECT_EQ(graph.node(NodeID{5}), nullptr);
    }

    /** No graph span escapes this observation or survives a mutation. */
    void expectBaseline(const DAG& graph, const reference::Graph& model,
                        const std::array<Node*, reference::nodeCount>& addresses) {
        ASSERT_EQ(graph.connections().size(), model.edges.size());
        for (std::size_t i = 0; i < model.edges.size(); ++i) {
            EXPECT_EQ(graph.connections()[i].from, model.edges[i].from);
            EXPECT_EQ(graph.connections()[i].to, model.edges[i].to);
        }
        ASSERT_NO_FATAL_FAILURE(expectMetadata(graph, model, addresses));
    }

    TEST(FourNodeOracle, AllAcyclicMasksAndCandidateRelationsMatchIndependentClosureAndOccupancy) {
        unsigned acyclicMasks = 0;
        std::size_t attempts = 0;
        std::array<std::size_t, 7> decisions{};
        for (unsigned mask = 0; mask < reference::maskCount; ++mask) {
            const auto model = reference::fromMask(static_cast<std::uint16_t>(mask));
            if (!model.acyclic()) continue;
            ++acyclicMasks;
            SCOPED_TRACE(::testing::Message() << "seed=0 mask=" << mask);
            DAG graph;
            std::array<Node*, reference::nodeCount> addresses{};
            for (std::size_t i = 0; i < reference::nodeCount; ++i) {
                const auto added = graph.add<OracleNode>();
                ASSERT_TRUE(added);
                ASSERT_EQ(*added, model.nodes[i]);
                addresses[i] = graph.node(*added);
            }
            for (const auto& edge : model.edges) ASSERT_TRUE(graph.connect(edge.to, edge.from));
            ASSERT_TRUE(graph.markAsOutput(model.nodes.back()));
            ASSERT_TRUE(graph.markAsOutput(model.nodes.front()));
            ASSERT_NO_FATAL_FAILURE(expectBaseline(graph, model, addresses));

            for (std::size_t source = 0; source < reference::nodeCount; ++source) {
                for (std::size_t destination = 0; destination < reference::nodeCount; ++destination) {
                    const auto sourceID = model.nodes[source];
                    const auto destinationID = model.nodes[destination];
                    const auto occupiedIndex = static_cast<std::uint16_t>(10 + source);
                    const std::array candidates{
                        reference::Edge{reference::channel(sourceID, reference::out, 0),
                                        reference::channel(destinationID, reference::in, occupiedIndex)},
                        reference::Edge{reference::channel(sourceID, reference::out, 65535),
                                        reference::channel(destinationID, reference::in, occupiedIndex)},
                        reference::Edge{reference::channel(sourceID, reference::out, 0),
                                        reference::channel(destinationID, reference::aux, occupiedIndex)},
                        reference::Edge{reference::channel(sourceID, reference::out, 0),
                                        reference::channel(destinationID, reference::in, 65535)},
                        reference::Edge{reference::channel(sourceID, reference::in, 0),
                                        reference::channel(destinationID, reference::in, occupiedIndex)},
                        reference::Edge{reference::channel(sourceID, reference::out, 0),
                                        reference::channel(destinationID, reference::out, occupiedIndex)}};
                    for (std::size_t variant = 0; variant < candidates.size(); ++variant) {
                        const auto& candidate = candidates[variant];
                        SCOPED_TRACE(::testing::Message() << "source=" << sourceID
                            << " destination=" << destinationID << " variant=" << variant
                            << " from=0x" << std::hex << candidate.from << " to=0x" << candidate.to);
                        const auto prediction = model.predict(candidate);
                        ++attempts;
                        ++decisions[static_cast<std::size_t>(prediction)];
                        const auto actual = graph.connect(candidate.to, candidate.from);
                        if (prediction == reference::Decision::Accepted) {
                            ASSERT_TRUE(actual);
                            ASSERT_EQ(graph.connections().size(), model.edges.size() + 1);
                            for (std::size_t i = 0; i < model.edges.size(); ++i) {
                                EXPECT_EQ(graph.connections()[i].from, model.edges[i].from);
                                EXPECT_EQ(graph.connections()[i].to, model.edges[i].to);
                            }
                            EXPECT_EQ(graph.connections().back(), (Connection{candidate.from, candidate.to}));
                            ASSERT_NO_FATAL_FAILURE(expectMetadata(graph, model, addresses));
                            const auto removed = graph.disconnect(candidate.to, candidate.from);
                            ASSERT_TRUE(removed);
                            ASSERT_TRUE(*removed);
                        } else {
                            ASSERT_FALSE(actual);
                            EXPECT_EQ(actual.error(), expectedError(prediction));
                        }
                        ASSERT_NO_FATAL_FAILURE(expectBaseline(graph, model, addresses));
                    }
                }
            }
        }
        EXPECT_EQ(reference::maskCount, 4096U);
        EXPECT_EQ(acyclicMasks, 543U);
        EXPECT_EQ(attempts, std::size_t{543} * 16 * 6);
        EXPECT_GT(decisions[static_cast<std::size_t>(reference::Decision::Accepted)], 0U);
        EXPECT_GT(decisions[static_cast<std::size_t>(reference::Decision::InvalidDirection)], 0U);
        EXPECT_GT(decisions[static_cast<std::size_t>(reference::Decision::ConnectionExists)], 0U);
        EXPECT_GT(decisions[static_cast<std::size_t>(reference::Decision::InputOccupied)], 0U);
        EXPECT_GT(decisions[static_cast<std::size_t>(reference::Decision::CycleDetected)], 0U);
    }
}
