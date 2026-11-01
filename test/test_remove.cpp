#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <expected>
#include <latch>
#include <limits>
#include <optional>
#include <span>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

import mka.audio.dsp.dag;

namespace {
    using namespace mka::audio::dsp;

    struct RemovalTrace {
        unsigned destructions = 0;
        unsigned calls = 0;
        std::size_t backendCount = 0;
        bool destroyedOnControl = true;
        std::thread::id control = std::this_thread::get_id();
    };

    /** Trace storage outlives its DAG; callback observations are read after join. */
    class RemovalNode final : public Node {
    public:
        RemovalNode(const NodeID id, RemovalTrace& trace)
            : Node(id, Config{.layoutInput = {0}, .layoutAuxiliary = {0}, .layoutOutput = {0}}),
              trace_(trace) {}

        ~RemovalNode() override {
            ++trace_.destructions;
            trace_.destroyedOnControl = trace_.destroyedOnControl &&
                std::this_thread::get_id() == trace_.control;
        }

        void process(const ProcessContext& context) noexcept override {
            ++trace_.calls;
            trace_.backendCount = context.backendOutputs.size();
            for (FrameCount frame = 0; frame < context.frames; ++frame) {
                const float input = context.inputs.empty() ? 0.F : context.inputs[0].buffer[frame];
                const float auxiliary = context.auxiliary.empty() ? 0.F : context.auxiliary[0].buffer[frame];
                const float sample = input + auxiliary + static_cast<float>(id());
                if (!context.outputs.empty()) context.outputs[0].buffer[frame] = sample;
                if (!context.backendOutputs.empty()) context.backendOutputs[0].buffer[frame] = sample;
            }
        }

    private:
        RemovalTrace& trace_; // Test control-owned counters, never callback buffers.
    };

    static_assert(std::same_as<decltype(&DAG::remove),
                               std::expected<void, GraphError> (DAG::*)(NodeID)>);

    void expectError(const Result& result, const GraphError error) {
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error(), error);
    }

    void expectEdges(const DAG& graph, const std::span<const Connection> expected) {
        ASSERT_EQ(graph.connections().size(), expected.size());
        for (std::size_t i = 0; i < expected.size(); ++i)
            EXPECT_EQ(graph.connections()[i], expected[i]);
    }

    constexpr BackendConfig callbackConfig{
        .maxFrames = 1, .inputChannels = 1, .outputChannels = 1};

    TEST(NodeRemoval, AbsentIdentityPreservesEdgesNodeAddressesOutputRolesAndOldPlan) {
        RemovalTrace sourceTrace, sinkTrace;
        DAG graph;
        const auto source = graph.add<RemovalNode>(sourceTrace);
        const auto sink = graph.add<RemovalNode>(sinkTrace);
        ASSERT_TRUE(source);
        ASSERT_TRUE(sink);
        Node* const sourceAddress = graph.node(*source);
        Node* const sinkAddress = graph.node(*sink);
        const Connection edge{encode(*source, PortType::Out, 0), encode(*sink, PortType::In, 0)};
        ASSERT_TRUE(graph.connect(edge.to, edge.from));
        ASSERT_TRUE(graph.markAsOutput(*sink));
        auto active = graph.compile(callbackConfig);
        ASSERT_TRUE(active);
        const float input = 2;
        float output = 99;
        const std::array inputs{&input};
        const std::array outputs{&output};
        for (const auto missing : {invalidNodeID, maxNodeID}) {
            expectError(graph.remove(missing), GraphError::NodeNotFound);
            const std::array expected{edge};
            expectEdges(graph, expected);
            EXPECT_EQ(graph.node(*source), sourceAddress);
            EXPECT_EQ(graph.node(*sink), sinkAddress);
            ASSERT_TRUE(active->exec(BackendBlock{inputs, outputs, 1}));
            EXPECT_EQ(output, 5);
            EXPECT_EQ(sourceTrace.backendCount, 0U);
            EXPECT_EQ(sinkTrace.backendCount, 1U);
            EXPECT_EQ(sourceTrace.destructions + sinkTrace.destructions, 0U);
        }
    }

    TEST(NodeRemoval, IsolatedUnborrowedNodeIsDestroyedImmediatelyOnceOnControl) {
        RemovalTrace trace;
        DAG graph;
        const auto id = graph.add<RemovalNode>(trace);
        ASSERT_TRUE(id);
        ASSERT_TRUE(graph.markAsOutput(*id));
        ASSERT_TRUE(graph.remove(*id));
        EXPECT_EQ(graph.node(*id), nullptr);
        EXPECT_EQ(trace.destructions, 1U);
        EXPECT_TRUE(trace.destroyedOnControl);
        EXPECT_EQ(graph.collectRetiredNodes(), 0U);
        expectError(graph.remove(*id), GraphError::NodeNotFound);
        EXPECT_EQ(trace.destructions, 1U);
        auto empty = graph.compile(callbackConfig);
        ASSERT_TRUE(empty); // A stale output marker would reject preparation.
        EXPECT_TRUE(empty->tasks().empty());
    }

    TEST(NodeRemoval, RemovesAllIncidentEdgesAndOutputMarkerButKeepsUnrelatedEdgesAndOldBindings) {
        RemovalTrace aTrace, sideTrace, victimTrace, sinkTrace;
        DAG graph;
        const auto a = graph.add<RemovalNode>(aTrace);
        const auto side = graph.add<RemovalNode>(sideTrace);
        const auto victim = graph.add<RemovalNode>(victimTrace);
        const auto sink = graph.add<RemovalNode>(sinkTrace);
        ASSERT_TRUE(a);
        ASSERT_TRUE(side);
        ASSERT_TRUE(victim);
        ASSERT_TRUE(sink);
        const std::array preserved{
            Connection{encode(*a, PortType::Out, 1), encode(*side, PortType::In, 1)},
            Connection{encode(*side, PortType::Out, 0), encode(*sink, PortType::In, 1)}};
        ASSERT_TRUE(graph.connect(encode(*victim, PortType::In, 0), encode(*a, PortType::Out, 0)));
        ASSERT_TRUE(graph.connect(encode(*victim, PortType::Aux, 0), encode(*side, PortType::Out, 0)));
        ASSERT_TRUE(graph.connect(encode(*sink, PortType::In, 0), encode(*victim, PortType::Out, 0)));
        ASSERT_TRUE(graph.connect(encode(*sink, PortType::Aux, 0), encode(*victim, PortType::Out, 0)));
        for (const auto& edge : preserved) ASSERT_TRUE(graph.connect(edge.to, edge.from));
        ASSERT_TRUE(graph.markAsOutput(*victim));
        ASSERT_TRUE(graph.markAsOutput(*sink));
        const std::array survivorAddresses{graph.node(*a), graph.node(*side), graph.node(*sink)};
        auto active = graph.compile(callbackConfig);
        ASSERT_TRUE(active);

        ASSERT_TRUE(graph.remove(*victim));
        EXPECT_EQ(graph.node(*victim), nullptr);
        expectEdges(graph, preserved);
        EXPECT_EQ(graph.node(*a), survivorAddresses[0]);
        EXPECT_EQ(graph.node(*side), survivorAddresses[1]);
        EXPECT_EQ(graph.node(*sink), survivorAddresses[2]);
        EXPECT_EQ(victimTrace.destructions, 0U);
        EXPECT_EQ(graph.collectRetiredNodes(), 0U);
        auto replacement = graph.compile(callbackConfig);
        ASSERT_TRUE(replacement); // Removed output marker cannot remain dangling.
        ASSERT_EQ(replacement->tasks().size(), 3U);
        for (const auto* node : replacement->tasks()) EXPECT_NE(node->id(), *victim);
        const float input = 2;
        float output = 99;
        const std::array inputs{&input};
        const std::array outputs{&output};
        ASSERT_TRUE(replacement->exec(BackendBlock{inputs, outputs, 1}));
        EXPECT_EQ(output, 4);
        EXPECT_EQ(victimTrace.calls, 0U);
        EXPECT_EQ(sinkTrace.backendCount, 1U);
        ASSERT_TRUE(active->exec(BackendBlock{inputs, outputs, 1}));
        EXPECT_EQ(output, 20); // Old In/Aux bindings and both output roles survive.
        EXPECT_EQ(victimTrace.backendCount, 1U);
        EXPECT_EQ(victimTrace.destructions, 0U);
        active = std::unexpected(GraphError::NodeNotFound);
        EXPECT_EQ(graph.collectRetiredNodes(), 1U);
        EXPECT_EQ(victimTrace.destructions, 1U);
        EXPECT_TRUE(victimTrace.destroyedOnControl);
    }

    TEST(NodeRemoval, FailedReplacementWhilePendingPreservesAllReferencingGenerations) {
        RemovalTrace trace;
        DAG graph;
        const auto id = graph.add<RemovalNode>(trace);
        ASSERT_TRUE(id);
        ASSERT_TRUE(graph.markAsOutput(*id));
        auto active = graph.compile(callbackConfig);
        ASSERT_TRUE(active);
        // These owners model control-side Ready/Pending/Retired generations.
        // Publication machinery belongs to S07; every owner retains its borrow.
        std::optional<ExecutionPlan> ready{*active};
        std::optional<ExecutionPlan> pending{*active};
        std::optional<ExecutionPlan> retired{*active};
        ASSERT_TRUE(graph.remove(*id));
        const BackendConfig impossible{.outputChannels = std::numeric_limits<std::size_t>::max()};
        auto failedReplacement = graph.compile(impossible);
        ASSERT_FALSE(failedReplacement);
        EXPECT_EQ(failedReplacement.error(), GraphError::PreparationCapacityExceeded);
        EXPECT_EQ(graph.collectRetiredNodes(), 0U);
        EXPECT_EQ(trace.destructions, 0U);
        const float input = 5;
        float output = 99;
        const std::array inputs{&input};
        const std::array outputs{&output};
        ASSERT_TRUE(active->exec(BackendBlock{inputs, outputs, 1}));
        EXPECT_EQ(output, 6);
        auto replacement = graph.compile(callbackConfig);
        ASSERT_TRUE(replacement);
        EXPECT_TRUE(replacement->tasks().empty());
        pending.reset();
        EXPECT_EQ(graph.collectRetiredNodes(), 0U);
        // Control applies the replacement only after the old callback quiesces.
        active = std::move(replacement);
        ASSERT_TRUE(active->exec(BackendBlock{inputs, outputs, 1}));
        EXPECT_EQ(output, 0);
        ASSERT_TRUE(ready->exec(BackendBlock{inputs, outputs, 1}));
        EXPECT_EQ(output, 6);
        ready.reset();
        EXPECT_EQ(graph.collectRetiredNodes(), 0U);
        retired.reset();
        EXPECT_EQ(trace.destructions, 0U); // Releasing a token does not own/delete Node.
        EXPECT_EQ(graph.collectRetiredNodes(), 1U);
        EXPECT_EQ(trace.destructions, 1U);
        EXPECT_TRUE(trace.destroyedOnControl);
    }

    TEST(NodeRemoval, CallbackAcknowledgmentDoesNotReleaseOtherGenerationsOrDestroyOnAudio) {
        RemovalTrace trace;
        DAG graph;
        const auto id = graph.add<RemovalNode>(trace);
        ASSERT_TRUE(id);
        ASSERT_TRUE(graph.markAsOutput(*id));
        auto active = graph.compile(callbackConfig);
        ASSERT_TRUE(active);
        std::optional<ExecutionPlan> pending{*active};
        std::optional<ExecutionPlan> retired{*active};
        ExecutionPlan* const borrowedPlan = &*active; // Owner stays on control through ACK.
        const float input = 5;
        float output = 99;
        const std::array inputs{&input};
        const std::array outputs{&output};
        std::latch callbackReady{1};
        std::latch runCallback{1};
        std::atomic<bool> acknowledged{false};
        bool accepted = false;
        std::thread audio{[&] {
            // Synchronization belongs to the test harness, never Node::process.
            callbackReady.count_down();
            runCallback.wait();
            accepted = borrowedPlan->exec(BackendBlock{inputs, outputs, 1});
            acknowledged.store(true, std::memory_order_release);
        }};
        callbackReady.wait();
        const auto removed = graph.remove(*id);
        const auto collectedWhileActive = graph.collectRetiredNodes();
        // Always release/join the worker before fatal assertions or owner edits.
        runCallback.count_down();
        audio.join();
        ASSERT_TRUE(removed);
        EXPECT_EQ(collectedWhileActive, 0U);
        EXPECT_TRUE(acknowledged.load(std::memory_order_acquire));
        EXPECT_TRUE(accepted);
        EXPECT_EQ(output, 6);
        EXPECT_EQ(trace.calls, 1U);
        EXPECT_EQ(trace.destructions, 0U);
        auto replacement = graph.compile(callbackConfig);
        ASSERT_TRUE(replacement);
        EXPECT_TRUE(replacement->tasks().empty());
        active = std::move(replacement); // Install B after ACK; retire only A's borrow.
        EXPECT_EQ(graph.collectRetiredNodes(), 0U);
        pending.reset();
        EXPECT_EQ(graph.collectRetiredNodes(), 0U);
        retired.reset();
        EXPECT_EQ(trace.destructions, 0U);
        EXPECT_EQ(graph.collectRetiredNodes(), 1U);
        EXPECT_EQ(trace.destructions, 1U);
        EXPECT_TRUE(trace.destroyedOnControl);
        EXPECT_EQ(graph.collectRetiredNodes(), 0U);
    }
}
