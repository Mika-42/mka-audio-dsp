/**
 * @file test_backend_io.cpp
 * @brief Tests for backend io.
 * @author Mika
 */
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

import mka.audio.dsp.dag;

namespace {
    using namespace mka::audio::dsp;

    /// @brief Expected physical addresses and binding roles for a synchronous callback.
    struct PointerOracle {
        /// @brief Input channel descriptors or physical sample pointers used by this state.
        std::span<const float* const> inputs;
        /// @brief Output channel descriptors or physical sample pointers used by this state.
        std::span<float* const> outputs;
        /// @brief Identity of the Node expected to receive physical output descriptors.
        NodeID outputNode = 0;
        /// @brief Identity of the source expected to receive physical input descriptors.
        NodeID inputNode = 0;
        /// @brief Address of the retained logical output pointer inspected by the oracle.
        float** logicalOutput = nullptr;
        /// @brief Address of the retained read-only input pointer inspected by the oracle.
        const float** logicalInput = nullptr;
    };
    /// @brief Borrowed pointer oracle installed for one synchronous callback.
    thread_local const PointerOracle* oracle = nullptr;
    /// @brief Temporarily installs the pointer oracle and restores the previous observer.
    struct OracleScope {
        /// @brief Observer restored when the scoped oracle leaves scope.
        const PointerOracle* previous;
        /// @brief Install the pointer oracle and save the previous synchronous observer.
        explicit OracleScope(const PointerOracle& current) : previous(std::exchange(oracle, &current)) {}
        /// @brief Restore the previously selected synchronous observation scope.
        ~OracleScope() { oracle = previous; }
    };

    /// @brief Selects the processing behavior used by the fixture.
    enum class Mode {
        /// @brief Forward the selected input samples without gain.
        Copy,
        /// @brief Produce the configured constant sample value.
        Constant,
        /// @brief Select the auxiliary fixture case.
        Auxiliary,
        /// @brief Select the no write fixture case.
        NoWrite
    };
    /// @brief Configurable fixture DSP used to observe routing, latency and lifetime behavior.
    class Processor : public Node {
    public:
        /// @brief Select the processor role and initialize its logical layouts and observation state.
        explicit Processor(const NodeID id)
            : Processor(id, Config{.layoutInput = {0}, .layoutAuxiliary = {0}, .layoutOutput = {0}}) {}
        /// @brief Observe context forwarding and render the selected copy, gain or constant routing policy.
        void process(const ProcessContext& context) noexcept override {
            ++calls;
            inputCount = context.inputs.size();
            auxiliaryCount = context.auxiliary.size();
            logicalOutputCount = context.outputs.size();
            backendOutputCount = context.backendOutputs.size();
            inputChannel = context.inputs.empty() ? 0 : context.inputs[0].channel;
            outputChannel = context.outputs.empty() ? 0 : context.outputs[0].channel;
            physicalIDsZero = true;
            viewsDistinct = true;
            physicalInitiallyZero = true;
            for (const auto& output : context.backendOutputs) {
                physicalIDsZero = physicalIDsZero && output.channel == 0;
                for (const auto& logical : context.outputs)
                    viewsDistinct = viewsDistinct && logical.buffer != output.buffer;
                for (FrameCount frame = 0; frame < context.frames; ++frame)
                    physicalInitiallyZero = physicalInitiallyZero && output.buffer[frame] == 0.F;
            }
            if (trace != nullptr) trace[(*traceSize)++] = id();
            if (oracle != nullptr) {
                directViews = context.inputs.size() == oracle->inputs.size() &&
                              context.backendOutputs.size() == oracle->outputs.size();
                if (directViews) {
                    for (std::size_t i = 0; i < context.inputs.size(); ++i)
                        directViews = directViews && context.inputs[i].buffer == oracle->inputs[i] &&
                                      context.inputs[i].channel == 0;
                    for (std::size_t i = 0; i < context.backendOutputs.size(); ++i)
                        directViews = directViews && context.backendOutputs[i].buffer == oracle->outputs[i];
                }
                // Test-only observer, alive for this synchronous invocation.
                // The Node itself retains no descriptor/sample pointers.
                if (id() == oracle->outputNode && oracle->logicalOutput && !context.outputs.empty())
                    *oracle->logicalOutput = context.outputs[0].buffer;
                if (id() == oracle->inputNode && oracle->logicalInput && !context.inputs.empty())
                    *oracle->logicalInput = context.inputs[0].buffer;
            }
            if (mode == Mode::NoWrite) return;
            for (FrameCount frame = 0; frame < context.frames; ++frame) {
                if (alternateFrames && frame % 2 != 0) continue;
                float sample = value;
                if (mode == Mode::Copy) {
                    sample = inputLane < context.inputs.size() ? context.inputs[inputLane].buffer[frame] : 0.F;
                    if (secondInputLane < context.inputs.size())
                        sample += context.inputs[secondInputLane].buffer[frame];
                }
                if (mode == Mode::Auxiliary)
                    sample = context.auxiliary.empty() ? 0.F : context.auxiliary[0].buffer[frame];
                sample *= gain;
                if (writeLogical && !context.outputs.empty()) context.outputs[0].buffer[frame] = sample;
                if (outputLane < context.backendOutputs.size())
                    context.backendOutputs[outputLane].buffer[frame] = sample + backendBias;
                if (secondOutputLane < context.backendOutputs.size())
                    context.backendOutputs[secondOutputLane].buffer[frame] = -sample;
            }
        }

        /// @brief Control-selected fixture behavior for the next processing invocation.
        Mode mode = Mode::Copy;
        /// @brief Configured scalar signal value or expected value for this fixture case.
        float value = 1.F;
        /// @brief Scalar multiplication factor configured for this fixture path.
        float gain = 1.F;
        /// @brief Offset distinguishing physical writes from the logical output signal.
        float backendBias = 0.F;
        /// @brief Physical input lane selected by a source-role fixture processor.
        std::size_t inputLane = 0;
        /// @brief Physical output lane selected by the fixture writer.
        std::size_t outputLane = 0;
        /// @brief Optional additional physical input lane; the maximum value disables it.
        std::size_t secondInputLane = std::numeric_limits<std::size_t>::max();
        /// @brief Optional additional physical output lane; the maximum value disables it.
        std::size_t secondOutputLane = std::numeric_limits<std::size_t>::max();
        /// @brief Whether physical writes select only alternating sample positions.
        bool alternateFrames = false;
        /// @brief Whether the fixture writes its logical output in addition to physical samples.
        bool writeLogical = true;
        /// @brief Number of processor invocations observed by the fixture.
        unsigned calls = 0;
        std::size_t inputCount = 0 /**< Number of input descriptors observed during the callback. */,
             auxiliaryCount = 0 /**< Auxiliary count retained by the fixture. */,
             logicalOutputCount = 0 /**< Logical output count retained by the fixture. */,
             backendOutputCount = 0 /**< Number of physical output views observed by the context fixture. */;
        ChannelID inputChannel = 0 /**< Input channel retained by the fixture. */,
             outputChannel = 0 /**< Output channel retained by the fixture. */;
        bool directViews = false /**< Direct views retained by the fixture. */,
             viewsDistinct = false /**< Views distinct retained by the fixture. */,
             physicalIDsZero = false /**< Whether every physical descriptor uses the required zero logical identity. */,
             physicalInitiallyZero = false /**< Physical initially zero retained by the fixture. */;
        /// @brief Externally observed lifetime, order or context data used by this fixture.
        NodeID* trace = nullptr; // Test event buffer, never a backend sample pointer.
        /// @brief Borrowed insertion cursor for the processing-order trace.
        std::size_t* traceSize = nullptr;
    protected:
        /// @brief Select the processor role and initialize its logical layouts and observation state.
        Processor(const NodeID id, const Config& config) : Node(id, config) {}
    };
    /// @brief Provides a structural graph node without reading or writing samples.
    class EmptyProcessor final : public Processor {
    public:
        /// @brief Declare the processor logical ports under the supplied stable Node identity.
        explicit EmptyProcessor(const NodeID id) : Processor(id, Config{}) {}
    };
    /// @brief Exercises logical layouts whose channel numbers are sparse.
    class SparseProcessor final : public Processor {
    public:
        /// @brief Declare the processor logical ports under the supplied stable Node identity.
        explicit SparseProcessor(const NodeID id)
            : Processor(id, Config{.layoutInput = {65535}, .layoutAuxiliary = {2}, .layoutOutput = {42}}) {}
    };

    /// @brief Require the expected numeric error while preserving its node/channel context.
    void expectError(const Result& result, const ErrorType error) {
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, error.code);
    }

    /// @brief Test case: marking rejects missing nodes and is idempotent.
    TEST(BackendRoles, MarkingRejectsMissingNodesAndIsIdempotent) {
        DAG graph;
        const auto addedWriter = graph.add<EmptyProcessor>();
        ASSERT_TRUE(addedWriter);
        auto& writer = *static_cast<EmptyProcessor*>(graph.node(*addedWriter));
        writer.mode = Mode::Constant;
        writer.value = 5.F;
        expectError(graph.markAsOutput(invalidNodeID), ErrorType::NodeNotFound);
        expectError(graph.markAsOutput(maxNodeID), ErrorType::NodeNotFound);
        ASSERT_TRUE(graph.markAsOutput(writer.id()));
        ASSERT_TRUE(graph.markAsOutput(writer.id()));
        auto plan = graph.compile(BackendConfig{.maxFrames = 2, .outputChannels = 1});
        ASSERT_TRUE(plan);
        std::array<float, 2> samples{99.F, 99.F};
        const std::array outputs{samples.data()};
        ASSERT_TRUE(plan->exec(BackendBlock{.outputs = outputs, .frames = 2}));
        EXPECT_EQ(writer.calls, 1U);
        EXPECT_EQ(samples, (std::array<float, 2>{5.F, 5.F}));
    }

    /// @brief Test case: removing A node clears its output marker before the next compilation.
    TEST(BackendRoles, RemovingANodeClearsItsOutputMarkerBeforeTheNextCompilation) {
        DAG graph;
        const auto addedRemoved = graph.add<EmptyProcessor>();
        ASSERT_TRUE(addedRemoved);
        const auto removed = *addedRemoved;
        ASSERT_TRUE(graph.markAsOutput(removed));
        ASSERT_TRUE(graph.remove(removed));
        expectError(graph.markAsOutput(removed), ErrorType::NodeNotFound);
        const auto addedUnmarked = graph.add<EmptyProcessor>();
        ASSERT_TRUE(addedUnmarked);
        auto& unmarked = *static_cast<EmptyProcessor*>(graph.node(*addedUnmarked));
        unmarked.mode = Mode::Constant;
        unmarked.value = 9;
        auto plan = graph.compile(BackendConfig{.maxFrames = 1, .outputChannels = 1});
        ASSERT_TRUE(plan); // A stale marker would refer to a node absent from the plan.
        float output = 99;
        const std::array outputs{&output};
        ASSERT_TRUE(plan->exec(BackendBlock{.outputs = outputs, .frames = 1}));
        EXPECT_EQ(unmarked.backendOutputCount, 0U);
        EXPECT_EQ(output, 0);
    }

    /// @brief Test case: empty layout source and output sees all six physical lanes directly.
    TEST(BackendRoles, EmptyLayoutSourceAndOutputSeesAllSixPhysicalLanesDirectly) {
        DAG graph;
        const auto addedNode = graph.add<EmptyProcessor>();
        ASSERT_TRUE(addedNode);
        auto& node = *static_cast<EmptyProcessor*>(graph.node(*addedNode));
        node.inputLane = 5;
        node.outputLane = 5;
        node.secondInputLane = 0; // Explicit effect-defined sum, not a graph mix.
        node.secondOutputLane = 1;
        ASSERT_TRUE(node.config().layoutInput.empty());
        ASSERT_TRUE(graph.markAsOutput(node.id()));
        auto plan = graph.compile(BackendConfig{.maxFrames = 3, .inputChannels = 6, .outputChannels = 6});
        ASSERT_TRUE(plan);
        const std::array<std::array<float, 3>, 6> inputs{{{1, 2, 3}, {4, 5, 6}, {7, 8, 9},
                                                       {10, 11, 12}, {13, 14, 15}, {16, 17, 18}}};
        std::array<std::array<float, 3>, 6> outputs;
        for (auto& lane : outputs) lane.fill(99.F);
        const std::array<const float*, 6> inputPointers{inputs[0].data(), inputs[1].data(), inputs[2].data(),
            inputs[3].data(), inputs[4].data(), inputs[5].data()};
        const std::array outputPointers{outputs[0].data(), outputs[1].data(), outputs[2].data(),
            outputs[3].data(), outputs[4].data(), outputs[5].data()};
        const PointerOracle expected{.inputs = inputPointers, .outputs = outputPointers};
        {
            const OracleScope scope{expected};
            ASSERT_TRUE(plan->exec(BackendBlock{inputPointers, outputPointers, 3}));
        }
        EXPECT_TRUE(node.directViews);
        EXPECT_TRUE(node.physicalIDsZero);
        EXPECT_TRUE(node.physicalInitiallyZero);
        EXPECT_EQ(node.inputCount, 6U);
        EXPECT_EQ(node.backendOutputCount, 6U);
        EXPECT_EQ(node.logicalOutputCount, 0U);
        for (const auto lane : std::array<std::size_t, 4>{0, 2, 3, 4})
            EXPECT_EQ(outputs[lane], (std::array<float, 3>{}));
        EXPECT_EQ(outputs[5], (std::array<float, 3>{17, 19, 21}));
        EXPECT_EQ(outputs[1], (std::array<float, 3>{-17, -19, -21}));
        EXPECT_EQ(inputs[0], (std::array<float, 3>{1, 2, 3}));
    }

    /// @brief Test case: an unmarked source cannot write the backend.
    TEST(BackendRoles, AnUnmarkedSourceCannotWriteTheBackend) {
        DAG graph;
        const auto addedNode = graph.add<Processor>();
        ASSERT_TRUE(addedNode);
        auto& node = *static_cast<Processor*>(graph.node(*addedNode));
        auto plan = graph.compile(BackendConfig{.maxFrames = 2, .inputChannels = 2, .outputChannels = 1});
        ASSERT_TRUE(plan);
        const std::array<float, 2> input{3, 5};
        const std::array<const float*, 2> inputs{input.data(), input.data()};
        std::array<float, 2> output{9, 9};
        const std::array outputs{output.data()};
        ASSERT_TRUE(plan->exec(BackendBlock{inputs, outputs, 2}));
        EXPECT_EQ(node.inputCount, 2U); // Not filtered to its one-element layout.
        EXPECT_EQ(node.backendOutputCount, 0U);
        EXPECT_EQ(node.logicalOutputCount, 1U);
        EXPECT_EQ(output, (std::array<float, 2>{}));
    }

    /// @brief Test case: marked non source executes after its producer even with lower node ID.
    TEST(BackendRouting, MarkedNonSourceExecutesAfterItsProducerEvenWithLowerNodeID) {
        DAG graph;
        const auto addedSink = graph.add<Processor>();
        ASSERT_TRUE(addedSink);
        auto& sink = *static_cast<Processor*>(graph.node(*addedSink));
        const auto addedSource = graph.add<Processor>();
        ASSERT_TRUE(addedSource);
        auto& source = *static_cast<Processor*>(graph.node(*addedSource));
        source.gain = 2;
        sink.gain = 3;
        ASSERT_TRUE(graph.connect(sink.channel(PortType::In, 0), source.channel(PortType::Out, 0)));
        ASSERT_TRUE(graph.markAsOutput(sink.id()));
        auto plan = graph.compile(BackendConfig{.maxFrames = 3, .inputChannels = 1, .outputChannels = 2});
        ASSERT_TRUE(plan);
        ASSERT_EQ(plan->tasks().size(), 2U);
        EXPECT_EQ(plan->tasks()[0], &source);
        EXPECT_EQ(plan->tasks()[1], &sink);
        const std::array<float, 3> input{3, 4, 5};
        const std::array<const float*, 1> inputs{input.data()};
        std::array<float, 3> first{}, second{99, 99, 99};
        const std::array outputs{first.data(), second.data()};
        ASSERT_TRUE(plan->exec(BackendBlock{inputs, outputs, 3}));
        EXPECT_EQ(first, (std::array<float, 3>{18, 24, 30}));
        EXPECT_EQ(second, (std::array<float, 3>{}));
        EXPECT_EQ(source.backendOutputCount, 0U);
        EXPECT_EQ(sink.inputCount, 1U);
        EXPECT_EQ(sink.backendOutputCount, 2U);
    }

    /// @brief Test case: marked source retains distinct logical outputs for its successor.
    TEST(BackendRouting, MarkedSourceRetainsDistinctLogicalOutputsForItsSuccessor) {
        DAG graph;
        const auto addedSink = graph.add<Processor>();
        ASSERT_TRUE(addedSink);
        auto& sink = *static_cast<Processor*>(graph.node(*addedSink));
        const auto addedSource = graph.add<Processor>();
        ASSERT_TRUE(addedSource);
        auto& source = *static_cast<Processor*>(graph.node(*addedSource));
        source.gain = 2;
        source.backendBias = 100;
        sink.gain = 3;
        ASSERT_TRUE(graph.connect(sink.channel(PortType::In, 0), source.channel(PortType::Out, 0)));
        ASSERT_TRUE(graph.markAsOutput(source.id()));
        ASSERT_TRUE(graph.markAsOutput(sink.id()));
        auto plan = graph.compile(BackendConfig{.maxFrames = 2, .inputChannels = 1, .outputChannels = 1});
        ASSERT_TRUE(plan);
        const std::array<float, 2> input{3, 4};
        const std::array<const float*, 1> inputs{input.data()};
        std::array<float, 2> output{};
        const std::array outputs{output.data()};
        ASSERT_TRUE(plan->exec(BackendBlock{inputs, outputs, 2}));
        EXPECT_TRUE(source.viewsDistinct);
        EXPECT_TRUE(sink.viewsDistinct);
        EXPECT_EQ(source.inputCount, 1U);
        EXPECT_EQ(source.backendOutputCount, 1U);
        EXPECT_EQ(sink.backendOutputCount, 1U);
        EXPECT_EQ(output, (std::array<float, 2>{18, 24})); // Reads 6/8, never 106/108.
    }

    /// @brief Test case: same lane uses the last effective write without mixing or intermediate clears.
    TEST(BackendRouting, SameLaneUsesTheLastEffectiveWriteWithoutMixingOrIntermediateClears) {
        DAG graph;
        const auto addedFirst = graph.add<EmptyProcessor>();
        ASSERT_TRUE(addedFirst);
        auto& first = *static_cast<EmptyProcessor*>(graph.node(*addedFirst));
        const auto addedSecond = graph.add<EmptyProcessor>();
        ASSERT_TRUE(addedSecond);
        auto& second = *static_cast<EmptyProcessor*>(graph.node(*addedSecond));
        first.mode = second.mode = Mode::Constant;
        first.value = 2;
        second.value = 7;
        second.alternateFrames = true;
        std::array<NodeID, 2> trace{};
        std::size_t traceSize = 0;
        first.trace = second.trace = trace.data();
        first.traceSize = second.traceSize = &traceSize;
        ASSERT_TRUE(graph.markAsOutput(second.id())); // Mark order must not affect execution.
        ASSERT_TRUE(graph.markAsOutput(first.id()));
        auto plan = graph.compile(BackendConfig{.maxFrames = 4, .outputChannels = 2});
        ASSERT_TRUE(plan);
        std::array<float, 4> output{99, 99, 99, 99}, untouched{99, 99, 99, 99};
        const std::array outputs{output.data(), untouched.data()};
        ASSERT_TRUE(plan->exec(BackendBlock{.outputs = outputs, .frames = 4}));
        EXPECT_EQ(output, (std::array<float, 4>{7, 2, 7, 2}));
        EXPECT_EQ(untouched, (std::array<float, 4>{}));
        EXPECT_TRUE(first.physicalInitiallyZero);
        EXPECT_FALSE(second.physicalInitiallyZero);
        EXPECT_EQ(traceSize, 2U);
        EXPECT_EQ(trace, (std::array<NodeID, 2>{first.id(), second.id()}));
    }

    /// @brief Test case: empty graph still clears all physical outputs for actual frames only.
    TEST(BackendRouting, EmptyGraphStillClearsAllPhysicalOutputsForActualFramesOnly) {
        DAG graph;
        auto plan = graph.compile(BackendConfig{.maxFrames = 4, .outputChannels = 2});
        ASSERT_TRUE(plan);
        std::array<float, 3> first{9, 9, 42}, second{8, 8, 43};
        const std::array outputs{first.data(), second.data()};
        ASSERT_TRUE(plan->exec(BackendBlock{.outputs = outputs, .frames = 2}));
        EXPECT_EQ(first, (std::array<float, 3>{0, 0, 42}));
        EXPECT_EQ(second, (std::array<float, 3>{0, 0, 43}));
    }

    /// @brief Test case: every callback uses its current addresses and channel counts.
    TEST(BackendLifetime, EveryCallbackUsesItsCurrentAddressesAndChannelCounts) {
        DAG graph;
        const auto addedNode = graph.add<EmptyProcessor>();
        ASSERT_TRUE(addedNode);
        auto& node = *static_cast<EmptyProcessor*>(graph.node(*addedNode));
        ASSERT_TRUE(graph.markAsOutput(node.id()));
        auto plan = graph.compile(BackendConfig{.maxFrames = 2, .inputChannels = 1, .outputChannels = 1});
        ASSERT_TRUE(plan);
        const std::array<float, 2> firstInput{1, 2};
        std::array<float, 2> firstOutput{};
        {
            const std::array<const float*, 1> inputs{firstInput.data()};
            const std::array outputs{firstOutput.data()};
            const PointerOracle expected{inputs, outputs};
            const OracleScope scope{expected};
            ASSERT_TRUE(plan->exec(BackendBlock{inputs, outputs, 2}));
            EXPECT_TRUE(node.directViews);
        }
        const auto previous = firstOutput;
        ASSERT_TRUE(plan->prepareBackend(BackendConfig{.maxFrames=2,.inputChannels=2,.outputChannels=1},
            graph.connections(),graph.outputNodes()));
        {
            const std::array<float, 2> secondInput{7, 11};
            std::array<float, 2> secondOutput{};
            const std::array<const float*, 2> inputs{secondInput.data(), secondInput.data()};
            const std::array outputs{secondOutput.data()};
            const PointerOracle expected{inputs, outputs};
            const OracleScope scope{expected};
            ASSERT_TRUE(plan->exec(BackendBlock{inputs, outputs, 2}));
            EXPECT_TRUE(node.directViews);
            EXPECT_EQ(node.inputCount, 2U);
            EXPECT_EQ(node.backendOutputCount, 1U);
            EXPECT_EQ(secondOutput, secondInput);
            EXPECT_EQ(firstOutput, previous);
        }
        ASSERT_TRUE(plan->prepareBackend(BackendConfig{.maxFrames=2},graph.connections(),graph.outputNodes()));
        ASSERT_TRUE(plan->exec(BackendBlock{.frames = 2}));
        EXPECT_EQ(node.inputCount, 0U);
        EXPECT_EQ(node.backendOutputCount, 0U);
        EXPECT_EQ(firstOutput, previous);
    }

    /// @brief Test case: rejected blocks silence their known capacity and the next valid callback recovers.
    TEST(BackendGuards, RejectedBlocksSilenceTheirKnownCapacityAndTheNextValidCallbackRecovers) {
        DAG graph;
        const auto addedNode = graph.add<EmptyProcessor>();
        ASSERT_TRUE(addedNode);
        auto& node = *static_cast<EmptyProcessor*>(graph.node(*addedNode));
        ASSERT_TRUE(graph.markAsOutput(node.id()));
        auto plan = graph.compile(BackendConfig{.maxFrames = 2, .inputChannels = 1, .outputChannels = 1});
        ASSERT_TRUE(plan);
        const std::array<float, 3> input{3, 5, 7};
        std::array<float, 3> output{41, 43, 47};
        const std::array<const float*, 1> inputs{input.data()};
        const std::array outputs{output.data()};
        EXPECT_FALSE(plan->exec(BackendBlock{inputs, outputs, 3}));
        const std::array<const float*, 2> tooManyInputs{input.data(), input.data()};
        EXPECT_FALSE(plan->exec(BackendBlock{tooManyInputs, outputs, 1}));
        const std::array tooManyOutputs{output.data(), output.data()};
        EXPECT_FALSE(plan->exec(BackendBlock{inputs, tooManyOutputs, 1}));
        const std::array<const float*, 1> nullInputs{nullptr};
        const std::array<float*, 1> nullOutputs{nullptr};
        EXPECT_FALSE(plan->exec(BackendBlock{nullInputs, outputs, 1}));
        EXPECT_FALSE(plan->exec(BackendBlock{inputs, nullOutputs, 1}));
        EXPECT_TRUE(plan->exec(BackendBlock{nullInputs, nullOutputs, 0}));
        EXPECT_EQ(node.calls, 1U);
        EXPECT_EQ(output, (std::array<float, 3>{0, 0, 47}));
        ASSERT_TRUE(plan->exec(BackendBlock{inputs, outputs, 2}));
        EXPECT_EQ(node.calls, 2U);
        EXPECT_EQ(output, (std::array<float, 3>{3, 5, 47}));
    }

    /// @brief Test case: structural plans reject backend execution until prepared.
    TEST(BackendPreparation, StructuralPlansRejectBackendExecutionUntilPrepared) {
        DAG graph;
        const auto addedNode = graph.add<EmptyProcessor>();
        ASSERT_TRUE(addedNode);
        auto& node = *static_cast<EmptyProcessor*>(graph.node(*addedNode));
        auto plan = graph.compile();
        ASSERT_TRUE(plan);
        EXPECT_FALSE(plan->isBackendPrepared());
        EXPECT_FALSE(plan->exec(BackendBlock{}));
        EXPECT_EQ(node.calls, 0U);
    }

    /// @brief Test case: zero maximum frames calls tasks without accessing samples.
    TEST(BackendPreparation, ZeroMaximumFramesCallsTasksWithoutAccessingSamples) {
        DAG graph;
        const auto addedNode = graph.add<Processor>();
        ASSERT_TRUE(addedNode);
        auto& node = *static_cast<Processor*>(graph.node(*addedNode));
        ASSERT_TRUE(graph.markAsOutput(node.id()));
        auto plan = graph.compile(BackendConfig{.inputChannels = 1, .outputChannels = 1});
        ASSERT_TRUE(plan);
        const std::array<const float*, 1> inputs{nullptr};
        const std::array<float*, 1> outputs{nullptr};
        EXPECT_TRUE(plan->exec(BackendBlock{inputs, outputs, 0}));
        EXPECT_FALSE(plan->exec(BackendBlock{inputs, outputs, 1}));
        EXPECT_EQ(node.calls, 1U);
    }

    /// @brief Test case: markers are snapshotted without changing existing plans.
    TEST(BackendRoles, MarkersAreSnapshottedWithoutChangingExistingPlans) {
        DAG graph;
        const auto addedNode = graph.add<EmptyProcessor>();
        ASSERT_TRUE(addedNode);
        auto& node = *static_cast<EmptyProcessor*>(graph.node(*addedNode));
        node.mode = Mode::Constant;
        node.value = 8;
        auto oldPlan = graph.compile(BackendConfig{.maxFrames = 1, .outputChannels = 1});
        ASSERT_TRUE(oldPlan);
        ASSERT_TRUE(graph.markAsOutput(node.id()));
        auto newPlan = graph.compile(BackendConfig{.maxFrames = 1, .outputChannels = 1});
        ASSERT_TRUE(newPlan);
        std::array<float, 1> output{99};
        const std::array outputs{output.data()};
        ASSERT_TRUE(newPlan->exec(BackendBlock{.outputs = outputs, .frames = 1}));
        EXPECT_EQ(output[0], 8);
        ASSERT_TRUE(oldPlan->exec(BackendBlock{.outputs = outputs, .frames = 1}));
        EXPECT_EQ(node.backendOutputCount, 0U);
        EXPECT_EQ(output[0], 0);
    }

    /// @brief Test case: any incoming aux edge prevents source status even outside the layout.
    TEST(BackendRouting, AnyIncomingAuxEdgePreventsSourceStatusEvenOutsideTheLayout) {
        DAG graph;
        const auto addedSource = graph.add<EmptyProcessor>();
        ASSERT_TRUE(addedSource);
        auto& source = *static_cast<EmptyProcessor*>(graph.node(*addedSource));
        const auto addedSink = graph.add<EmptyProcessor>();
        ASSERT_TRUE(addedSink);
        auto& sink = *static_cast<EmptyProcessor*>(graph.node(*addedSink));
        source.mode = Mode::NoWrite;
        sink.mode = Mode::Constant;
        sink.value = 7;
        ASSERT_TRUE(graph.connect(sink.channel(PortType::Aux, 65535), source.channel(PortType::Out, 65535)));
        ASSERT_TRUE(graph.markAsOutput(sink.id()));
        auto plan = graph.compile(BackendConfig{.maxFrames = 1, .inputChannels = 2, .outputChannels = 1});
        ASSERT_TRUE(plan);
        const float input = 9;
        const std::array<const float*, 2> inputs{&input, &input};
        float output = 0;
        const std::array outputs{&output};
        ASSERT_TRUE(plan->exec(BackendBlock{inputs, outputs, 1}));
        EXPECT_EQ(source.inputCount, 2U);
        EXPECT_EQ(sink.inputCount, 0U);
        EXPECT_EQ(output, 7);
    }

    /// @brief Test case: unproduced logical channels read silence instead of physical inputs.
    TEST(BackendRouting, UnproducedLogicalChannelsReadSilenceInsteadOfPhysicalInputs) {
        DAG graph;
        const auto addedSource = graph.add<EmptyProcessor>();
        ASSERT_TRUE(addedSource);
        auto& source = *static_cast<EmptyProcessor*>(graph.node(*addedSource)); // No declared logical outputs.
        const auto addedSink = graph.add<Processor>();
        ASSERT_TRUE(addedSink);
        auto& sink = *static_cast<Processor*>(graph.node(*addedSink));
        ASSERT_TRUE(graph.connect(sink.channel(PortType::In, 0), source.channel(PortType::Out, 65535)));
        ASSERT_TRUE(graph.markAsOutput(sink.id()));
        auto plan = graph.compile(BackendConfig{.maxFrames = 1, .inputChannels = 1, .outputChannels = 1});
        ASSERT_TRUE(plan);
        const float input = 9;
        const std::array<const float*, 1> inputs{&input};
        float output = 99;
        const std::array outputs{&output};
        ASSERT_TRUE(plan->exec(BackendBlock{inputs, outputs, 1}));
        EXPECT_EQ(output, 0);
        sink.mode = Mode::Auxiliary; // Unconnected declared Aux also reads silence.
        ASSERT_TRUE(plan->exec(BackendBlock{inputs, outputs, 1}));
        EXPECT_EQ(output, 0);
    }

    /// @brief Test case: unwritten internal outputs are reset for each callback.
    TEST(BackendRouting, UnwrittenInternalOutputsAreResetForEachCallback) {
        DAG graph;
        const auto addedSource = graph.add<Processor>();
        ASSERT_TRUE(addedSource);
        auto& source = *static_cast<Processor*>(graph.node(*addedSource));
        const auto addedSink = graph.add<Processor>();
        ASSERT_TRUE(addedSink);
        auto& sink = *static_cast<Processor*>(graph.node(*addedSink));
        source.mode = Mode::Constant;
        source.value = 3;
        ASSERT_TRUE(graph.connect(sink.channel(PortType::In, 0), source.channel(PortType::Out, 0)));
        ASSERT_TRUE(graph.markAsOutput(sink.id()));
        auto plan = graph.compile(BackendConfig{.maxFrames = 1, .outputChannels = 1});
        ASSERT_TRUE(plan);
        float output = 99;
        const std::array outputs{&output};
        ASSERT_TRUE(plan->exec(BackendBlock{.outputs = outputs, .frames = 1}));
        EXPECT_EQ(output, 3);
        source.mode = Mode::NoWrite; // Safe control edit between quiescent callbacks.
        ASSERT_TRUE(plan->exec(BackendBlock{.outputs = outputs, .frames = 1}));
        EXPECT_EQ(output, 0);
    }

    /// @brief Test case: sparse logical indices bind by identity instead of array position.
    TEST(BackendRouting, SparseLogicalIndicesBindByIdentityInsteadOfArrayPosition) {
        DAG graph;
        const auto addedSource = graph.add<SparseProcessor>();
        ASSERT_TRUE(addedSource);
        auto& source = *static_cast<SparseProcessor*>(graph.node(*addedSource));
        const auto addedSink = graph.add<SparseProcessor>();
        ASSERT_TRUE(addedSink);
        auto& sink = *static_cast<SparseProcessor*>(graph.node(*addedSink));
        source.gain = 2;
        sink.gain = 5;
        ASSERT_TRUE(graph.connect(sink.channel(PortType::In, 65535), source.channel(PortType::Out, 42)));
        ASSERT_TRUE(graph.markAsOutput(sink.id()));
        auto plan = graph.compile(BackendConfig{.maxFrames = 1, .inputChannels = 1, .outputChannels = 1});
        ASSERT_TRUE(plan);
        const float input = 4;
        const std::array<const float*, 1> inputs{&input};
        float output = 99;
        const std::array outputs{&output};
        ASSERT_TRUE(plan->exec(BackendBlock{inputs, outputs, 1}));
        EXPECT_EQ(output, 40);
        EXPECT_EQ(sink.inputCount, 1U);
        EXPECT_EQ(sink.inputChannel, sink.channel(PortType::In, 65535));
        EXPECT_EQ(source.outputChannel, source.channel(PortType::Out, 42));
    }

    /// @brief Test case: connected auxiliary receives logical samples and makes A non source.
    TEST(BackendRouting, ConnectedAuxiliaryReceivesLogicalSamplesAndMakesANonSource) {
        DAG graph;
        const auto addedSource = graph.add<SparseProcessor>();
        ASSERT_TRUE(addedSource);
        auto& source = *static_cast<SparseProcessor*>(graph.node(*addedSource));
        const auto addedSink = graph.add<SparseProcessor>();
        ASSERT_TRUE(addedSink);
        auto& sink = *static_cast<SparseProcessor*>(graph.node(*addedSink));
        source.mode = Mode::Constant;
        source.value = 5;
        sink.mode = Mode::Auxiliary;
        sink.gain = 3;
        ASSERT_TRUE(graph.connect(sink.channel(PortType::Aux, 2), source.channel(PortType::Out, 42)));
        ASSERT_TRUE(graph.markAsOutput(sink.id()));
        auto plan = graph.compile(BackendConfig{.maxFrames = 1, .inputChannels = 2, .outputChannels = 1});
        ASSERT_TRUE(plan);
        const float input = 99;
        const std::array<const float*, 2> inputs{&input, &input};
        float output = 0;
        const std::array outputs{&output};
        ASSERT_TRUE(plan->exec(BackendBlock{inputs, outputs, 1}));
        EXPECT_EQ(output, 15);
        EXPECT_EQ(sink.inputCount, 1U); // Unconnected logical In reads silence, not backend.
        EXPECT_EQ(sink.auxiliaryCount, 1U);
    }

    /// @brief Test case: logical fan out feeds independent writers without reserving physical lanes.
    TEST(BackendRouting, LogicalFanOutFeedsIndependentWritersWithoutReservingPhysicalLanes) {
        DAG graph;
        const auto addedSource = graph.add<Processor>();
        ASSERT_TRUE(addedSource);
        auto& source = *static_cast<Processor*>(graph.node(*addedSource));
        const auto addedFirst = graph.add<Processor>();
        ASSERT_TRUE(addedFirst);
        auto& first = *static_cast<Processor*>(graph.node(*addedFirst));
        const auto addedSecond = graph.add<Processor>();
        ASSERT_TRUE(addedSecond);
        auto& second = *static_cast<Processor*>(graph.node(*addedSecond));
        source.mode = Mode::Constant;
        source.value = 4;
        first.gain = 2;
        second.gain = 3;
        second.outputLane = 1;
        ASSERT_TRUE(graph.connect(first.channel(PortType::In, 0), source.channel(PortType::Out, 0)));
        ASSERT_TRUE(graph.connect(second.channel(PortType::In, 0), source.channel(PortType::Out, 0)));
        ASSERT_TRUE(graph.markAsOutput(first.id()));
        ASSERT_TRUE(graph.markAsOutput(second.id()));
        auto plan = graph.compile(BackendConfig{.maxFrames = 1, .outputChannels = 2});
        ASSERT_TRUE(plan);
        float left = 99, right = 99;
        const std::array outputs{&left, &right};
        ASSERT_TRUE(plan->exec(BackendBlock{.outputs = outputs, .frames = 1}));
        EXPECT_EQ(left, 8);
        EXPECT_EQ(right, 12);
    }

    /// @brief Test case: copies own independent logical storage and moves transfer preparation.
    TEST(BackendLifetime, CopiesOwnIndependentLogicalStorageAndMovesTransferPreparation) {
        DAG graph;
        const auto addedSource = graph.add<Processor>();
        ASSERT_TRUE(addedSource);
        auto& source = *static_cast<Processor*>(graph.node(*addedSource));
        const auto addedSink = graph.add<Processor>();
        ASSERT_TRUE(addedSink);
        auto& sink = *static_cast<Processor*>(graph.node(*addedSink));
        source.mode = Mode::Constant;
        source.value = 4;
        sink.gain = 3;
        ASSERT_TRUE(graph.connect(sink.channel(PortType::In, 0), source.channel(PortType::Out, 0)));
        ASSERT_TRUE(graph.markAsOutput(sink.id()));
        auto initial = graph.compile(BackendConfig{.maxFrames = 1, .outputChannels = 1});
        ASSERT_TRUE(initial);
        std::optional<ExecutionPlan> original{std::move(*initial)};
        initial = std::unexpected(ErrorType::NodeNotFound); // Discard moved-from shell.
        std::optional<ExecutionPlan> copy{*original};
        float result = 0;
        const std::array outputs{&result};
        float* originalOutput = nullptr;
        float* copiedOutput = nullptr;
        const float* copiedInput = nullptr;
        {
            const PointerOracle expected{.outputNode = source.id(), .logicalOutput = &originalOutput};
            const OracleScope scope{expected};
            ASSERT_TRUE(original->exec(BackendBlock{.outputs = outputs, .frames = 1}));
        }
        {
            const PointerOracle expected{.outputNode = source.id(), .inputNode = sink.id(),
                .logicalOutput = &copiedOutput, .logicalInput = &copiedInput};
            const OracleScope scope{expected};
            ASSERT_TRUE(copy->exec(BackendBlock{.outputs = outputs, .frames = 1}));
        }
        ASSERT_NE(originalOutput, nullptr);
        ASSERT_NE(copiedOutput, nullptr);
        ASSERT_NE(originalOutput, copiedOutput);
        EXPECT_EQ(copiedInput, copiedOutput);
        original.reset(); // The copied plan must not rely on the original samples.
        ASSERT_TRUE(copy->exec(BackendBlock{.outputs = outputs, .frames = 1}));
        EXPECT_EQ(result, 12);
        auto shell = graph.compile();
        ASSERT_TRUE(shell);
        *shell = *copy; // Copy assignment also rebuilds descriptor pointers.
        copy.reset();
        ASSERT_TRUE(shell->exec(BackendBlock{.outputs = outputs, .frames = 1}));
        ExecutionPlan moved{std::move(*shell)};
        EXPECT_FALSE(shell->isBackendPrepared());
        EXPECT_FALSE(shell->exec(BackendBlock{.outputs = outputs, .frames = 1}));
        ASSERT_TRUE(moved.exec(BackendBlock{.outputs = outputs, .frames = 1}));
        EXPECT_EQ(result, 12);
        auto destination = graph.compile();
        ASSERT_TRUE(destination);
        *destination = std::move(moved);
        EXPECT_FALSE(moved.isBackendPrepared());
        EXPECT_FALSE(moved.exec(BackendBlock{.outputs = outputs, .frames = 1}));
        ASSERT_TRUE(destination->exec(BackendBlock{.outputs = outputs, .frames = 1}));
        EXPECT_EQ(result, 12);
        ASSERT_TRUE(graph.remove(source.id()));
        EXPECT_EQ(graph.node(*addedSource), nullptr);
        EXPECT_EQ(graph.collectRetiredNodes(), 0U);
        // Retirement keeps the old copied/moved plan's raw pointers usable.
        ASSERT_TRUE(destination->exec(BackendBlock{.outputs = outputs, .frames = 1}));
        EXPECT_EQ(result, 12);
    }

    /// @brief Test case: semantic failures preserve previously prepared state.
    TEST(BackendPreparation, SemanticFailuresPreservePreviouslyPreparedState) {
        DAG graph;
        const auto addedA = graph.add<EmptyProcessor>();
        ASSERT_TRUE(addedA);
        auto& a = *static_cast<EmptyProcessor*>(graph.node(*addedA));
        const auto addedB = graph.add<EmptyProcessor>();
        ASSERT_TRUE(addedB);
        auto& b = *static_cast<EmptyProcessor*>(graph.node(*addedB));
        a.mode = Mode::Constant;
        a.value = 5;
        ASSERT_TRUE(graph.markAsOutput(a.id()));
        auto plan = graph.compile(BackendConfig{.maxFrames = 1, .outputChannels = 1});
        ASSERT_TRUE(plan);
        const std::array unknown{maxNodeID};
        expectError(plan->prepareBackend(BackendConfig{}, {}, unknown), ErrorType::NodeNotFound);
        const std::array backward{detail::PlanConnection{b.channel(PortType::Out, 0), a.channel(PortType::In, 0)}};
        expectError(plan->prepareBackend(BackendConfig{}, backward, {}), ErrorType::InvalidExecutionOrder);
        const std::array illegal{detail::PlanConnection{a.channel(PortType::In, 0), b.channel(PortType::In, 0)}};
        expectError(plan->prepareBackend(BackendConfig{}, illegal, {}), ErrorType::InvalidDirection);
        const std::array missing{detail::PlanConnection{encode(maxNodeID, PortType::Out, 0), b.channel(PortType::In, 0)}};
        expectError(plan->prepareBackend(BackendConfig{}, missing, {}), ErrorType::NodeNotFound);
        const auto connection = detail::PlanConnection{a.channel(PortType::Out, 0), b.channel(PortType::In, 0)};
        const std::array duplicates{connection, connection};
        expectError(plan->prepareBackend(BackendConfig{}, duplicates, {}), ErrorType::ConnectionExists);
        const auto excessive=makeBackendConfig(std::numeric_limits<std::uint64_t>::max(),0,0,0);
        ASSERT_FALSE(excessive); EXPECT_EQ(excessive.error().code,ErrorCode::CapacityExceeded);
        EXPECT_TRUE(plan->isBackendPrepared());
        float output = 0;
        const std::array outputs{&output};
        ASSERT_TRUE(plan->exec(BackendBlock{.outputs = outputs, .frames = 1}));
        EXPECT_EQ(output, 5);
    }

    /// @brief Test case: unencodable legacy indices are rejected before narrowing.
    TEST(BackendPreparation, UnencodableLegacyIndicesAreRejectedBeforeNarrowing) {
        DAG graph;
        const auto addedA = graph.add<Processor>();
        ASSERT_TRUE(addedA);
        auto& a = *static_cast<Processor*>(graph.node(*addedA));
        const auto addedB = graph.add<Processor>();
        ASSERT_TRUE(addedB);
        auto& b = *static_cast<Processor*>(graph.node(*addedB));
        const auto rejected = graph.connect(Channel{b.id(), 0, PortType::In},
                                            Channel{a.id(), 65536, PortType::Out});
        ASSERT_FALSE(rejected);
        EXPECT_EQ(rejected.error(), ErrorType::InvalidChannelIndex);
        EXPECT_TRUE(graph.connections().empty());
        ASSERT_TRUE(graph.remove(a.id())); // Rejected insertion retained no borrow.
    }

    /// @brief Test case: descriptor views use physical inputs and backend outputs only.
    TEST(BackendCallbackAdapter, DescriptorViewsUsePhysicalInputsAndBackendOutputsOnly) {
        DAG graph;
        const auto addedNode = graph.add<EmptyProcessor>();
        ASSERT_TRUE(addedNode);
        auto& node = *static_cast<EmptyProcessor*>(graph.node(*addedNode));
        ASSERT_TRUE(graph.markAsOutput(node.id()));
        auto plan = graph.compile(BackendConfig{.maxFrames = 1, .inputChannels = 1, .outputChannels = 1});
        ASSERT_TRUE(plan);
        const float input = 7;
        float output = 99;
        const std::array inputs{InputChannel{123, &input}};
        const std::array outputs{OutputChannel{456, &output}};
        ASSERT_TRUE(plan->exec(ProcessContext{.inputs = inputs, .backendOutputs = outputs, .frames = 1}));
        EXPECT_EQ(output, 7);
        EXPECT_TRUE(node.physicalIDsZero);
        output = 99;
        EXPECT_FALSE(plan->exec(ProcessContext{.inputs = inputs, .outputs = outputs, .frames = 1}));
        EXPECT_EQ(output, 99);
    }
}
