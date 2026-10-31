#include <array>
#include <concepts>
#include <cstddef>
#include <expected>
#include <span>
#include <type_traits>
#include <utility>

#include <gtest/gtest.h>

// The context's public import must make its descriptor and numeric types usable.
import mka.audio.dsp.process_context;
import mka.audio.dsp.node;

namespace {
    using namespace mka::audio::dsp;

    static_assert(std::same_as<decltype(ProcessContext::inputs), std::span<const InputChannel>>);
    static_assert(std::same_as<decltype(ProcessContext::auxiliary), std::span<const InputChannel>>);
    static_assert(std::same_as<decltype(ProcessContext::outputs), std::span<const OutputChannel>>);
    static_assert(std::same_as<decltype(ProcessContext::backendOutputs), std::span<const OutputChannel>>);
    static_assert(std::same_as<decltype(ProcessContext::frames), FrameCount>);
    static_assert(std::is_aggregate_v<ProcessContext> && !std::is_polymorphic_v<ProcessContext>);
    static_assert(std::is_trivially_copyable_v<ProcessContext>);
    static_assert(std::is_trivially_destructible_v<ProcessContext>);
    static_assert(std::same_as<decltype(std::declval<const ProcessContext&>().inputs[0].buffer[0]),
                               const float&>);
    static_assert(std::same_as<decltype(std::declval<const ProcessContext&>().auxiliary[0].buffer[0]),
                               const float&>);
    static_assert(std::same_as<decltype(std::declval<const ProcessContext&>().outputs[0].buffer[0]),
                               float&>);
    static_assert(std::same_as<decltype(std::declval<const ProcessContext&>().backendOutputs[0].buffer[0]),
                               float&>);

    template<typename Context>
    concept RebindableInput = requires(const Context& ctx, const float* pointer) {
        ctx.inputs[0].buffer = pointer;
    };
    template<typename Context>
    concept RebindableOutput = requires(const Context& ctx, float* pointer) {
        ctx.outputs[0].buffer = pointer;
        ctx.backendOutputs[0].buffer = pointer;
    };
    static_assert(!RebindableInput<ProcessContext> && !RebindableOutput<ProcessContext>);

    constexpr bool hasExactlyFiveDefaultFields() {
        const auto [inputs, auxiliary, outputs, backendOutputs, frames] = ProcessContext{};
        return inputs.empty() && auxiliary.empty() && outputs.empty() &&
               backendOutputs.empty() && frames == 0;
    }
    static_assert(hasExactlyFiveDefaultFields());
    static_assert(noexcept(validateContextBindings(std::declval<const PreparedContextBindings&>())));
    static_assert(std::same_as<
        decltype(validateContextBindings(std::declval<const PreparedContextBindings&>())), Result>);

    struct NothrowConsumer {
        void operator()(const ProcessContext&) const noexcept {}
    };
    struct ThrowingConsumer {
        void operator()(const ProcessContext&) const {}
    };
    struct NonVoidConsumer {
        int operator()(const ProcessContext&) const noexcept { return 0; }
    };
    template<typename Consumer>
    concept AcceptedContextConsumer = requires(const PreparedContextBindings& bindings, Consumer&& consume) {
        withProcessContext(bindings, std::span<const InputChannel>{},
                           std::span<const OutputChannel>{}, FrameCount{},
                           std::forward<Consumer>(consume));
    };
    static_assert(AcceptedContextConsumer<NothrowConsumer>);
    static_assert(!AcceptedContextConsumer<ThrowingConsumer>);
    static_assert(!AcceptedContextConsumer<NonVoidConsumer>);
    static_assert(noexcept(withProcessContext(std::declval<const PreparedContextBindings&>(),
        std::span<const InputChannel>{}, std::span<const OutputChannel>{},
        FrameCount{}, NothrowConsumer{})));
    static_assert(std::same_as<decltype(withProcessContext(std::declval<const PreparedContextBindings&>(),
        std::span<const InputChannel>{}, std::span<const OutputChannel>{},
        FrameCount{}, NothrowConsumer{})), bool>);

    void expectNullBuffer(const Result& result) {
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error(), ErrorType::NullBuffer);
    }

    // Concrete Node used only to exercise the public context API. Roles are
    // prepared explicitly; this does not implement DAG role detection/routing.
    class ContextNode final : public Node {
    public:
        explicit ContextNode(const NodeID id, const float gain = 1.F,
                             const bool writeBackend = false)
            : Node(id, Config{}), gain_(gain), writeBackend_(writeBackend) {}

        void process(const ProcessContext& ctx) noexcept override {
            ++calls;
            inputCount = ctx.inputs.size();
            backendOutputCount = ctx.backendOutputs.size();
            if (ctx.inputs.empty()) return;
            for (FrameCount frame = 0; frame < ctx.frames; ++frame) {
                const auto sample = ctx.inputs.back().buffer[frame];
                if (!ctx.outputs.empty()) ctx.outputs[0].buffer[frame] = sample * gain_;
                if (writeBackend_ && !ctx.backendOutputs.empty()) {
                    ctx.backendOutputs[0].buffer[frame] = sample + 100.F;
                }
            }
        }

        unsigned calls = 0;
        std::size_t inputCount = 0;
        std::size_t backendOutputCount = 0;

    private:
        float gain_;
        bool writeBackend_;
    };

    TEST(ProcessContextViews, DefaultsToFiveEmptyNonOwningFields) {
        const ProcessContext ctx{};
        EXPECT_TRUE(ctx.inputs.empty());
        EXPECT_TRUE(ctx.auxiliary.empty());
        EXPECT_TRUE(ctx.outputs.empty());
        EXPECT_TRUE(ctx.backendOutputs.empty());
        EXPECT_EQ(ctx.frames, 0);
    }

    TEST(ProcessContextPreparation, ValidatesInternalPointersWithoutMutatingBindings) {
        const std::array<float, 2> input{2.F, 3.F};
        std::array<float, 2> output{7.F, 11.F};
        const std::array validInputs{InputChannel{encode(1, PortType::In, 0), input.data()}};
        const std::array validAuxiliary{InputChannel{encode(1, PortType::Aux, 0), input.data()}};
        const std::array validOutputs{OutputChannel{encode(1, PortType::Out, 0), output.data()}};
        const std::array nullInputs{InputChannel{}};
        const std::array nullOutputs{OutputChannel{}};
        PreparedContextBindings bindings{.inputs = validInputs, .auxiliary = validAuxiliary,
                                         .outputs = validOutputs, .maxFrames = 2};
        ASSERT_TRUE(validateContextBindings(bindings));
        for (unsigned invalidView = 0; invalidView < 3; ++invalidView) {
            auto invalid = bindings;
            if (invalidView == 0) invalid.inputs = nullInputs;
            if (invalidView == 1) invalid.auxiliary = nullInputs;
            if (invalidView == 2) invalid.outputs = nullOutputs;
            const auto before = invalid;
            expectNullBuffer(validateContextBindings(invalid));
            EXPECT_EQ(invalid.inputs.data(), before.inputs.data());
            EXPECT_EQ(invalid.auxiliary.data(), before.auxiliary.data());
            EXPECT_EQ(invalid.outputs.data(), before.outputs.data());
        }
        EXPECT_EQ(output, (std::array<float, 2>{7.F, 11.F}));
    }

    TEST(ProcessContextPreparation, SourceIgnoresUnusedInternalInputsAndZeroMaximumAllowsPreparationNulls) {
        const std::array nullInputs{InputChannel{}};
        const std::array nullOutputs{OutputChannel{}};
        PreparedContextBindings source{.inputs = nullInputs, .maxFrames = 2, .isSource = true};
        EXPECT_TRUE(validateContextBindings(source));
        source.isSource = false;
        expectNullBuffer(validateContextBindings(source));
        const PreparedContextBindings zero{.inputs = nullInputs, .auxiliary = nullInputs,
                                           .outputs = nullOutputs, .maxFrames = 0};
        EXPECT_TRUE(validateContextBindings(zero));
    }

    TEST(ProcessContextViews, OrdinaryNodeUsesAllInternalViewsAndIgnoresUnusedBackendData) {
        const std::array<float, 2> samples{3.F, 5.F};
        const std::array<float, 2> auxSamples{7.F, 11.F};
        std::array<float, 2> logicalOutput{};
        const std::array inputs{InputChannel{encode(1, PortType::In, 0), samples.data()}};
        const std::array auxiliary{InputChannel{encode(1, PortType::Aux, 0), auxSamples.data()}};
        const std::array outputs{OutputChannel{encode(1, PortType::Out, 0), logicalOutput.data()}};
        // Unused backend views deliberately exceed zero capacities and are null.
        const std::array unusedBackendInputs{InputChannel{}};
        const std::array unusedBackendOutputs{OutputChannel{}};
        const PreparedContextBindings bindings{.inputs = inputs, .auxiliary = auxiliary,
                                                .outputs = outputs, .maxFrames = 2};
        ASSERT_TRUE(validateContextBindings(bindings));
        ContextNode node{1, 2.F};
        ASSERT_TRUE(withProcessContext(bindings, unusedBackendInputs, unusedBackendOutputs, 2,
            [&](const ProcessContext& ctx) noexcept {
                EXPECT_EQ(ctx.inputs.data(), inputs.data());
                EXPECT_EQ(ctx.auxiliary.data(), auxiliary.data());
                EXPECT_EQ(ctx.outputs.data(), outputs.data());
                EXPECT_TRUE(ctx.backendOutputs.empty());
                node.process(ctx);
            }));
        EXPECT_EQ(logicalOutput, (std::array<float, 2>{6.F, 10.F}));
        EXPECT_EQ(node.calls, 1U);
    }

    TEST(ProcessContextViews, SourceAloneReceivesEveryPhysicalInputDespiteEmptyLayout) {
        const std::array<std::array<float, 2>, 3> samples{{{1.F, 2.F}, {3.F, 4.F}, {5.F, 6.F}}};
        std::array<float, 2> logicalOutput{0.F, 19.F};
        const std::array backendInputs{InputChannel{0, samples[0].data()},
                                      InputChannel{0, samples[1].data()},
                                      InputChannel{0, samples[2].data()}};
        const std::array outputs{OutputChannel{encode(1, PortType::Out, 0), logicalOutput.data()}};
        const PreparedContextBindings bindings{.outputs = outputs, .maxFrames = 2,
                                                .maxBackendInputChannels = 3, .isSource = true};
        ContextNode node{1};
        ASSERT_TRUE(node.config().layoutInput.empty());
        ASSERT_TRUE(withProcessContext(bindings, backendInputs, {}, 1,
            [&](const ProcessContext& ctx) noexcept {
                ASSERT_EQ(ctx.inputs.size(), 3U);
                EXPECT_EQ(ctx.frames, 1);
                EXPECT_EQ(ctx.inputs.data(), backendInputs.data());
                for (std::size_t lane = 0; lane < samples.size(); ++lane) {
                    EXPECT_EQ(ctx.inputs[lane].buffer, samples[lane].data());
                    EXPECT_EQ(ctx.inputs[lane].channel, 0);
                }
                EXPECT_TRUE(ctx.backendOutputs.empty());
                node.process(ctx);
            }));
        EXPECT_EQ(node.inputCount, 3U);
        EXPECT_EQ(logicalOutput, (std::array<float, 2>{5.F, 19.F}));
    }

    TEST(ProcessContextViews, OutputAloneReceivesEveryBackendOutputAlongsideLogicalOutputs) {
        const std::array<float, 2> input{2.F, 4.F};
        std::array<float, 2> logicalOutput{};
        std::array<std::array<float, 2>, 2> physicalOutputs{};
        const std::array inputs{InputChannel{encode(1, PortType::In, 0), input.data()}};
        const std::array outputs{OutputChannel{encode(1, PortType::Out, 0), logicalOutput.data()}};
        const std::array backendOutputs{OutputChannel{0, physicalOutputs[0].data()},
                                       OutputChannel{0, physicalOutputs[1].data()}};
        const PreparedContextBindings bindings{.inputs = inputs, .outputs = outputs,
            .maxFrames = 2, .maxBackendOutputChannels = 2, .isOutput = true};
        ContextNode node{1, 3.F, true};
        ASSERT_TRUE(withProcessContext(bindings, {}, backendOutputs, 2,
            [&](const ProcessContext& ctx) noexcept {
                EXPECT_EQ(ctx.inputs.data(), inputs.data());
                EXPECT_EQ(ctx.outputs.data(), outputs.data());
                ASSERT_EQ(ctx.outputs.size(), 1U);
                ASSERT_EQ(ctx.backendOutputs.size(), 2U);
                EXPECT_EQ(ctx.backendOutputs.data(), backendOutputs.data());
                EXPECT_NE(ctx.outputs[0].buffer, ctx.backendOutputs[0].buffer);
                EXPECT_EQ(ctx.backendOutputs[0].channel, 0);
                EXPECT_EQ(ctx.backendOutputs[1].channel, 0);
                node.process(ctx);
            }));
        EXPECT_EQ(logicalOutput, (std::array<float, 2>{6.F, 12.F}));
        EXPECT_EQ(physicalOutputs[0], (std::array<float, 2>{102.F, 104.F}));
        EXPECT_EQ(physicalOutputs[1], (std::array<float, 2>{}));
        EXPECT_EQ(node.backendOutputCount, 2U);
    }

    TEST(ProcessContextViews, SourceAndOutputRolesAreIndependentOnTheSameNode) {
        const std::array<float, 2> first{1.F, 2.F};
        const std::array<float, 2> second{7.F, 8.F};
        std::array<float, 2> logicalOutput{};
        std::array<float, 2> backendOutput{};
        const std::array backendInputs{InputChannel{0, first.data()}, InputChannel{0, second.data()}};
        const std::array outputs{OutputChannel{encode(1, PortType::Out, 0), logicalOutput.data()}};
        const std::array backendOutputs{OutputChannel{0, backendOutput.data()}};
        const PreparedContextBindings bindings{.outputs = outputs, .maxFrames = 2,
            .maxBackendInputChannels = 2, .maxBackendOutputChannels = 1,
            .isSource = true, .isOutput = true};
        ContextNode node{1, 2.F, true};
        ASSERT_TRUE(node.config().layoutInput.empty());
        ASSERT_TRUE(withProcessContext(bindings, backendInputs, backendOutputs, 2,
            [&](const ProcessContext& ctx) noexcept {
                ASSERT_EQ(ctx.inputs.size(), 2U);
                ASSERT_EQ(ctx.outputs.size(), 1U);
                ASSERT_EQ(ctx.backendOutputs.size(), 1U);
                node.process(ctx);
            }));
        EXPECT_EQ(node.inputCount, 2U);
        EXPECT_EQ(node.backendOutputCount, 1U);
        EXPECT_EQ(logicalOutput, (std::array<float, 2>{14.F, 16.F}));
        EXPECT_EQ(backendOutput, (std::array<float, 2>{107.F, 108.F}));
    }

    TEST(ProcessContextViews, OutputNodeKeepsItsLogicalBufferForASuccessor) {
        const std::array<float, 2> samples{3.F, 4.F};
        std::array<float, 2> logicalOutput{};
        std::array<float, 2> physicalOutput{};
        std::array<float, 2> successorOutput{};
        const std::array backendInputs{InputChannel{0, samples.data()}};
        const std::array producerOutputs{OutputChannel{encode(1, PortType::Out, 0), logicalOutput.data()}};
        const std::array backendOutputs{OutputChannel{0, physicalOutput.data()}};
        const PreparedContextBindings producerBindings{.outputs = producerOutputs, .maxFrames = 2,
            .maxBackendInputChannels = 1, .maxBackendOutputChannels = 1,
            .isSource = true, .isOutput = true};
        ContextNode producer{1, 2.F, true};
        ContextNode successor{2, 3.F};
        ASSERT_TRUE(withProcessContext(producerBindings, backendInputs, backendOutputs, 2,
            [&](const ProcessContext& ctx) noexcept {
                ASSERT_EQ(ctx.inputs.size(), 1U);
                ASSERT_EQ(ctx.outputs.size(), 1U);
                ASSERT_EQ(ctx.backendOutputs.size(), 1U);
                producer.process(ctx);
            }));
        const std::array successorInputs{InputChannel{producerOutputs[0].channel, logicalOutput.data()}};
        const std::array successorOutputs{OutputChannel{encode(2, PortType::Out, 0), successorOutput.data()}};
        const PreparedContextBindings successorBindings{.inputs = successorInputs,
            .outputs = successorOutputs, .maxFrames = 2};
        ASSERT_TRUE(withProcessContext(successorBindings, {}, backendOutputs, 2,
            [&](const ProcessContext& ctx) noexcept {
                ASSERT_EQ(ctx.inputs.size(), 1U);
                EXPECT_EQ(ctx.inputs[0].buffer, logicalOutput.data());
                EXPECT_TRUE(ctx.backendOutputs.empty());
                successor.process(ctx);
            }));
        EXPECT_EQ(successorOutput, (std::array<float, 2>{18.F, 24.F}));
        EXPECT_EQ(physicalOutput, (std::array<float, 2>{103.F, 104.F}));
        EXPECT_EQ(producer.calls, 1U);
        EXPECT_EQ(successor.calls, 1U);
    }

    TEST(ProcessContextGuards, ZeroFramesSkipTheConsumerEvenWithNullDescriptors) {
        const std::array nullInputs{InputChannel{}};
        const std::array nullOutputs{OutputChannel{}};
        const PreparedContextBindings bindings{.inputs = nullInputs, .auxiliary = nullInputs,
            .outputs = nullOutputs, .maxFrames = 2,
            .maxBackendInputChannels = 1, .maxBackendOutputChannels = 1,
            .isSource = true, .isOutput = true};
        unsigned calls = 0;
        EXPECT_TRUE(withProcessContext(bindings, nullInputs, nullOutputs, 0,
            [&](const ProcessContext&) noexcept { ++calls; }));
        EXPECT_EQ(calls, 0U);
    }

    TEST(ProcessContextGuards, ZeroChannelsAllowAPositiveFrameContext) {
        const PreparedContextBindings bindings{.maxFrames = 2, .isSource = true, .isOutput = true};
        ASSERT_TRUE(validateContextBindings(bindings));
        unsigned calls = 0;
        ASSERT_TRUE(withProcessContext(bindings, {}, {}, 2,
            [&](const ProcessContext& ctx) noexcept {
                ++calls;
                EXPECT_TRUE(ctx.inputs.empty());
                EXPECT_TRUE(ctx.auxiliary.empty());
                EXPECT_TRUE(ctx.outputs.empty());
                EXPECT_TRUE(ctx.backendOutputs.empty());
                EXPECT_EQ(ctx.frames, 2);
            }));
        EXPECT_EQ(calls, 1U);
    }

    TEST(ProcessContextGuards, ExcessiveFramesSkipSamplesAndTheNextCallbackUsesFreshViews) {
        const std::array<float, 2> oldInput{2.F, 3.F};
        std::array<float, 2> oldOutput{11.F, 13.F};
        const std::array oldInputs{InputChannel{0, oldInput.data()}};
        const std::array oldOutputs{OutputChannel{0, oldOutput.data()}};
        const PreparedContextBindings bindings{.maxFrames = 2,
            .maxBackendInputChannels = 1, .maxBackendOutputChannels = 1,
            .isSource = true, .isOutput = true};
        unsigned skippedCalls = 0;
        EXPECT_FALSE(withProcessContext(bindings, oldInputs, oldOutputs, 3,
            [&](const ProcessContext&) noexcept { ++skippedCalls; }));
        EXPECT_EQ(skippedCalls, 0U);
        EXPECT_EQ(oldOutput, (std::array<float, 2>{11.F, 13.F}));
        const std::array<float, 2> newInput{5.F, 7.F};
        std::array<float, 2> newOutput{};
        const std::array newInputs{InputChannel{0, newInput.data()}};
        const std::array newOutputs{OutputChannel{0, newOutput.data()}};
        ContextNode node{1, 1.F, true};
        ASSERT_TRUE(withProcessContext(bindings, newInputs, newOutputs, 2,
            [&](const ProcessContext& ctx) noexcept {
                ASSERT_EQ(ctx.inputs.size(), 1U);
                ASSERT_EQ(ctx.backendOutputs.size(), 1U);
                EXPECT_EQ(ctx.inputs[0].buffer, newInput.data());
                EXPECT_EQ(ctx.backendOutputs[0].buffer, newOutput.data());
                node.process(ctx);
            }));
        EXPECT_EQ(node.calls, 1U);
        EXPECT_EQ(newOutput, (std::array<float, 2>{105.F, 107.F}));
        EXPECT_EQ(oldOutput, (std::array<float, 2>{11.F, 13.F}));
    }

    TEST(ProcessContextGuards, BackendInputCountOverflowSkipsTheConsumer) {
        const std::array<float, 1> samples{2.F};
        const std::array tooMany{InputChannel{0, samples.data()}, InputChannel{0, samples.data()}};
        const PreparedContextBindings bindings{.maxFrames = 1,
            .maxBackendInputChannels = 1, .isSource = true};
        unsigned calls = 0;
        EXPECT_FALSE(withProcessContext(bindings, tooMany, {}, 1,
            [&](const ProcessContext&) noexcept { ++calls; }));
        EXPECT_EQ(calls, 0U);
        EXPECT_TRUE(withProcessContext(bindings, std::span<const InputChannel>{tooMany}.first(1), {}, 1,
            [&](const ProcessContext&) noexcept { ++calls; }));
        EXPECT_EQ(calls, 1U);
    }

    TEST(ProcessContextGuards, BackendOutputCountOverflowSkipsTheConsumerAndLeavesSamplesUntouched) {
        std::array<float, 2> samples{17.F, 19.F};
        const std::array tooMany{OutputChannel{0, &samples[0]}, OutputChannel{0, &samples[1]}};
        const PreparedContextBindings bindings{.maxFrames = 1,
            .maxBackendOutputChannels = 1, .isOutput = true};
        unsigned calls = 0;
        EXPECT_FALSE(withProcessContext(bindings, {}, tooMany, 1,
            [&](const ProcessContext&) noexcept { ++calls; }));
        EXPECT_EQ(calls, 0U);
        EXPECT_EQ(samples, (std::array<float, 2>{17.F, 19.F}));
        EXPECT_TRUE(withProcessContext(bindings, {}, std::span<const OutputChannel>{tooMany}.first(1), 1,
            [&](const ProcessContext&) noexcept { ++calls; }));
        EXPECT_EQ(calls, 1U);
    }

    TEST(ProcessContextGuards, NullPointersInEverySelectedViewRejectBeforeTheConsumer) {
        const std::array<float, 1> inputSample{2.F};
        std::array<float, 1> outputSample{7.F};
        std::array<float, 1> backendSample{11.F};
        const std::array inputs{InputChannel{encode(1, PortType::In, 0), inputSample.data()}};
        const std::array outputs{OutputChannel{encode(1, PortType::Out, 0), outputSample.data()}};
        const std::array physicalInputs{InputChannel{0, inputSample.data()}};
        const std::array physicalOutputs{OutputChannel{0, backendSample.data()}};
        const std::array nullInputs{InputChannel{}};
        const std::array nullOutputs{OutputChannel{}};
        for (unsigned view = 0; view < 5; ++view) {
            SCOPED_TRACE(view);
            PreparedContextBindings bindings{.inputs = inputs, .auxiliary = inputs,
                .outputs = outputs, .maxFrames = 1,
                .maxBackendInputChannels = 1, .maxBackendOutputChannels = 1,
                .isOutput = true};
            std::span<const InputChannel> backendInputs = physicalInputs;
            std::span<const OutputChannel> backendOutputs = physicalOutputs;
            if (view == 0) bindings.inputs = nullInputs;
            if (view == 1) bindings.auxiliary = nullInputs;
            if (view == 2) bindings.outputs = nullOutputs;
            if (view == 3) backendOutputs = nullOutputs;
            if (view == 4) {
                bindings.isSource = true;
                backendInputs = nullInputs;
            }
            unsigned calls = 0;
            EXPECT_FALSE(withProcessContext(bindings, backendInputs, backendOutputs, 1,
                [&](const ProcessContext&) noexcept { ++calls; }));
            EXPECT_EQ(calls, 0U);
            EXPECT_EQ(outputSample, (std::array<float, 1>{7.F}));
            EXPECT_EQ(backendSample, (std::array<float, 1>{11.F}));
        }
    }

    TEST(ProcessContextLifetime, SuccessiveCallbacksBorrowOnlyTheirCurrentDescriptorStorage) {
        const PreparedContextBindings bindings{.maxFrames = 2,
            .maxBackendInputChannels = 1, .maxBackendOutputChannels = 1,
            .isSource = true, .isOutput = true};
        const auto before = bindings;
        ContextNode node{1, 1.F, true};
        const std::array<float, 2> firstInput{1.F, 2.F};
        std::array<float, 2> firstOutput{};
        {
            const std::array inputs{InputChannel{0, firstInput.data()}};
            const std::array outputs{OutputChannel{0, firstOutput.data()}};
            ASSERT_TRUE(withProcessContext(bindings, inputs, outputs, 2,
                [&](const ProcessContext& ctx) noexcept {
                    ASSERT_EQ(ctx.inputs.size(), 1U);
                    ASSERT_EQ(ctx.backendOutputs.size(), 1U);
                    EXPECT_EQ(ctx.inputs.data(), inputs.data());
                    EXPECT_EQ(ctx.backendOutputs.data(), outputs.data());
                    node.process(ctx);
                }));
        } // Callback descriptor storage is now gone; bindings must not retain it.
        const auto firstAfterCallback = firstOutput;
        {
            const std::array<float, 2> secondInput{7.F, 11.F};
            std::array<float, 2> secondOutput{};
            const std::array inputs{InputChannel{0, secondInput.data()}};
            const std::array outputs{OutputChannel{0, secondOutput.data()}};
            ASSERT_TRUE(withProcessContext(bindings, inputs, outputs, 2,
                [&](const ProcessContext& ctx) noexcept {
                    ASSERT_EQ(ctx.inputs.size(), 1U);
                    ASSERT_EQ(ctx.backendOutputs.size(), 1U);
                    EXPECT_EQ(ctx.inputs.data(), inputs.data());
                    EXPECT_EQ(ctx.backendOutputs.data(), outputs.data());
                    EXPECT_NE(ctx.inputs[0].buffer, firstInput.data());
                    EXPECT_NE(ctx.backendOutputs[0].buffer, firstOutput.data());
                    node.process(ctx);
                }));
            EXPECT_EQ(secondOutput, (std::array<float, 2>{107.F, 111.F}));
            EXPECT_EQ(firstOutput, firstAfterCallback);
        }
        EXPECT_EQ(bindings.inputs.data(), before.inputs.data());
        EXPECT_EQ(bindings.auxiliary.data(), before.auxiliary.data());
        EXPECT_EQ(bindings.outputs.data(), before.outputs.data());
        EXPECT_EQ(node.calls, 2U);
        EXPECT_EQ(firstOutput, (std::array<float, 2>{101.F, 102.F}));
    }
}
