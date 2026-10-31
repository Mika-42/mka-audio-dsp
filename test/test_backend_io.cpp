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

    struct PointerOracle {
        std::span<const float* const> inputs;
        std::span<float* const> outputs;
        NodeID outputNode = 0;
        NodeID inputNode = 0;
        float** logicalOutput = nullptr;
        const float** logicalInput = nullptr;
    };
    thread_local const PointerOracle* oracle = nullptr;
    struct OracleScope {
        const PointerOracle* previous;
        explicit OracleScope(const PointerOracle& current) : previous(std::exchange(oracle, &current)) {}
        ~OracleScope() { oracle = previous; }
    };

    enum class Mode { Copy, Constant, Auxiliary, NoWrite };
    class Processor : public Node {
    public:
        explicit Processor(const NodeID id)
            : Processor(id, Config{.layoutInput = {0}, .layoutAuxiliary = {0}, .layoutOutput = {0}}) {}
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

        Mode mode = Mode::Copy;
        float value = 1.F;
        float gain = 1.F;
        float backendBias = 0.F;
        std::size_t inputLane = 0;
        std::size_t outputLane = 0;
        std::size_t secondInputLane = std::numeric_limits<std::size_t>::max();
        std::size_t secondOutputLane = std::numeric_limits<std::size_t>::max();
        bool alternateFrames = false;
        bool writeLogical = true;
        unsigned calls = 0;
        std::size_t inputCount = 0, auxiliaryCount = 0, logicalOutputCount = 0, backendOutputCount = 0;
        ChannelID inputChannel = 0, outputChannel = 0;
        bool directViews = false, viewsDistinct = false, physicalIDsZero = false, physicalInitiallyZero = false;
        NodeID* trace = nullptr; // Test event buffer, never a backend sample pointer.
        std::size_t* traceSize = nullptr;
    protected:
        Processor(const NodeID id, const Config& config) : Node(id, config) {}
    };
    class EmptyProcessor final : public Processor {
    public:
        explicit EmptyProcessor(const NodeID id) : Processor(id, Config{}) {}
    };
    class SparseProcessor final : public Processor {
    public:
        explicit SparseProcessor(const NodeID id)
            : Processor(id, Config{.layoutInput = {65535}, .layoutAuxiliary = {2}, .layoutOutput = {42}}) {}
    };

    void expectError(const Result& result, const ErrorType error) {
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error(), error);
    }

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
        ASSERT_TRUE(graph.connect(source.channel(PortType::Out, 0), sink.channel(PortType::In, 0)));
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
        ASSERT_TRUE(graph.connect(source.channel(PortType::Out, 0), sink.channel(PortType::In, 0)));
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

    TEST(BackendLifetime, EveryCallbackUsesItsCurrentAddressesAndChannelCounts) {
        DAG graph;
        const auto addedNode = graph.add<EmptyProcessor>();
        ASSERT_TRUE(addedNode);
        auto& node = *static_cast<EmptyProcessor*>(graph.node(*addedNode));
        ASSERT_TRUE(graph.markAsOutput(node.id()));
        auto plan = graph.compile(BackendConfig{.maxFrames = 2, .inputChannels = 2, .outputChannels = 2});
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
        ASSERT_TRUE(plan->exec(BackendBlock{.frames = 2}));
        EXPECT_EQ(node.inputCount, 0U);
        EXPECT_EQ(node.backendOutputCount, 0U);
        EXPECT_EQ(firstOutput, previous);
    }

    TEST(BackendGuards, RejectedBlocksTouchNoSamplesAndTheNextValidCallbackRecovers) {
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
        EXPECT_EQ(node.calls, 0U);
        EXPECT_EQ(output, (std::array<float, 3>{41, 43, 47}));
        ASSERT_TRUE(plan->exec(BackendBlock{inputs, outputs, 2}));
        EXPECT_EQ(node.calls, 1U);
        EXPECT_EQ(output, (std::array<float, 3>{3, 5, 47}));
    }

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

    TEST(BackendPreparation, ZeroMaximumFramesCanPrepareANoopPlan) {
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
        EXPECT_EQ(node.calls, 0U);
    }

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
        ASSERT_TRUE(graph.connect(source.channel(PortType::Out, 65535), sink.channel(PortType::Aux, 65535)));
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

    TEST(BackendRouting, UnproducedLogicalChannelsReadSilenceInsteadOfPhysicalInputs) {
        DAG graph;
        const auto addedSource = graph.add<EmptyProcessor>();
        ASSERT_TRUE(addedSource);
        auto& source = *static_cast<EmptyProcessor*>(graph.node(*addedSource)); // No declared logical outputs.
        const auto addedSink = graph.add<Processor>();
        ASSERT_TRUE(addedSink);
        auto& sink = *static_cast<Processor*>(graph.node(*addedSink));
        ASSERT_TRUE(graph.connect(source.channel(PortType::Out, 65535), sink.channel(PortType::In, 0)));
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
        ASSERT_TRUE(graph.connect(source.channel(PortType::Out, 0), sink.channel(PortType::In, 0)));
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
        ASSERT_TRUE(graph.connect(source.channel(PortType::Out, 42), sink.channel(PortType::In, 65535)));
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
        ASSERT_TRUE(graph.connect(source.channel(PortType::Out, 42), sink.channel(PortType::Aux, 2)));
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
        ASSERT_TRUE(graph.connect(source.channel(PortType::Out, 0), first.channel(PortType::In, 0)));
        ASSERT_TRUE(graph.connect(source.channel(PortType::Out, 0), second.channel(PortType::In, 0)));
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
        ASSERT_TRUE(graph.connect(source.channel(PortType::Out, 0), sink.channel(PortType::In, 0)));
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
        expectError(plan->prepareBackend(BackendConfig{}, duplicates, {}), ErrorType::ChannelAlreadyConnected);
        const BackendConfig excessive{.inputChannels = std::numeric_limits<std::size_t>::max()};
        expectError(plan->prepareBackend(excessive, {}, {}), ErrorType::PreparationCapacityExceeded);
        EXPECT_TRUE(plan->isBackendPrepared());
        float output = 0;
        const std::array outputs{&output};
        ASSERT_TRUE(plan->exec(BackendBlock{.outputs = outputs, .frames = 1}));
        EXPECT_EQ(output, 5);
    }

    TEST(BackendPreparation, UnencodableLegacyIndicesAreRejectedBeforeNarrowing) {
        DAG graph;
        const auto addedA = graph.add<Processor>();
        ASSERT_TRUE(addedA);
        auto& a = *static_cast<Processor*>(graph.node(*addedA));
        const auto addedB = graph.add<Processor>();
        ASSERT_TRUE(addedB);
        auto& b = *static_cast<Processor*>(graph.node(*addedB));
        ASSERT_TRUE(graph.connect(Channel{a.id(), 65536, PortType::Out}, Channel{b.id(), 0, PortType::In}));
        auto plan = graph.compile(BackendConfig{.maxFrames = 1});
        ASSERT_FALSE(plan);
        EXPECT_EQ(plan.error(), ErrorType::InvalidChannelIndex);
        ASSERT_TRUE(graph.remove(a.id())); // Failed compilation retained no borrow.
    }

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
