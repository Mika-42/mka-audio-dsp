#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

import mka.audio.dsp.config;
import mka.audio.dsp.channel;
import mka.audio.dsp.process_context;
import mka.audio.dsp.node;
import mka.audio.dsp.dag;

namespace {
    using namespace mka::audio::dsp;

    static_assert(std::same_as<decltype(Config::layoutInput), std::vector<ChannelIndex>>);
    static_assert(std::same_as<decltype(Config::layoutAuxiliary), std::vector<ChannelIndex>>);
    static_assert(std::same_as<decltype(Config::layoutOutput), std::vector<ChannelIndex>>);
    static_assert(std::is_aggregate_v<Config>);
    static_assert(!std::is_polymorphic_v<Config> && !std::is_abstract_v<Config>);
    static_assert(!std::is_final_v<Config>);
    static_assert(std::is_copy_constructible_v<Config>);
    static_assert(!std::is_nothrow_copy_constructible_v<Config>);
    static_assert(maxLayoutChannels == 65536);
    static_assert(std::same_as<decltype(validateConfig(std::declval<const Config&>())),
                               std::expected<void, ErrorType>>);
    static_assert(noexcept(validateConfig(std::declval<const Config&>())));
    static_assert(std::same_as<decltype(std::declval<const Node&>().config()), const Config&>);
    static_assert(noexcept(std::declval<const Node&>().config()));

    class SnapshotNode final : public Node {
    public:
        using Node::Node;
        void process(const ProcessContext&) noexcept override {}
    };

    // Inheriting the constructor exposes the base exception specification:
    // copying an owning Config must not promise noexcept.
    static_assert(std::is_constructible_v<SnapshotNode, NodeID, const Config&>);
    static_assert(!std::is_nothrow_constructible_v<SnapshotNode, NodeID, const Config&>);

    class EmptyLayoutNode final : public Node {
    public:
        explicit EmptyLayoutNode(const NodeID id) : Node(id, Config{}) {}
        void process(const ProcessContext&) noexcept override {}
    };

    class SparseLayoutNode final : public Node {
    public:
        explicit SparseLayoutNode(const NodeID id)
            : Node(id, Config{.layoutInput = {0}, .layoutAuxiliary = {1}, .layoutOutput = {2}}) {}
        void process(const ProcessContext&) noexcept override {}
    };

    using LayoutMember = std::vector<ChannelIndex> Config::*;
    constexpr std::array<LayoutMember, 3> layouts{
        &Config::layoutInput, &Config::layoutAuxiliary, &Config::layoutOutput};

    void expectSameConfig(const Config& actual, const Config& expected) {
        EXPECT_EQ(actual.layoutInput, expected.layoutInput);
        EXPECT_EQ(actual.layoutAuxiliary, expected.layoutAuxiliary);
        EXPECT_EQ(actual.layoutOutput, expected.layoutOutput);
    }

    void expectError(const Result& result, const ErrorType error) {
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, error.code);
    }

    void expectDisconnect(const std::expected<bool, GraphError>& result, const bool removed) {
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(*result, removed);
    }

    std::vector<ChannelIndex> allChannelIndices() {
        std::vector<ChannelIndex> values;
        values.reserve(maxLayoutChannels);
        // The wide counter and checked domain bound prevent uint16_t wrapping.
        for (std::uint32_t value = 0; value < maxLayoutChannels; ++value) {
            values.push_back(static_cast<ChannelIndex>(value));
        }
        return values;
    }

    TEST(ConfigLayouts, DefaultsToThreeEmptyLayouts) {
        const Config config;
        EXPECT_TRUE(config.layoutInput.empty());
        EXPECT_TRUE(config.layoutAuxiliary.empty());
        EXPECT_TRUE(config.layoutOutput.empty());
        EXPECT_TRUE(validateConfig(config));
    }

    TEST(ConfigLayouts, AcceptsSparseUnsortedLayoutsAndPreservesBoundaryIndices) {
        const Config config{.layoutInput = {65535, 0, 7},
                            .layoutAuxiliary = {42, 0},
                            .layoutOutput = {3, 65535}};
        const auto before = config;
        ASSERT_TRUE(validateConfig(config));
        expectSameConfig(config, before);
    }

    TEST(ConfigLayouts, AllowsTheSameIndicesAcrossIndependentPorts) {
        const Config config{.layoutInput = {0, 65535},
                            .layoutAuxiliary = {0, 65535},
                            .layoutOutput = {0, 65535}};
        EXPECT_TRUE(validateConfig(config));
    }

    TEST(ConfigLayouts, RejectsAdjacentAndSeparatedDuplicatesInEachPortWithoutMutation) {
        const std::array<std::vector<ChannelIndex>, 3> duplicates{
            std::vector<ChannelIndex>{0, 0, 65535},
            std::vector<ChannelIndex>{0, 65535, 65535},
            std::vector<ChannelIndex>{0, 65535, 7, 0}};
        for (std::size_t port = 0; port < layouts.size(); ++port) {
            SCOPED_TRACE(port);
            for (const auto& invalid : duplicates) {
                Config config;
                config.*layouts[port] = invalid;
                const auto before = config;
                expectError(validateConfig(config), ErrorType::DuplicateChannelIndex);
                expectSameConfig(config, before);
            }
        }
    }

    TEST(ConfigLayouts, EachPortAcceptsAll65536DistinctIndicesWithoutACombinedLimit) {
        const auto all = allChannelIndices();
        ASSERT_EQ(all.size(), 65536U);
        ASSERT_EQ(all.front(), 0);
        ASSERT_EQ(all.back(), 65535);
        for (std::size_t port = 0; port < layouts.size(); ++port) {
            SCOPED_TRACE(port);
            Config config;
            config.*layouts[port] = all;
            ASSERT_TRUE(validateConfig(config));
            EXPECT_EQ(config.*layouts[port], all);
        }
        const Config full{.layoutInput = all, .layoutAuxiliary = all, .layoutOutput = all};
        EXPECT_TRUE(validateConfig(full));
    }

    TEST(ConfigLayouts, Rejects65537EntriesBeforeDuplicateChecksWithinEachPort) {
        for (std::size_t port = 0; port < layouts.size(); ++port) {
            SCOPED_TRACE(port);
            Config config;
            (config.*layouts[port]).assign(maxLayoutChannels + 1, ChannelIndex{0});
            const auto before = config;
            expectError(validateConfig(config), ErrorType::LayoutCapacityExceeded);
            expectSameConfig(config, before);
        }
    }

    TEST(ConfigLayouts, FullCapacityDoesNotHideARepeatedIndex) {
        const auto all = allChannelIndices();
        for (std::size_t port = 0; port < layouts.size(); ++port) {
            SCOPED_TRACE(port);
            Config config;
            auto& selected = config.*layouts[port];
            selected = all;
            selected.back() = selected.front();
            const auto before = config;
            expectError(validateConfig(config), ErrorType::DuplicateChannelIndex);
            expectSameConfig(config, before);
        }
    }

    TEST(ConfigLayouts, CopiesOwnIndependentVectorStorage) {
        const Config original{.layoutInput = {0, 1},
                              .layoutAuxiliary = {2},
                              .layoutOutput = {3, 65535}};
        Config copy = original;
        expectSameConfig(copy, original);
        EXPECT_NE(copy.layoutInput.data(), original.layoutInput.data());
        EXPECT_NE(copy.layoutAuxiliary.data(), original.layoutAuxiliary.data());
        EXPECT_NE(copy.layoutOutput.data(), original.layoutOutput.data());
        copy.layoutInput[0] = 65535;
        copy.layoutAuxiliary.push_back(4);
        copy.layoutOutput.clear();
        EXPECT_EQ(original.layoutInput, (std::vector<ChannelIndex>{0, 1}));
        EXPECT_EQ(original.layoutAuxiliary, (std::vector<ChannelIndex>{2}));
        EXPECT_EQ(original.layoutOutput, (std::vector<ChannelIndex>{3, 65535}));
    }

    TEST(ConfigNodeSnapshot, OwnsAReadOnlyCopyUnaffectedByCallerEdits) {
        Config caller{.layoutInput = {0, 65535},
                      .layoutAuxiliary = {7},
                      .layoutOutput = {1, 2}};
        const auto expected = caller;
        const SnapshotNode node{42, caller};
        const auto* snapshot = &node.config();
        EXPECT_NE(node.config().layoutInput.data(), caller.layoutInput.data());
        EXPECT_NE(node.config().layoutAuxiliary.data(), caller.layoutAuxiliary.data());
        EXPECT_NE(node.config().layoutOutput.data(), caller.layoutOutput.data());
        caller.layoutInput.clear();
        caller.layoutAuxiliary[0] = 8;
        caller.layoutOutput.push_back(3);
        expectSameConfig(node.config(), expected);
        EXPECT_EQ(&node.config(), snapshot);
        EXPECT_TRUE(validateConfig(node.config()));
    }

    TEST(ConfigNodeSnapshot, RemainsValidAfterTheCallerTemporaryIsDestroyed) {
        const SnapshotNode node{42, Config{.layoutInput = {65535},
                                          .layoutAuxiliary = {},
                                          .layoutOutput = {0}}};
        // The temporary Config above died at the end of its full expression.
        EXPECT_EQ(node.config().layoutInput, (std::vector<ChannelIndex>{65535}));
        EXPECT_TRUE(node.config().layoutAuxiliary.empty());
        EXPECT_EQ(node.config().layoutOutput, (std::vector<ChannelIndex>{0}));
        EXPECT_TRUE(validateConfig(node.config()));
    }

    TEST(ConfigGraphContract, EmptyLayoutsDoNotRejectLogicalConnections) {
        DAG graph;
        const auto addedSource = graph.add<EmptyLayoutNode>();
        ASSERT_TRUE(addedSource);
        const auto& source = *static_cast<EmptyLayoutNode*>(graph.node(*addedSource));
        const auto addedDestination = graph.add<EmptyLayoutNode>();
        ASSERT_TRUE(addedDestination);
        const auto& destination = *static_cast<EmptyLayoutNode*>(graph.node(*addedDestination));
        ASSERT_TRUE(validateConfig(source.config()));
        ASSERT_TRUE(validateConfig(destination.config()));
        ASSERT_TRUE(graph.connect(destination.channel(PortType::In, 65535),
                                  source.channel(PortType::Out, 65535)));
        expectDisconnect(graph.disconnect(destination.channel(PortType::In, 65535),
                                           source.channel(PortType::Out, 65535)), true);
        expectDisconnect(graph.disconnect(destination.channel(PortType::In, 65535),
                                           source.channel(PortType::Out, 65535)), false);
    }

    TEST(ConfigGraphContract, SparseLayoutsDoNotFilterUndeclaredMainOrAuxiliaryChannels) {
        DAG graph;
        const auto addedSource = graph.add<SparseLayoutNode>();
        ASSERT_TRUE(addedSource);
        const auto& source = *static_cast<SparseLayoutNode*>(graph.node(*addedSource));
        const auto addedDestination = graph.add<SparseLayoutNode>();
        ASSERT_TRUE(addedDestination);
        const auto& destination = *static_cast<SparseLayoutNode*>(graph.node(*addedDestination));
        ASSERT_TRUE(validateConfig(source.config()));
        ASSERT_TRUE(validateConfig(destination.config()));
        ASSERT_TRUE(graph.connect(destination.channel(PortType::In, 65535),
                                  source.channel(PortType::Out, 65535)));
        ASSERT_TRUE(graph.connect(destination.channel(PortType::Aux, 65535),
                                  source.channel(PortType::Out, 65535)));
        expectDisconnect(graph.disconnect(destination.channel(PortType::In, 65535),
                                           source.channel(PortType::Out, 65535)), true);
        expectDisconnect(graph.disconnect(destination.channel(PortType::Aux, 65535),
                                           source.channel(PortType::Out, 65535)), true);
        expectDisconnect(graph.disconnect(destination.channel(PortType::In, 65535),
                                           source.channel(PortType::Out, 65535)), false);
    }
}
