#include <array>
#include <concepts>
#include <cstddef>
#include <expected>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

import mka.audio.dsp.dag;

namespace {
    using namespace mka::audio::dsp;

    class MarkerNode final : public Node {
    public:
        explicit MarkerNode(const NodeID id)
            : Node(id, Config{.layoutInput = {0}, .layoutOutput = {0}}) {}

        void process(const ProcessContext& context) noexcept override {
            inputCount = context.inputs.size();
            backendCount = context.backendOutputs.size();
            logicalCount = context.outputs.size();
            distinctDestinations = true;
            for (const auto& logical : context.outputs)
                for (const auto& physical : context.backendOutputs)
                    distinctDestinations = distinctDestinations && logical.buffer != physical.buffer;
            for (FrameCount frame = 0; frame < context.frames; ++frame) {
                if (evenFramesOnly && frame % 2 != 0) continue;
                const float input = context.inputs.empty() ? 0.F : context.inputs[0].buffer[frame];
                lastSample = constant ? value : input * gain;
                if (!context.outputs.empty()) context.outputs[0].buffer[frame] = lastSample;
                if (outputLane < context.backendOutputs.size())
                    context.backendOutputs[outputLane].buffer[frame] = lastSample + backendBias;
            }
        }

        float gain = 1.F;
        float backendBias = 0.F;
        float value = 0.F;
        float lastSample = 0.F;
        bool constant = false;
        bool evenFramesOnly = false;
        bool distinctDestinations = true;
        std::size_t outputLane = 0;
        std::size_t inputCount = 0, backendCount = 0, logicalCount = 0;
    };

    static_assert(std::same_as<decltype(std::declval<DAG&>().markAsOutput(NodeID{})),
                               std::expected<void, GraphError>>);
    static_assert(std::same_as<decltype(std::declval<DAG&>().unmarkAsOutput(NodeID{})),
                               std::expected<void, GraphError>>);
    static_assert(noexcept(std::declval<DAG&>().unmarkAsOutput(NodeID{})));
    static_assert(std::same_as<decltype(std::declval<const DAG&>().outputNodes()),
                               std::span<const NodeID>>);
    static_assert(noexcept(std::declval<const DAG&>().outputNodes()));
    static_assert(!std::is_assignable_v<decltype(std::declval<const DAG&>().outputNodes()[0]), NodeID>);

    void expectError(const Result& result, const GraphError error) {
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, error.code);
    }

    void expectMarkers(const DAG& graph, const std::span<const NodeID> expected) {
        ASSERT_EQ(graph.outputNodes().size(), expected.size());
        for (std::size_t i = 0; i < expected.size(); ++i) EXPECT_EQ(graph.outputNodes()[i], expected[i]);
    }

    TEST(OutputMarkers, InvalidAndRepeatedEditsPreserveAUniqueSortedControlCollection) {
        DAG graph;
        const auto a = graph.add<MarkerNode>();
        const auto b = graph.add<MarkerNode>();
        const auto c = graph.add<MarkerNode>();
        ASSERT_TRUE(a); ASSERT_TRUE(b); ASSERT_TRUE(c);
        ASSERT_TRUE(graph.unmarkAsOutput(*b));
        ASSERT_TRUE(graph.unmarkAsOutput(*b));
        ASSERT_TRUE(graph.markAsOutput(*c));
        ASSERT_TRUE(graph.markAsOutput(*a));
        ASSERT_TRUE(graph.markAsOutput(*b));
        ASSERT_TRUE(graph.markAsOutput(*b));
        const std::array all{*a, *b, *c};
        expectMarkers(graph, all);
        const std::vector<NodeID> detached{graph.outputNodes().begin(), graph.outputNodes().end()};
        for (const auto missing : {invalidNodeID, maxNodeID}) {
            expectError(graph.markAsOutput(missing), GraphError::NodeNotFound);
            expectError(graph.unmarkAsOutput(missing), GraphError::NodeNotFound);
            expectMarkers(graph, all);
        }
        ASSERT_TRUE(graph.unmarkAsOutput(*b));
        ASSERT_TRUE(graph.unmarkAsOutput(*b));
        const std::array remaining{*a, *c};
        expectMarkers(graph, remaining);
        ASSERT_TRUE(graph.markAsOutput(*b));
        expectMarkers(graph, all);
        EXPECT_EQ(detached, (std::vector<NodeID>{*a, *b, *c}));
        EXPECT_TRUE(graph.connections().empty());
    }

    TEST(OutputMarkers, RemovingANodeDropsItsMarkerWithoutCreatingHardwareEdges) {
        DAG graph;
        const auto a = graph.add<MarkerNode>();
        const auto b = graph.add<MarkerNode>();
        ASSERT_TRUE(a); ASSERT_TRUE(b);
        ASSERT_TRUE(graph.markAsOutput(*b));
        ASSERT_TRUE(graph.markAsOutput(*a));
        EXPECT_TRUE(graph.connections().empty());
        ASSERT_TRUE(graph.remove(*a));
        const std::array expected{*b};
        expectMarkers(graph, expected);
        expectError(graph.markAsOutput(*a), GraphError::NodeNotFound);
        expectError(graph.unmarkAsOutput(*a), GraphError::NodeNotFound);
        expectMarkers(graph, expected);
        EXPECT_TRUE(graph.connections().empty());
        ASSERT_TRUE(graph.compile(BackendConfig{.maxFrames = 1, .outputChannels = 2}));
    }

    TEST(OutputMarkers, MarkUnmarkAndRemarkCreateIndependentSnapshotsForASourceWithASuccessor) {
        DAG graph;
        const auto source = graph.add<MarkerNode>();
        const auto sink = graph.add<MarkerNode>();
        ASSERT_TRUE(source); ASSERT_TRUE(sink);
        auto* producer = static_cast<MarkerNode*>(graph.node(*source));
        auto* consumer = static_cast<MarkerNode*>(graph.node(*sink));
        producer->gain = 2; producer->backendBias = 100;
        consumer->gain = 3;
        ASSERT_TRUE(graph.connect(encode(*sink, PortType::In, 0), encode(*source, PortType::Out, 0)));
        ASSERT_TRUE(graph.markAsOutput(*source));
        const BackendConfig config{.maxFrames = 1, .inputChannels = 1, .outputChannels = 1};
        auto original = graph.compile(config); ASSERT_TRUE(original);
        ASSERT_TRUE(graph.unmarkAsOutput(*source));
        auto middle = graph.compile(config); ASSERT_TRUE(middle);
        ASSERT_TRUE(graph.markAsOutput(*source));
        auto latest = graph.compile(config); ASSERT_TRUE(latest);
        const float input = 3; float first = 99, second = 99, third = 99;
        const std::array inputs{&input};
        const std::array firstOutputs{&first}, secondOutputs{&second}, thirdOutputs{&third};
        ASSERT_TRUE(original->exec(BackendBlock{inputs, firstOutputs, 1}));
        EXPECT_EQ(first, 106); EXPECT_EQ(consumer->lastSample, 18);
        EXPECT_EQ(producer->inputCount, 1U); EXPECT_EQ(producer->backendCount, 1U);
        EXPECT_EQ(producer->logicalCount, 1U); EXPECT_TRUE(producer->distinctDestinations);
        EXPECT_EQ(consumer->backendCount, 0U);
        ASSERT_TRUE(middle->exec(BackendBlock{inputs, secondOutputs, 1}));
        EXPECT_EQ(second, 0); EXPECT_EQ(consumer->lastSample, 18);
        EXPECT_EQ(producer->backendCount, 0U);
        ASSERT_TRUE(latest->exec(BackendBlock{inputs, thirdOutputs, 1}));
        EXPECT_EQ(third, 106); EXPECT_EQ(consumer->lastSample, 18);
        EXPECT_EQ(producer->backendCount, 1U); EXPECT_TRUE(producer->distinctDestinations);
        EXPECT_EQ(first, 106); EXPECT_EQ(second, 0);
        EXPECT_EQ(graph.connections().size(), 1U);
    }

    TEST(OutputMarkers, UnmarkingANonSourceKeepsLogicalOutputsForItsSuccessor) {
        DAG graph;
        const auto a = graph.add<MarkerNode>();
        const auto b = graph.add<MarkerNode>();
        const auto c = graph.add<MarkerNode>();
        ASSERT_TRUE(a); ASSERT_TRUE(b); ASSERT_TRUE(c);
        auto* middle = static_cast<MarkerNode*>(graph.node(*b));
        auto* last = static_cast<MarkerNode*>(graph.node(*c));
        static_cast<MarkerNode*>(graph.node(*a))->gain = 2;
        middle->gain = 3; middle->backendBias = 100;
        last->gain = 4;
        ASSERT_TRUE(graph.connect(encode(*b, PortType::In, 0), encode(*a, PortType::Out, 0)));
        ASSERT_TRUE(graph.connect(encode(*c, PortType::In, 0), encode(*b, PortType::Out, 0)));
        ASSERT_TRUE(graph.markAsOutput(*b));
        const BackendConfig config{.maxFrames = 1, .inputChannels = 1, .outputChannels = 1};
        auto marked = graph.compile(config); ASSERT_TRUE(marked);
        ASSERT_TRUE(graph.unmarkAsOutput(*b));
        auto unmarked = graph.compile(config); ASSERT_TRUE(unmarked);
        const float input = 2; float output = 99;
        const std::array inputs{&input}; const std::array outputs{&output};
        ASSERT_TRUE(marked->exec(BackendBlock{inputs, outputs, 1}));
        EXPECT_EQ(output, 112); EXPECT_EQ(last->lastSample, 48);
        EXPECT_EQ(middle->backendCount, 1U); EXPECT_TRUE(middle->distinctDestinations);
        ASSERT_TRUE(unmarked->exec(BackendBlock{inputs, outputs, 1}));
        EXPECT_EQ(output, 0); EXPECT_EQ(last->lastSample, 48);
        EXPECT_EQ(middle->backendCount, 0U);
        EXPECT_EQ(graph.connections().size(), 2U);
    }

    TEST(OutputMarkers, StableWritersUseCurrentBackendAddressesAndLastEffectiveSampleWins) {
        DAG graph;
        const auto first = graph.add<MarkerNode>();
        const auto second = graph.add<MarkerNode>();
        ASSERT_TRUE(first); ASSERT_TRUE(second);
        auto* a = static_cast<MarkerNode*>(graph.node(*first));
        auto* b = static_cast<MarkerNode*>(graph.node(*second));
        a->constant = true; a->value = 2;
        b->constant = true; b->value = 7; b->evenFramesOnly = true;
        ASSERT_TRUE(graph.markAsOutput(*second));
        ASSERT_TRUE(graph.markAsOutput(*first)); // Reversed edits cannot reorder tasks.
        const BackendConfig config{.maxFrames = 4, .outputChannels = 2};
        auto both = graph.compile(config); ASSERT_TRUE(both);
        ASSERT_TRUE(graph.unmarkAsOutput(*second));
        auto one = graph.compile(config); ASSERT_TRUE(one);
        std::array<float, 4> oldLane{99,99,99,99}, oldUnused{99,99,99,99};
        std::array<float, 4> newLane{99,99,99,99}, newUnused{99,99,99,99};
        const std::array oldOutputs{oldLane.data(),oldUnused.data()};
        const std::array newOutputs{newLane.data(),newUnused.data()};
        ASSERT_TRUE(both->exec(BackendBlock{.outputs = oldOutputs, .frames = 4}));
        const std::array<float, 4> lastWrites{7,2,7,2};
        EXPECT_EQ(oldLane, lastWrites); EXPECT_EQ(oldUnused, (std::array<float,4>{}));
        EXPECT_EQ(a->backendCount, 2U); EXPECT_EQ(b->backendCount, 2U);
        ASSERT_TRUE(one->exec(BackendBlock{.outputs = newOutputs, .frames = 4}));
        EXPECT_EQ(newLane, (std::array<float,4>{2,2,2,2}));
        EXPECT_EQ(newUnused, (std::array<float,4>{})); EXPECT_EQ(b->backendCount, 0U);
        EXPECT_EQ(oldLane, lastWrites); // Later callback touched only its current addresses.
        ASSERT_TRUE(both->exec(BackendBlock{.outputs = newOutputs, .frames = 4}));
        EXPECT_EQ(newLane, lastWrites); EXPECT_EQ(oldLane, lastWrites);
        EXPECT_EQ(newUnused, (std::array<float,4>{}));
        EXPECT_TRUE(graph.connections().empty());
    }
}
