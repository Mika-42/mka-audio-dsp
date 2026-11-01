#include <algorithm>
#include <array>
#include <cstddef>
#include <expected>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

import mka.audio.dsp.dag;

namespace {
    using namespace mka::audio::dsp;

    enum class Role : std::size_t { Source, Gain, Mix, Output };

    struct EditingTrace {
        std::array<unsigned, 4> destructions{};
        unsigned processCalls = 0;
        bool destroyedOnControl = true;
        std::thread::id control = std::this_thread::get_id();
    };

    /** Test-owned trace outlives the DAG; this scenario never calls process. */
    class EditingNode : public Node {
    public:
        ~EditingNode() override {
            ++trace_.destructions[static_cast<std::size_t>(role_)];
            trace_.destroyedOnControl = trace_.destroyedOnControl &&
                std::this_thread::get_id() == trace_.control;
        }

        void process(const ProcessContext&) noexcept final { ++trace_.processCalls; }

    protected:
        EditingNode(NodeID id, const Config& config, EditingTrace& trace, Role role)
            : Node(id, config), trace_(trace), role_(role) {}

    private:
        EditingTrace& trace_;
        Role role_;
    };

    class Source final : public EditingNode {
    public:
        Source(NodeID id, EditingTrace& trace)
            : EditingNode(id, Config{.layoutOutput = {0}}, trace, Role::Source) {}
    };
    class Gain final : public EditingNode {
    public:
        Gain(NodeID id, EditingTrace& trace)
            : EditingNode(id, Config{.layoutInput = {0}, .layoutOutput = {0}}, trace, Role::Gain) {}
    };
    class Mix final : public EditingNode {
    public:
        Mix(NodeID id, EditingTrace& trace)
            : EditingNode(id, Config{.layoutInput = {0, 1}, .layoutAuxiliary = {0},
                                     .layoutOutput = {0}}, trace, Role::Mix) {}
    };
    class Output final : public EditingNode {
    public:
        Output(NodeID id, EditingTrace& trace)
            : EditingNode(id, Config{.layoutInput = {0}}, trace, Role::Output) {}
    };

    struct ExpectedEditingState {
        std::vector<std::pair<NodeID, Node*>> nodes;
        std::vector<Connection> edges;
        std::vector<NodeID> outputs;
        std::array<unsigned, 4> destructions{};
    };

    /**
     * @brief Observe the entire desired graph on control after each attempted edit.
     * @details Expected values are an independent oracle, not borrowed graph spans.
     * The structural plan is local and retired before another mutation; no audio
     * plan, backend buffers, callback or process call participates in this test.
     */
    void expectEditingState(const DAG& graph, const ExpectedEditingState& expected,
                            const EditingTrace& trace) {
        std::vector<NodeID> expectedIDs;
        for (const auto& [id, address] : expected.nodes) {
            EXPECT_EQ(graph.node(id), address);
            expectedIDs.push_back(id);
        }
        auto structural = graph.compile();
        ASSERT_TRUE(structural);
        std::vector<NodeID> actualIDs;
        for (const auto* node : structural->tasks()) actualIDs.push_back(node->id());
        std::ranges::sort(expectedIDs);
        std::ranges::sort(actualIDs);
        EXPECT_EQ(actualIDs, expectedIDs);

        ASSERT_EQ(graph.connections().size(), expected.edges.size());
        for (std::size_t i = 0; i < expected.edges.size(); ++i)
            EXPECT_EQ(graph.connections()[i], expected.edges[i]);
        ASSERT_EQ(graph.outputNodes().size(), expected.outputs.size());
        for (std::size_t i = 0; i < expected.outputs.size(); ++i)
            EXPECT_EQ(graph.outputNodes()[i], expected.outputs[i]);
        EXPECT_EQ(trace.destructions, expected.destructions);
        EXPECT_TRUE(trace.destroyedOnControl);
        EXPECT_EQ(trace.processCalls, 0U);
    }

    TEST(EditableDAGIntegration, SourceGainMixOutputEditsPreserveExactDesiredStateWithoutAudio) {
        EditingTrace trace;
        {
            DAG graph;
            ExpectedEditingState expected;
            ASSERT_NO_FATAL_FAILURE(expectEditingState(graph, expected, trace));

            const auto source = graph.add<Source>(trace);
            ASSERT_TRUE(source);
            expected.nodes.emplace_back(*source, graph.node(*source));
            ASSERT_NO_FATAL_FAILURE(expectEditingState(graph, expected, trace));
            const auto gain = graph.add<Gain>(trace);
            ASSERT_TRUE(gain);
            expected.nodes.emplace_back(*gain, graph.node(*gain));
            ASSERT_NO_FATAL_FAILURE(expectEditingState(graph, expected, trace));
            const auto mix = graph.add<Mix>(trace);
            ASSERT_TRUE(mix);
            expected.nodes.emplace_back(*mix, graph.node(*mix));
            ASSERT_NO_FATAL_FAILURE(expectEditingState(graph, expected, trace));
            const auto output = graph.add<Output>(trace);
            ASSERT_TRUE(output);
            expected.nodes.emplace_back(*output, graph.node(*output));
            ASSERT_NO_FATAL_FAILURE(expectEditingState(graph, expected, trace));

            const auto sourceOut = encode(*source, PortType::Out, 0);
            const auto gainIn = encode(*gain, PortType::In, 0);
            const auto gainOut = encode(*gain, PortType::Out, 0);
            const auto mixIn = encode(*mix, PortType::In, 0);
            const auto mixOtherIn = encode(*mix, PortType::In, 1);
            const auto mixAux = encode(*mix, PortType::Aux, 0);
            const auto mixOut = encode(*mix, PortType::Out, 0);
            const auto outputIn = encode(*output, PortType::In, 0);

            const auto addEdge = [&](ChannelID destination, ChannelID producer) {
                ASSERT_TRUE(graph.connect(destination, producer));
                expected.edges.push_back({producer, destination});
                ASSERT_NO_FATAL_FAILURE(expectEditingState(graph, expected, trace));
            };
            ASSERT_NO_FATAL_FAILURE(addEdge(gainIn, sourceOut));
            ASSERT_NO_FATAL_FAILURE(addEdge(mixIn, gainOut));
            ASSERT_NO_FATAL_FAILURE(addEdge(outputIn, mixOut));
            ASSERT_NO_FATAL_FAILURE(addEdge(mixAux, sourceOut));
            ASSERT_NO_FATAL_FAILURE(addEdge(mixOtherIn, sourceOut)); // Same Out fan-out, same Node pair as Aux.

            ASSERT_TRUE(graph.markAsOutput(*output));
            expected.outputs = {*output};
            ASSERT_NO_FATAL_FAILURE(expectEditingState(graph, expected, trace));
            ASSERT_TRUE(graph.markAsOutput(*gain)); // An output may keep successors.
            expected.outputs = {*gain, *output};
            ASSERT_NO_FATAL_FAILURE(expectEditingState(graph, expected, trace));
            ASSERT_TRUE(graph.markAsOutput(*gain));
            ASSERT_NO_FATAL_FAILURE(expectEditingState(graph, expected, trace));
            ASSERT_TRUE(graph.unmarkAsOutput(*gain));
            expected.outputs = {*output};
            ASSERT_NO_FATAL_FAILURE(expectEditingState(graph, expected, trace));
            ASSERT_TRUE(graph.markAsOutput(*gain));
            expected.outputs = {*gain, *output};
            ASSERT_NO_FATAL_FAILURE(expectEditingState(graph, expected, trace));

            const auto rejectEdge = [&](ChannelID destination, ChannelID producer, GraphError error) {
                const auto rejected = graph.connect(destination, producer);
                ASSERT_FALSE(rejected);
                EXPECT_EQ(rejected.error().code, error.code);
                ASSERT_NO_FATAL_FAILURE(expectEditingState(graph, expected, trace));
            };
            ASSERT_NO_FATAL_FAILURE(rejectEdge(mixIn, encode(*source, PortType::Out, 1), GraphError::InputOccupied));
            ASSERT_NO_FATAL_FAILURE(rejectEdge(gainIn, sourceOut, GraphError::ConnectionExists));
            ASSERT_NO_FATAL_FAILURE(rejectEdge(sourceOut, gainIn, GraphError::InvalidDirection)); // Inverted arguments.
            ASSERT_NO_FATAL_FAILURE(rejectEdge(encode(*output, PortType::Out, 0), sourceOut, GraphError::InvalidDirection));
            ASSERT_NO_FATAL_FAILURE(rejectEdge(encode(*source, PortType::In, 0),
                                              encode(*output, PortType::Out, 0), GraphError::CycleDetected));

            // Neither endpoint index/port is declared in these Config snapshots.
            const auto outsideSource = encode(*source, PortType::Out, 65535);
            const auto outsideDestination = encode(*output, PortType::Aux, 65535);
            ASSERT_NO_FATAL_FAILURE(addEdge(outsideDestination, outsideSource));

            const auto disconnected = graph.disconnect(mixOtherIn, sourceOut);
            ASSERT_TRUE(disconnected);
            EXPECT_TRUE(*disconnected);
            expected.edges.erase(expected.edges.begin() + 4);
            ASSERT_NO_FATAL_FAILURE(expectEditingState(graph, expected, trace));
            // This exact In is gone; Source -> Mix.Aux and Source -> Output.Aux remain.
            const auto alreadyAbsent = graph.disconnect(mixOtherIn, sourceOut);
            ASSERT_TRUE(alreadyAbsent);
            EXPECT_FALSE(*alreadyAbsent);
            ASSERT_NO_FATAL_FAILURE(expectEditingState(graph, expected, trace));

            ASSERT_TRUE(graph.remove(*gain));
            expected.nodes.erase(expected.nodes.begin() + 1);
            expected.edges.erase(expected.edges.begin(), expected.edges.begin() + 2);
            expected.outputs = {*output};
            expected.destructions[static_cast<std::size_t>(Role::Gain)] = 1;
            EXPECT_EQ(graph.node(*gain), nullptr);
            ASSERT_NO_FATAL_FAILURE(expectEditingState(graph, expected, trace));

            ASSERT_TRUE(graph.remove(*source));
            expected.nodes.erase(expected.nodes.begin());
            expected.edges = {{mixOut, outputIn}};
            expected.destructions[static_cast<std::size_t>(Role::Source)] = 1;
            EXPECT_EQ(graph.node(*source), nullptr);
            ASSERT_NO_FATAL_FAILURE(expectEditingState(graph, expected, trace));

            ASSERT_TRUE(graph.remove(*mix));
            expected.nodes.erase(expected.nodes.begin());
            expected.edges.clear();
            expected.destructions[static_cast<std::size_t>(Role::Mix)] = 1;
            EXPECT_EQ(graph.node(*mix), nullptr);
            ASSERT_NO_FATAL_FAILURE(expectEditingState(graph, expected, trace));

            ASSERT_TRUE(graph.remove(*output));
            expected.nodes.clear();
            expected.outputs.clear();
            expected.destructions[static_cast<std::size_t>(Role::Output)] = 1;
            EXPECT_EQ(graph.node(*output), nullptr);
            ASSERT_NO_FATAL_FAILURE(expectEditingState(graph, expected, trace));
        }
        EXPECT_EQ(trace.destructions, (std::array<unsigned, 4>{1, 1, 1, 1}));
        EXPECT_TRUE(trace.destroyedOnControl);
        EXPECT_EQ(trace.processCalls, 0U);
    }
}
