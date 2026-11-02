#include <array>
#include <concepts>
#include <cstddef>
#include <expected>
#include <limits>
#include <span>
#include <type_traits>
#include <utility>

#include <gtest/gtest.h>

import mka.audio.dsp.channel;

namespace {
    using namespace mka::audio::dsp;

    static_assert(std::same_as<decltype(InputChannel::channel), ChannelID>);
    static_assert(std::same_as<decltype(OutputChannel::channel), ChannelID>);
    static_assert(std::same_as<decltype(InputChannel::buffer), const float*>);
    static_assert(std::same_as<decltype(OutputChannel::buffer), float*>);
    static_assert(std::is_aggregate_v<InputChannel> && std::is_aggregate_v<OutputChannel>);
    static_assert(std::is_standard_layout_v<InputChannel> && std::is_standard_layout_v<OutputChannel>);
    static_assert(std::is_trivially_copyable_v<InputChannel> && std::is_trivially_copyable_v<OutputChannel>);
    static_assert(std::is_trivially_destructible_v<InputChannel> &&
                  std::is_trivially_destructible_v<OutputChannel>);
    static_assert(!std::is_polymorphic_v<InputChannel> && !std::is_polymorphic_v<OutputChannel>);
    static_assert(std::is_convertible_v<float*, const float*>);
    static_assert(!std::is_convertible_v<const float*, float*>);

    template<typename Descriptor>
    concept WritableSamples = requires(Descriptor& channel) { channel.buffer[0] = 1.F; };

    template<typename Descriptor>
    concept RebindableDescriptor = requires(Descriptor& channel, float* buffer) {
        channel.buffer = buffer;
    };

    static_assert(!WritableSamples<InputChannel> && !WritableSamples<const InputChannel>);
    static_assert(WritableSamples<OutputChannel> && WritableSamples<const OutputChannel>);
    static_assert(RebindableDescriptor<InputChannel> && RebindableDescriptor<OutputChannel>);
    static_assert(!RebindableDescriptor<const InputChannel> &&
                  !RebindableDescriptor<const OutputChannel>);
    static_assert(std::same_as<
        decltype(std::declval<std::span<const InputChannel>>()[0].buffer[0]), const float&>);
    static_assert(std::same_as<
        decltype(std::declval<std::span<const OutputChannel>>()[0].buffer[0]), float&>);
    static_assert(noexcept(validateChannelBuffers(std::span<const InputChannel>{}, FrameCount{})));
    static_assert(noexcept(validateChannelBuffers(std::span<const OutputChannel>{}, FrameCount{})));
    static_assert(std::same_as<
        decltype(validateChannelBuffers(std::span<const InputChannel>{}, FrameCount{})),
        std::expected<void, ErrorType>>);
    static_assert(std::same_as<
        decltype(validateChannelBuffers(std::span<const OutputChannel>{}, FrameCount{})),
        std::expected<void, ErrorType>>);
    static_assert(validateChannelBuffers(std::span<const InputChannel>{}, FrameCount{}).has_value());
    static_assert(validateChannelBuffers(std::span<const OutputChannel>{}, FrameCount{}).has_value());
    static_assert(InputChannel{}.channel == 0 && InputChannel{}.buffer == nullptr);
    static_assert(OutputChannel{}.channel == 0 && OutputChannel{}.buffer == nullptr);

    // Stateless test harness for a conforming consumer, not Node/ExecutionPlan
    // integration. The caller guarantees matching views and sufficient extents.
    void copySamples(const std::span<const InputChannel> inputs,
                     const std::span<const OutputChannel> outputs,
                     const FrameCount frames) noexcept {
        for (std::size_t channel = 0; channel < outputs.size(); ++channel) {
            for (FrameCount frame = 0; frame < frames; ++frame) {
                outputs[channel].buffer[frame] = inputs[channel].buffer[frame];
            }
        }
    }

    TEST(AudioChannelDescriptors, DefaultInitializationLeavesZeroIdentityAndNullPreparationBuffer) {
        const InputChannel input;
        const OutputChannel output;
        EXPECT_EQ(input.channel, 0);
        EXPECT_EQ(output.channel, 0);
        EXPECT_EQ(input.buffer, nullptr);
        EXPECT_EQ(output.buffer, nullptr);
    }

    TEST(AudioChannelDescriptors, CopiesPreserveLogicalIdentityAndBorrowCallerOwnedStorage) {
        const std::array<float, 3> inputOwner{2.F, 4.F, 6.F};
        std::array<float, 3> outputOwner{7.F, 8.F, 9.F};
        constexpr auto inputID = encode(42, PortType::In, 65535);
        constexpr auto outputID = encode(42, PortType::Out, 65535);
        {
            const InputChannel input{inputID, inputOwner.data()};
            const OutputChannel output{outputID, outputOwner.data()};
            const auto inputCopy = input;
            const auto outputCopy = output;
            EXPECT_EQ(inputCopy.channel, inputID);
            EXPECT_EQ(outputCopy.channel, outputID);
            EXPECT_EQ(inputCopy.buffer, inputOwner.data());
            EXPECT_EQ(outputCopy.buffer, outputOwner.data());
            outputCopy.buffer[1] = inputCopy.buffer[2];
        } // Trivial descriptor destruction cannot destroy caller-owned samples.
        EXPECT_EQ(inputOwner, (std::array<float, 3>{2.F, 4.F, 6.F}));
        EXPECT_EQ(outputOwner, (std::array<float, 3>{7.F, 6.F, 9.F}));
    }

    TEST(AudioChannelDescriptors, MonoConsumerWritesThroughConstOutputDescriptors) {
        const std::array<float, 4> inputOwner{1.F, -2.F, 3.F, -4.F};
        std::array<float, 4> outputOwner{};
        const std::array inputs{InputChannel{encode(1, PortType::In, 0), inputOwner.data()}};
        const std::array outputs{OutputChannel{encode(1, PortType::Out, 0), outputOwner.data()}};
        ASSERT_TRUE(validateChannelBuffers(inputs, FrameCount{4}));
        ASSERT_TRUE(validateChannelBuffers(outputs, FrameCount{4}));
        copySamples(inputs, outputs, 4);
        EXPECT_EQ(outputOwner, inputOwner);
        EXPECT_EQ(outputs[0].buffer, outputOwner.data());
    }

    TEST(AudioChannelDescriptors, StereoAndSixBackendLanesUsePositionWithZeroIdentity) {
        for (const auto channelCount : std::array<std::size_t, 2>{2, 6}) {
            SCOPED_TRACE(channelCount);
            std::array<std::array<float, 3>, 6> inputOwners{};
            std::array<std::array<float, 3>, 6> outputOwners{};
            std::array<InputChannel, 6> inputDescriptors{};
            std::array<OutputChannel, 6> outputDescriptors{};
            for (std::size_t channel = 0; channel < channelCount; ++channel) {
                for (std::size_t frame = 0; frame < 3; ++frame) {
                    inputOwners[channel][frame] = static_cast<float>(10 * channel + frame + 1);
                }
                inputDescriptors[channel] = {0, inputOwners[channel].data()};
                outputDescriptors[channel] = {0, outputOwners[channel].data()};
            }
            const auto inputs = std::span<const InputChannel>{inputDescriptors}.first(channelCount);
            const auto outputs = std::span<const OutputChannel>{outputDescriptors}.first(channelCount);
            ASSERT_TRUE(validateChannelBuffers(inputs, FrameCount{3}));
            ASSERT_TRUE(validateChannelBuffers(outputs, FrameCount{3}));
            copySamples(inputs, outputs, 3);
            for (std::size_t channel = 0; channel < channelCount; ++channel) {
                EXPECT_EQ(inputs[channel].channel, 0);
                EXPECT_EQ(outputs[channel].channel, 0);
                EXPECT_EQ(inputs[channel].buffer, inputOwners[channel].data());
                EXPECT_EQ(outputs[channel].buffer, outputOwners[channel].data());
                EXPECT_EQ(outputOwners[channel], inputOwners[channel]);
            }
        }
    }

    TEST(AudioChannelDescriptors, ExplicitReadOnlyViewAliasesMutableSamplesWithoutOwnership) {
        std::array<float, 2> owner{3.F, 5.F};
        const OutputChannel writable{encode(7, PortType::Out, 1), owner.data()};
        const InputChannel readOnly{writable.channel, writable.buffer};
        EXPECT_EQ(readOnly.buffer, writable.buffer);
        EXPECT_EQ(readOnly.channel, writable.channel);
        writable.buffer[1] = 11.F;
        EXPECT_FLOAT_EQ(readOnly.buffer[1], 11.F);
        EXPECT_FLOAT_EQ(owner[1], 11.F);
    }

    TEST(AudioChannelBuffers, PositiveFramesRejectNullInputAtEveryConsumedPositionWithoutMutation) {
        const std::array<std::array<float, 3>, 3> owners{{{1.F, 2.F, 3.F},
                                                       {4.F, 5.F, 6.F},
                                                       {7.F, 8.F, 9.F}}};
        for (std::size_t missing = 0; missing < owners.size(); ++missing) {
            SCOPED_TRACE(missing);
            std::array<InputChannel, 3> channels{};
            for (std::size_t channel = 0; channel < owners.size(); ++channel) {
                channels[channel] = {encode(1, PortType::In, static_cast<ChannelIndex>(channel)),
                                     channel == missing ? nullptr : owners[channel].data()};
            }
            const auto before = channels;
            const auto result = validateChannelBuffers(channels, FrameCount{3});
            ASSERT_FALSE(result.has_value());
            EXPECT_EQ(result.error(), ErrorType::NullBuffer);
            for (std::size_t channel = 0; channel < channels.size(); ++channel) {
                EXPECT_EQ(channels[channel].channel, before[channel].channel);
                EXPECT_EQ(channels[channel].buffer, before[channel].buffer);
            }
        }
    }

    TEST(AudioChannelBuffers, PositiveFramesRejectNullOutputAtEveryConsumedPositionWithoutMutation) {
        std::array<std::array<float, 3>, 3> owners{{{1.F, 2.F, 3.F},
                                                 {4.F, 5.F, 6.F},
                                                 {7.F, 8.F, 9.F}}};
        const auto samplesBefore = owners;
        for (std::size_t missing = 0; missing < owners.size(); ++missing) {
            SCOPED_TRACE(missing);
            std::array<OutputChannel, 3> channels{};
            for (std::size_t channel = 0; channel < owners.size(); ++channel) {
                channels[channel] = {encode(1, PortType::Out, static_cast<ChannelIndex>(channel)),
                                     channel == missing ? nullptr : owners[channel].data()};
            }
            const auto before = channels;
            const auto result = validateChannelBuffers(channels, FrameCount{3});
            ASSERT_FALSE(result.has_value());
            EXPECT_EQ(result.error(), ErrorType::NullBuffer);
            for (std::size_t channel = 0; channel < channels.size(); ++channel) {
                EXPECT_EQ(channels[channel].channel, before[channel].channel);
                EXPECT_EQ(channels[channel].buffer, before[channel].buffer);
            }
            EXPECT_EQ(owners, samplesBefore);
        }
    }

    TEST(AudioChannelBuffers, EmptyViewsAndZeroFramesDoNotAccessSamplesOrNullPointers) {
        EXPECT_TRUE(validateChannelBuffers(std::span<const InputChannel>{}, FrameCount{4}));
        EXPECT_TRUE(validateChannelBuffers(std::span<const OutputChannel>{}, FrameCount{4}));
        std::array<float, 2> sentinel{17.F, 19.F};
        const std::array inputs{InputChannel{}, InputChannel{0, sentinel.data()}};
        const std::array outputs{OutputChannel{}, OutputChannel{0, sentinel.data()}};
        ASSERT_TRUE(validateChannelBuffers(inputs, FrameCount{0}));
        ASSERT_TRUE(validateChannelBuffers(outputs, FrameCount{0}));
        copySamples(inputs, outputs, 0); // No dereference, even for default null descriptors.
        EXPECT_EQ(sentinel, (std::array<float, 2>{17.F, 19.F}));
        EXPECT_EQ(inputs[0].buffer, nullptr);
        EXPECT_EQ(outputs[0].buffer, nullptr);
        EXPECT_EQ(inputs[1].buffer, sentinel.data());
        EXPECT_EQ(outputs[1].buffer, sentinel.data());
    }

    TEST(AudioChannelBuffers, ValidationInspectsPointersOnlyEvenForMaximumFrameCount) {
        const float inputSample = 23.F;
        float outputSample = 29.F;
        const std::array inputs{InputChannel{0, &inputSample}};
        const std::array outputs{OutputChannel{0, &outputSample}};
        // The validator does not know buffer extents. This only checks pointer
        // presence; these one-sample buffers MUST NOT be processed for max frames.
        ASSERT_TRUE(validateChannelBuffers(inputs, std::numeric_limits<FrameCount>::max()));
        ASSERT_TRUE(validateChannelBuffers(outputs, std::numeric_limits<FrameCount>::max()));
        EXPECT_FLOAT_EQ(inputSample, 23.F);
        EXPECT_FLOAT_EQ(outputSample, 29.F);
    }

    TEST(AudioChannelBuffers, DisconnectedInputCanBorrowAReadOnlySilenceBuffer) {
        const std::array<float, 4> silence{};
        std::array<float, 4> outputOwner{1.F, 2.F, 3.F, 4.F};
        const std::array inputs{InputChannel{encode(1, PortType::In, 0), silence.data()}};
        const std::array outputs{OutputChannel{encode(1, PortType::Out, 0), outputOwner.data()}};
        ASSERT_TRUE(validateChannelBuffers(inputs, FrameCount{4}));
        ASSERT_TRUE(validateChannelBuffers(outputs, FrameCount{4}));
        copySamples(inputs, outputs, 4);
        EXPECT_EQ(outputOwner, silence);
        EXPECT_EQ(inputs[0].buffer, silence.data());
        EXPECT_EQ(silence, (std::array<float, 4>{}));
    }

    TEST(AudioChannelBuffers, CallbackHarnessRebindsBackendBuffersAndClearsBorrowedPointers) {
        std::array<InputChannel, 1> inputDescriptors{};
        std::array<OutputChannel, 1> outputDescriptors{};
        const std::array<float, 3> firstInput{1.F, 2.F, 3.F};
        std::array<float, 3> firstOutput{};
        {
            inputDescriptors[0] = {0, firstInput.data()};
            outputDescriptors[0] = {0, firstOutput.data()};
            ASSERT_TRUE(validateChannelBuffers(inputDescriptors, FrameCount{3}));
            ASSERT_TRUE(validateChannelBuffers(outputDescriptors, FrameCount{3}));
            copySamples(inputDescriptors, outputDescriptors, 3);
            EXPECT_EQ(firstOutput, firstInput);
            inputDescriptors = {};
            outputDescriptors = {};
        }
        const auto firstOutputAfterCallback = firstOutput;
        {
            const std::array<float, 3> secondInput{7.F, 8.F, 9.F};
            std::array<float, 3> secondOutput{};
            inputDescriptors[0] = {0, secondInput.data()};
            outputDescriptors[0] = {0, secondOutput.data()};
            EXPECT_NE(inputDescriptors[0].buffer, firstInput.data());
            EXPECT_NE(outputDescriptors[0].buffer, firstOutput.data());
            ASSERT_TRUE(validateChannelBuffers(inputDescriptors, FrameCount{3}));
            ASSERT_TRUE(validateChannelBuffers(outputDescriptors, FrameCount{3}));
            copySamples(inputDescriptors, outputDescriptors, 3);
            EXPECT_EQ(secondOutput, secondInput);
            EXPECT_EQ(firstOutput, firstOutputAfterCallback);
            // Explicit harness cleanup models the caller's lifetime contract;
            // aggregate types do not automatically prevent retained raw pointers.
            inputDescriptors = {};
            outputDescriptors = {};
        }
        EXPECT_EQ(inputDescriptors[0].buffer, nullptr);
        EXPECT_EQ(outputDescriptors[0].buffer, nullptr);
        EXPECT_EQ(firstOutput, firstOutputAfterCallback);
        EXPECT_EQ(firstInput, (std::array<float, 3>{1.F, 2.F, 3.F}));
    }
}
