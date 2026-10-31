#include <array>
#include <concepts>
#include <cstdint>
#include <limits>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

// Node's public interface must expose the referenced value and context types.
import mka.audio.dsp.node;

namespace {
    using namespace mka::audio::dsp;

    static_assert(std::is_abstract_v<Node> && !std::is_final_v<Node>);
    static_assert(std::has_virtual_destructor_v<Node> && std::is_destructible_v<Node>);
    static_assert(std::same_as<decltype(&Node::process),
                               void (Node::*)(const ProcessContext&) noexcept>);
    static_assert(std::same_as<decltype(&Node::latencySamples),
                               std::uint32_t (Node::*)() const noexcept>);
    static_assert(std::same_as<decltype(&Node::id), NodeID (Node::*)() const noexcept>);
    static_assert(std::same_as<decltype(&Node::channel),
                               ChannelID (Node::*)(PortType, ChannelIndex) const noexcept>);
    static_assert(std::same_as<decltype(&Node::config), const Config& (Node::*)() const noexcept>);
    static_assert(std::same_as<Node::Config, Config>);

    template<typename T>
    concept HasPrepareMember = requires { &T::prepare; };
    static_assert(!HasPrepareMember<Node>);

    class ProcessOnlyNode final : public Node {
    public:
        using Node::Node;
        void process(const ProcessContext& context) noexcept override {
            ++calls;
            lastFrames = context.frames;
        }
        unsigned calls = 0;
        FrameCount lastFrames = 0;
    };

    class LatencyOnlyNode : public Node {
    public:
        using Node::Node;
        std::uint32_t latencySamples() const noexcept override { return 128; }
    };

    static_assert(std::is_abstract_v<LatencyOnlyNode>); // process is still pure.
    static_assert(!std::is_abstract_v<ProcessOnlyNode>); // latency is not pure.
    static_assert(std::is_final_v<ProcessOnlyNode>);
    static_assert(!std::is_copy_constructible_v<ProcessOnlyNode>);
    static_assert(!std::is_copy_assignable_v<ProcessOnlyNode>);
    static_assert(!std::is_move_constructible_v<ProcessOnlyNode>);
    static_assert(!std::is_move_assignable_v<ProcessOnlyNode>);
    static_assert(std::is_nothrow_destructible_v<ProcessOnlyNode>);
    static_assert(!std::is_nothrow_constructible_v<ProcessOnlyNode, NodeID, const Config&>);

    class DeclaredLatencyNode final : public Node {
    public:
        DeclaredLatencyNode(const NodeID id, const std::uint32_t latency)
            : Node(id, Config{}), latency_(latency) {}
        void process(const ProcessContext&) noexcept override {}
        std::uint32_t latencySamples() const noexcept override { return latency_; }
    private:
        const std::uint32_t latency_;
    };

    class HidingAccessorsNode final : public Node {
    public:
        HidingAccessorsNode() : Node(11, Config{.layoutInput = {0}, .layoutOutput = {1}}) {}
        void process(const ProcessContext&) noexcept override {}
        NodeID id() const noexcept { return 99; }
        ChannelID channel(PortType, ChannelIndex) const noexcept { return 123; }
        const Config& config() const noexcept { return alternate_; }
    private:
        Config alternate_{.layoutInput = {9}, .layoutOutput = {10}};
    };

    // All retained state is scalar. Callback descriptor/sample pointers are
    // consumed synchronously and are deliberately not fields of this effect.
    class ScalarGainNode final : public Node {
    public:
        explicit ScalarGainNode(const NodeID id) : Node(id, Config{}) {}
        void process(const ProcessContext& context) noexcept override {
            ++calls;
            lastTotal = 0.F;
            if (context.inputs.empty()) return;
            for (FrameCount frame = 0; frame < context.frames; ++frame) {
                const auto value = context.inputs[0].buffer[frame] * 2.F;
                if (!context.outputs.empty()) context.outputs[0].buffer[frame] = value;
                if (!context.backendOutputs.empty()) context.backendOutputs[0].buffer[frame] = value + 1.F;
                lastTotal += value;
            }
        }
        unsigned calls = 0;
        float lastTotal = 0.F;
    };

    TEST(NodeInterface, OrdinaryAccessorsPreserveIdentityAndReadOnlyConfiguration) {
        const Config config{.layoutInput = {2, 6}, .layoutAuxiliary = {7}, .layoutOutput = {0}};
        const ProcessOnlyNode node{42, config};
        const Node& base = node;
        EXPECT_EQ(base.id(), 42U);
        EXPECT_EQ(base.config().layoutInput, config.layoutInput);
        EXPECT_EQ(base.config().layoutAuxiliary, config.layoutAuxiliary);
        EXPECT_EQ(base.config().layoutOutput, config.layoutOutput);
        EXPECT_EQ(&base.config(), &base.config());
    }

    TEST(NodeInterface, ChannelReturnsTheSpecifiedNumericGoldenIdentity) {
        const ProcessOnlyNode node{0x12345678U, Config{}};
        EXPECT_EQ(node.channel(PortType::Out, 0xABCD), 0x123456780001ABCDULL);
        EXPECT_EQ(node.channel(PortType::In, 0), 0x1234567800000000ULL);
        EXPECT_EQ(node.channel(PortType::Aux, 65535), 0x123456780002FFFFULL);
    }

    TEST(NodeInterface, ValidPortsAndBoundaryIndicesPreserveTheExactNodeIdentity) {
        constexpr std::array<NodeID, 3> identities{1, 0x80000000U, UINT32_MAX};
        constexpr std::array ports{PortType::In, PortType::Out, PortType::Aux};
        constexpr std::array<ChannelIndex, 2> indices{0, 65535};
        for (const auto identity : identities) {
            const ProcessOnlyNode node{identity, Config{}};
            EXPECT_EQ(node.id(), identity);
            for (const auto port : ports) {
                for (const auto index : indices) {
                    const auto id = node.channel(port, index);
                    EXPECT_EQ(id, encode(identity, port, index));
                    EXPECT_EQ(decodeNode(id), identity);
                    EXPECT_EQ(decodePort(id), port);
                    EXPECT_EQ(decodeChannel(id), index);
                }
            }
        }
    }

    TEST(NodeInterface, DefaultLatencyIsExactlyZeroWithoutAnOverride) {
        const ProcessOnlyNode node{1, Config{}};
        const Node& base = node;
        EXPECT_EQ(base.latencySamples(), 0U);
        EXPECT_EQ(node.latencySamples(), 0U);
    }

    TEST(NodeInterface, VirtualLatencyOverrideDispatchesThroughTheBaseInterface) {
        const DeclaredLatencyNode delayed{1, 128};
        const DeclaredLatencyNode maximum{2, std::numeric_limits<std::uint32_t>::max()};
        const Node& delayedBase = delayed;
        const Node& maximumBase = maximum;
        EXPECT_EQ(delayedBase.latencySamples(), 128U);
        EXPECT_EQ(maximumBase.latencySamples(), UINT32_MAX);
    }

    TEST(NodeInterface, IdentityChannelAndConfigAccessorsAreNonVirtual) {
        const HidingAccessorsNode node;
        const Node& base = node;
        EXPECT_EQ(node.id(), 99U);
        EXPECT_EQ(base.id(), 11U);
        EXPECT_EQ(node.channel(PortType::Out, 0), 123U);
        EXPECT_EQ(base.channel(PortType::Out, 0), 0x0000000B00010000ULL);
        EXPECT_EQ(node.config().layoutInput, (std::vector<ChannelIndex>{9}));
        EXPECT_EQ(base.config().layoutInput, (std::vector<ChannelIndex>{0}));
        EXPECT_NE(&node.config(), &base.config());
    }

    TEST(NodeInterface, ConfigurationRemainsOwnedAfterCallerStorageChanges) {
        Config caller{.layoutInput = {0, 65535}, .layoutOutput = {1}};
        const ProcessOnlyNode node{1, caller};
        EXPECT_NE(node.config().layoutInput.data(), caller.layoutInput.data());
        caller.layoutInput.clear();
        caller.layoutOutput[0] = 2;
        EXPECT_EQ(node.config().layoutInput, (std::vector<ChannelIndex>{0, 65535}));
        EXPECT_EQ(node.config().layoutOutput, (std::vector<ChannelIndex>{1}));
    }

    TEST(NodeProcessingContract, VirtualProcessUsesFreshBackendViewsWithoutRetainingTheirPointers) {
        ScalarGainNode effect{1};
        Node& base = effect;
        std::array<float, 4> logicalOutput{};
        const std::array logicalDescriptors{
            OutputChannel{effect.channel(PortType::Out, 0), logicalOutput.data()}};
        const PreparedContextBindings bindings{.outputs = logicalDescriptors, .maxFrames = 4,
            .maxBackendInputChannels = 1, .maxBackendOutputChannels = 1,
            .isSource = true, .isOutput = true};
        const std::array<float, 4> firstInput{1.F, -2.F, 3.F, -4.F};
        std::array<float, 4> firstOutput{};
        {
            const std::array inputs{InputChannel{0, firstInput.data()}};
            const std::array outputs{OutputChannel{0, firstOutput.data()}};
            ASSERT_TRUE(withProcessContext(bindings, inputs, outputs, 4,
                [&](const ProcessContext& context) noexcept { base.process(context); }));
        }
        EXPECT_EQ(logicalOutput, (std::array<float, 4>{2.F, -4.F, 6.F, -8.F}));
        EXPECT_EQ(firstOutput, (std::array<float, 4>{3.F, -3.F, 7.F, -7.F}));
        EXPECT_FLOAT_EQ(effect.lastTotal, -4.F);
        const auto firstAfterCallback = firstOutput;
        {
            const std::array<float, 4> secondInput{5.F, 6.F, 7.F, 8.F};
            std::array<float, 4> secondOutput{};
            const std::array inputs{InputChannel{0, secondInput.data()}};
            const std::array outputs{OutputChannel{0, secondOutput.data()}};
            ASSERT_TRUE(withProcessContext(bindings, inputs, outputs, 4,
                [&](const ProcessContext& context) noexcept { base.process(context); }));
            EXPECT_EQ(secondOutput, (std::array<float, 4>{11.F, 13.F, 15.F, 17.F}));
            EXPECT_EQ(firstOutput, firstAfterCallback);
        }
        EXPECT_EQ(logicalOutput, (std::array<float, 4>{10.F, 12.F, 14.F, 16.F}));
        EXPECT_EQ(firstOutput, firstAfterCallback);
        EXPECT_EQ(effect.calls, 2U);
        EXPECT_FLOAT_EQ(effect.lastTotal, 52.F);
    }

    TEST(NodeProcessingContract, ZeroAndInvalidBlocksDoNotInvokeTheEffect) {
        ScalarGainNode effect{1};
        const std::array<float, 2> input{2.F, 3.F};
        std::array<float, 2> output{11.F, 13.F};
        const std::array inputs{InputChannel{0, input.data()}};
        const std::array outputs{OutputChannel{0, output.data()}};
        const PreparedContextBindings bindings{.maxFrames = 2,
            .maxBackendInputChannels = 1, .maxBackendOutputChannels = 1,
            .isSource = true, .isOutput = true};
        const auto consume = [&](const ProcessContext& context) noexcept { effect.process(context); };
        EXPECT_TRUE(withProcessContext(bindings, inputs, outputs, 0, consume));
        EXPECT_FALSE(withProcessContext(bindings, inputs, outputs, 3, consume));
        const std::array nullInput{InputChannel{}};
        EXPECT_FALSE(withProcessContext(bindings, nullInput, outputs, 2, consume));
        EXPECT_EQ(effect.calls, 0U);
        EXPECT_EQ(output, (std::array<float, 2>{11.F, 13.F}));
    }
}
