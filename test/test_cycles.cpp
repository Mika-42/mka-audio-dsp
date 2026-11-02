#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <gtest/gtest.h>

import mka.audio.dsp.dag;

namespace {
    using namespace mka::audio::dsp;

    class CycleNode final : public Node {
    public:
        explicit CycleNode(NodeID id) : Node(id, Config{}) {}
        void process(const ProcessContext&) noexcept override { ++processCalls; }
        unsigned processCalls = 0;
    };

    void expectError(const Result& result, GraphError error) {
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, error.code);
    }

    void expectEdges(const DAG& graph, std::span<const Connection> expected) {
        ASSERT_EQ(graph.connections().size(), expected.size());
        for (std::size_t i = 0; i < expected.size(); ++i)
            EXPECT_EQ(graph.connections()[i], expected[i]);
    }

    TEST(CycleReachability, OneTwoAndFourNodeCyclesLeaveEdgesMarkersAndBorrowedTasksUnchanged) {
        for (const std::size_t count : {1U, 2U, 4U}) {
            SCOPED_TRACE(count);
            DAG graph;
            std::vector<NodeID> ids;
            std::vector<Node*> addresses;
            std::vector<Connection> expected;
            for (std::size_t i = 0; i < count; ++i) {
                const auto added = graph.add<CycleNode>();
                ASSERT_TRUE(added);
                ids.push_back(*added);
                addresses.push_back(graph.node(*added));
            }
            for (std::size_t i = 1; i < count; ++i) {
                const Connection edge{encode(ids[i - 1], PortType::Out, 65535),
                                      encode(ids[i], PortType::In, 65535)};
                ASSERT_TRUE(graph.connect(edge.to, edge.from));
                expected.push_back(edge);
            }
            ASSERT_TRUE(graph.markAsOutput(ids.back()));
            auto oldPlan = graph.compile();
            ASSERT_TRUE(oldPlan);
            const std::vector<Node*> oldTasks{oldPlan->tasks().begin(), oldPlan->tasks().end()};
            // This destination is free: the refusal must identify the cycle,
            // rather than duplicate/occupancy, including the one-node case.
            expectError(graph.connect(encode(ids.front(), PortType::Aux, 65535),
                                      encode(ids.back(), PortType::Out, 42)),
                        GraphError::CycleDetected);
            ASSERT_NO_FATAL_FAILURE(expectEdges(graph, expected));
            ASSERT_EQ(graph.outputNodes().size(), 1U);
            EXPECT_EQ(graph.outputNodes()[0], ids.back());
            ASSERT_EQ(oldPlan->tasks().size(), oldTasks.size());
            for (std::size_t i = 0; i < count; ++i) {
                EXPECT_EQ(graph.node(ids[i]), addresses[i]);
                EXPECT_EQ(oldPlan->tasks()[i], oldTasks[i]);
                EXPECT_EQ(static_cast<CycleNode*>(addresses[i])->processCalls, 0U);
            }
            ASSERT_TRUE(graph.compile());
        }
    }

    TEST(CycleReachability, DiamondFanOutAndParallelLogicalEdgesRemainLegal) {
        DAG graph;
        std::array<NodeID, 4> ids;
        for (auto& id : ids) {
            const auto added = graph.add<CycleNode>();
            ASSERT_TRUE(added);
            id = *added;
        }
        const std::array expected{
            Connection{encode(ids[0], PortType::Out, 0), encode(ids[1], PortType::In, 0)},
            Connection{encode(ids[0], PortType::Out, 0), encode(ids[2], PortType::Aux, 0)},
            Connection{encode(ids[1], PortType::Out, 7), encode(ids[3], PortType::In, 0)},
            Connection{encode(ids[2], PortType::Out, 9), encode(ids[3], PortType::Aux, 0)},
            Connection{encode(ids[0], PortType::Out, 1), encode(ids[1], PortType::Aux, 1)},
            Connection{encode(ids[0], PortType::Out, 0), encode(ids[3], PortType::In, 1)}};
        for (const auto& edge : expected) ASSERT_TRUE(graph.connect(edge.to, edge.from));
        ASSERT_NO_FATAL_FAILURE(expectEdges(graph, expected));
        expectError(graph.connect(encode(ids[0], PortType::Out, 65535),
                                  encode(ids[3], PortType::Aux, 65535)),
                    GraphError::InvalidDirection); // Old source-first convention.
        expectError(graph.connect(encode(ids[0], PortType::In, 65535),
                                  encode(ids[3], PortType::Out, 65535)), GraphError::CycleDetected);
        ASSERT_NO_FATAL_FAILURE(expectEdges(graph, expected));
        auto plan = graph.compile();
        ASSERT_TRUE(plan);
        ASSERT_EQ(plan->tasks().size(), 4U);
        EXPECT_EQ(plan->tasks().front()->id(), ids[0]);
        EXPECT_EQ(plan->tasks().back()->id(), ids[3]);
    }

    TEST(CycleReachability, UndeclaredAuxiliaryEdgesCountAndDisconnectRemovesTheirReachability) {
        DAG graph;
        std::array<NodeID, 4> ids;
        for (auto& id : ids) {
            const auto added = graph.add<CycleNode>();
            ASSERT_TRUE(added);
            id = *added;
        }
        const Connection first{encode(ids[0], PortType::Out, 9), encode(ids[1], PortType::Aux, 65535)};
        const Connection middle{encode(ids[1], PortType::Out, 6), encode(ids[2], PortType::In, 42)};
        const Connection last{encode(ids[2], PortType::Out, 1), encode(ids[3], PortType::Aux, 7)};
        for (const auto edge : {first, middle, last}) ASSERT_TRUE(graph.connect(edge.to, edge.from));
        const Connection candidate{encode(ids[3], PortType::Out, 3), encode(ids[0], PortType::In, 65535)};
        expectError(graph.connect(candidate.to, candidate.from), GraphError::CycleDetected);
        const std::array before{first, middle, last};
        ASSERT_NO_FATAL_FAILURE(expectEdges(graph, before));
        const auto removed = graph.disconnect(middle.to, middle.from);
        ASSERT_TRUE(removed);
        ASSERT_TRUE(*removed);
        ASSERT_TRUE(graph.connect(candidate.to, candidate.from));
        const std::array after{first, last, candidate};
        ASSERT_NO_FATAL_FAILURE(expectEdges(graph, after));
        ASSERT_TRUE(graph.compile());
    }

    TEST(CycleReachability, LongChainUsesNoRecursionAndHandlesGapsAndAnIsolatedNode) {
        DAG graph;
        std::vector<NodeID> allocated;
        for (std::size_t i = 0; i < 1026; ++i) {
            const auto added = graph.add<CycleNode>();
            ASSERT_TRUE(added);
            allocated.push_back(*added);
        }
        ASSERT_TRUE(graph.remove(allocated[1]));
        ASSERT_TRUE(graph.remove(allocated[513]));
        std::vector<NodeID> chain;
        for (std::size_t i = 0; i < allocated.size(); ++i)
            if (i != 1 && i != 513) chain.push_back(allocated[i]);
        ASSERT_EQ(chain.size(), 1024U);
        const auto isolated = graph.add<CycleNode>();
        ASSERT_TRUE(isolated);
        Node* const isolatedAddress = graph.node(*isolated);
        for (std::size_t i = 1; i < chain.size(); ++i)
            ASSERT_TRUE(graph.connect(encode(chain[i], PortType::In, 0),
                                      encode(chain[i - 1], PortType::Out, 0)));
        ASSERT_TRUE(graph.markAsOutput(chain.back()));
        expectError(graph.connect(encode(chain.front(), PortType::In, 65535),
                                  encode(chain.back(), PortType::Out, 65535)), GraphError::CycleDetected);
        EXPECT_EQ(graph.connections().size(), chain.size() - 1);
        EXPECT_EQ(graph.node(allocated[1]), nullptr);
        EXPECT_EQ(graph.node(allocated[513]), nullptr);
        EXPECT_EQ(graph.node(*isolated), isolatedAddress);
        ASSERT_EQ(graph.outputNodes().size(), 1U);
        EXPECT_EQ(graph.outputNodes()[0], chain.back());
        ASSERT_TRUE(graph.connect(encode(*isolated, PortType::In, 42),
                                  encode(chain.front(), PortType::Out, 0)));
        auto plan = graph.compile();
        ASSERT_TRUE(plan);
        EXPECT_EQ(plan->tasks().size(), 1025U);
    }

    TEST(CycleReachability, SampledFourNodeReachabilityMatchesIndependentTransitiveClosure) {
        // Representative masks only: exhaustive enumeration belongs to S03-04.
        // Oracle arithmetic/matrix storage never calls the production traversal.
        constexpr std::array<std::uint16_t, 6> masks{0, 7, 291, 273, 3584, 2184};
        for (const auto mask : masks) {
            SCOPED_TRACE(mask);
            DAG graph;
            std::array<NodeID, 4> ids;
            for (auto& id : ids) {
                const auto added = graph.add<CycleNode>();
                ASSERT_TRUE(added);
                id = *added;
            }
            std::array<std::array<bool, 4>, 4> reachable{};
            std::vector<Connection> baseline;
            unsigned bit = 0;
            for (std::size_t source = 0; source < 4; ++source) {
                for (std::size_t destination = 0; destination < 4; ++destination) {
                    if (source == destination) continue;
                    if ((mask & (std::uint16_t{1} << bit)) != 0) {
                        reachable[source][destination] = true;
                        const Connection edge{
                            encode(ids[source], PortType::Out, 0),
                            encode(ids[destination], PortType::In, static_cast<ChannelIndex>(10 + source))};
                        ASSERT_TRUE(graph.connect(edge.to, edge.from));
                        baseline.push_back(edge);
                    }
                    ++bit;
                }
            }
            // Floyd-Warshall differs from the production adjacency DFS/BFS.
            for (std::size_t via = 0; via < 4; ++via)
                for (std::size_t source = 0; source < 4; ++source)
                    for (std::size_t destination = 0; destination < 4; ++destination)
                        reachable[source][destination] = reachable[source][destination] ||
                            (reachable[source][via] && reachable[via][destination]);
            for (std::size_t source = 0; source < 4; ++source) EXPECT_FALSE(reachable[source][source]);
            ASSERT_TRUE(graph.markAsOutput(ids[3]));
            for (std::size_t source = 0; source < 4; ++source) {
                for (std::size_t destination = 0; destination < 4; ++destination) {
                    SCOPED_TRACE(source);
                    SCOPED_TRACE(destination);
                    const Connection candidate{encode(ids[source], PortType::Out, 65535),
                        encode(ids[destination], destination % 2 ? PortType::Aux : PortType::In, 65535)};
                    const bool wouldCycle = source == destination || reachable[destination][source];
                    const auto result = graph.connect(candidate.to, candidate.from);
                    if (wouldCycle) {
                        ASSERT_FALSE(result);
                        EXPECT_EQ(result.error(), GraphError::CycleDetected);
                    } else {
                        ASSERT_TRUE(result);
                        ASSERT_EQ(graph.connections().size(), baseline.size() + 1);
                        EXPECT_EQ(graph.connections().back(), candidate);
                        const auto removed = graph.disconnect(candidate.to, candidate.from);
                        ASSERT_TRUE(removed);
                        ASSERT_TRUE(*removed);
                    }
                    ASSERT_NO_FATAL_FAILURE(expectEdges(graph, baseline));
                    ASSERT_EQ(graph.outputNodes().size(), 1U);
                    EXPECT_EQ(graph.outputNodes()[0], ids[3]);
                }
            }
        }
    }
}
