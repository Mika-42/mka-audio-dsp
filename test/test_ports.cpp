#include <array>
#include <concepts>
#include <cstdint>
#include <expected>
#include <limits>
#include <string>
#include <type_traits>
#include <utility>

#include <gtest/gtest.h>

import mka.audio.dsp.types;
import mka.audio.dsp.channel;
import mka.audio.dsp.process_context;
import mka.audio.dsp.dag;

namespace {
    using namespace mka::audio::dsp;

    static_assert(std::is_scoped_enum_v<PortType>);
    static_assert(std::same_as<std::underlying_type_t<PortType>, std::uint16_t>);
    static_assert(sizeof(PortType) == 2);
    static_assert(static_cast<std::uint16_t>(PortType::In) == 0);
    static_assert(static_cast<std::uint16_t>(PortType::Out) == 1);
    static_assert(static_cast<std::uint16_t>(PortType::Aux) == 2);
    static_assert(!std::is_convertible_v<PortType, std::uint16_t>);
    static_assert(std::same_as<decltype(&isInputPort), bool (*)(PortType) noexcept>);
    static_assert(std::same_as<decltype(&isOutputPort), bool (*)(PortType) noexcept>);
    static_assert(std::same_as<decltype(&isValidPort), bool (*)(PortType) noexcept>);
    static_assert(isValidPort(PortType::In) && isValidPort(PortType::Out) &&
                  isValidPort(PortType::Aux));
    static_assert(isInputPort(PortType::In) && isInputPort(PortType::Aux));
    static_assert(!isInputPort(PortType::Out));
    static_assert(isOutputPort(PortType::Out));
    static_assert(!isOutputPort(PortType::In) && !isOutputPort(PortType::Aux));
    static_assert(!isValidPort(static_cast<PortType>(3)));
    static_assert(!isValidPort(static_cast<PortType>(65535)));
    static_assert(!isInputPort(static_cast<PortType>(3)));
    static_assert(!isOutputPort(static_cast<PortType>(65535)));
    static_assert(std::same_as<GraphError, ErrorType>);
    static_assert(std::same_as<Result, std::expected<void, GraphError>>);
    static_assert(std::same_as<
        decltype(std::declval<DAG&>().connect(std::declval<const Channel&>(),
                                            std::declval<const Channel&>())),
        std::expected<void, GraphError>>);

    class PortTestNode final : public Node {
    public:
        explicit PortTestNode(const NodeID id)
            : Node(id, {.layoutInput = {0, 1},
                        .layoutAuxiliary = {},
                        .layoutOutput = {0, 1}}) {}

        void process(const ProcessContext&) noexcept override {}
    };

    void expectError(const std::expected<void, GraphError>& result, const GraphError error) {
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error(), error);
    }

    void expectDisconnect(const std::expected<bool, GraphError>& result, const bool removed) {
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(*result, removed);
    }

    struct PortPair {
        PortType source;
        PortType destination;
        bool allowed;
    };

    // Independent, explicit oracle for all nine official source/destination pairs.
    constexpr std::array directionCases{
        PortPair{PortType::In,  PortType::In,  false},
        PortPair{PortType::In,  PortType::Out, false},
        PortPair{PortType::In,  PortType::Aux, false},
        PortPair{PortType::Out, PortType::In,  true},
        PortPair{PortType::Out, PortType::Out, false},
        PortPair{PortType::Out, PortType::Aux, true},
        PortPair{PortType::Aux, PortType::In,  false},
        PortPair{PortType::Aux, PortType::Out, false},
        PortPair{PortType::Aux, PortType::Aux, false},
    };
    constexpr ChannelIndex index = 0;

    // Malformed IDs must be constructed without calling Node::channel/encode
    // outside their valid-port precondition.
    constexpr ChannelID rawChannelID(const NodeID node, const PortType port) noexcept {
        return (ChannelID{node} << 32U) |
               (ChannelID{static_cast<std::uint16_t>(port)} << 16U);
    }

    TEST(PortTypeContract, HasFixedSixteenBitCodes) {
        EXPECT_EQ(sizeof(PortType), sizeof(std::uint16_t));
        EXPECT_EQ(static_cast<std::uint16_t>(PortType::In), 0);
        EXPECT_EQ(static_cast<std::uint16_t>(PortType::Out), 1);
        EXPECT_EQ(static_cast<std::uint16_t>(PortType::Aux), 2);
        EXPECT_EQ(std::numeric_limits<std::underlying_type_t<PortType>>::digits, 16);
    }

    TEST(PortTypeContract, ClassifiesMainAndAuxiliaryInputsAndOutput) {
        EXPECT_TRUE(isInputPort(PortType::In));
        EXPECT_TRUE(isInputPort(PortType::Aux));
        EXPECT_FALSE(isInputPort(PortType::Out));
        EXPECT_TRUE(isOutputPort(PortType::Out));
        EXPECT_FALSE(isOutputPort(PortType::In));
        EXPECT_FALSE(isOutputPort(PortType::Aux));
    }

    TEST(PortTypeContract, HelpersClassifyEverySixteenBitRepresentation) {
        // Use a wider counter so the loop terminates instead of wrapping.
        for (std::uint32_t code = 0; code <= 65535; ++code) {
            const auto port = static_cast<PortType>(code);
            ASSERT_EQ(isValidPort(port), code <= 2) << code;
            ASSERT_EQ(isInputPort(port), code == 0 || code == 2) << code;
            ASSERT_EQ(isOutputPort(port), code == 1) << code;
        }
    }

    class PortDirectionMatrix : public ::testing::TestWithParam<PortPair> {};

    TEST_P(PortDirectionMatrix, AcceptsOnlyOutToInOrAuxWithoutPhantomEdges) {
        DAG graph;
        const auto addedSource = graph.add<PortTestNode>();
        ASSERT_TRUE(addedSource);
        const auto& source = *static_cast<PortTestNode*>(graph.node(*addedSource));
        const auto addedDestination = graph.add<PortTestNode>();
        ASSERT_TRUE(addedDestination);
        const auto& destination = *static_cast<PortTestNode*>(graph.node(*addedDestination));
        const auto pair = GetParam();
        const auto result = graph.connect(destination.channel(pair.destination, index),
                                          source.channel(pair.source, index));

        if (pair.allowed) {
            ASSERT_TRUE(result.has_value());
            const auto duplicate = graph.connect(destination.channel(pair.destination, index),
                                                  source.channel(pair.source, index));
            ASSERT_FALSE(duplicate.has_value());
            EXPECT_EQ(duplicate.error(), GraphError::ConnectionExists);
        } else {
            ASSERT_FALSE(result.has_value());
            EXPECT_EQ(result.error(), GraphError::InvalidDirection);
            expectDisconnect(graph.disconnect(destination.channel(PortType::In, index),
                                               source.channel(PortType::Out, index)), false);
            // Rejection must not leave any edge, reverse path or occupied input.
            ASSERT_TRUE(graph.connect(destination.channel(PortType::In, index),
                                      source.channel(PortType::Out, index)));
        }

        const auto connectedDestination = destination.channel(pair.allowed ? pair.destination : PortType::In, index);
        const auto connectedSource = source.channel(PortType::Out, index);
        expectDisconnect(graph.disconnect(connectedDestination, connectedSource), true);
        expectDisconnect(graph.disconnect(connectedDestination, connectedSource), false);
    }

    const char* portName(const PortType port) {
        switch (port) {
            case PortType::In: return "In";
            case PortType::Out: return "Out";
            case PortType::Aux: return "Aux";
        }
        return "Invalid";
    }

    INSTANTIATE_TEST_SUITE_P(
        OfficialPairs, PortDirectionMatrix, ::testing::ValuesIn(directionCases),
        [](const ::testing::TestParamInfo<PortPair>& info) {
            return std::string{portName(info.param.source)} + "To" +
                   portName(info.param.destination);
        });

    TEST(PortDirections, RejectsUndefinedCodesInEitherPosition) {
        constexpr std::array<std::uint16_t, 6> codes{3, 4, 255, 256, 32768, 65535};
        for (const auto code : codes) {
            SCOPED_TRACE(code);
            const auto invalid = static_cast<PortType>(code);
            for (const auto pair : std::array{
                     PortPair{invalid, PortType::In, false},
                     PortPair{invalid, PortType::Aux, false},
                     PortPair{PortType::Out, invalid, false},
                     PortPair{invalid, invalid, false}}) {
                DAG graph;
                const auto addedSource = graph.add<PortTestNode>();
                ASSERT_TRUE(addedSource);
                const auto& source = *static_cast<PortTestNode*>(graph.node(*addedSource));
                const auto addedDestination = graph.add<PortTestNode>();
                ASSERT_TRUE(addedDestination);
                const auto& destination = *static_cast<PortTestNode*>(graph.node(*addedDestination));
                const auto result = graph.connect(rawChannelID(destination.id(), pair.destination),
                                                  rawChannelID(source.id(), pair.source));
                ASSERT_FALSE(result.has_value());
                EXPECT_EQ(result.error(), GraphError::InvalidPort);
                expectDisconnect(graph.disconnect(destination.channel(PortType::In, index),
                                                   source.channel(PortType::Out, index)), false);
                ASSERT_TRUE(graph.connect(destination.channel(PortType::In, index),
                                          source.channel(PortType::Out, index)));
                expectDisconnect(graph.disconnect(destination.channel(PortType::In, index),
                                                   source.channel(PortType::Out, index)), true);
            }
        }
    }

    TEST(PortDirections, RejectionsPreserveAnExistingEdgeAndDoNotAddAnother) {
        DAG graph;
        const auto addedA = graph.add<PortTestNode>();
        ASSERT_TRUE(addedA);
        const auto& a = *static_cast<PortTestNode*>(graph.node(*addedA));
        const auto addedB = graph.add<PortTestNode>();
        ASSERT_TRUE(addedB);
        const auto& b = *static_cast<PortTestNode*>(graph.node(*addedB));
        const auto addedC = graph.add<PortTestNode>();
        ASSERT_TRUE(addedC);
        const auto& c = *static_cast<PortTestNode*>(graph.node(*addedC));
        const auto addedD = graph.add<PortTestNode>();
        ASSERT_TRUE(addedD);
        const auto& d = *static_cast<PortTestNode*>(graph.node(*addedD));
        const auto originalFrom = a.channel(PortType::Out, index);
        const auto originalTo = b.channel(PortType::In, index);
        ASSERT_TRUE(graph.connect(originalTo, originalFrom));

        constexpr std::array ports{PortType::In, PortType::Out, PortType::Aux,
                                  static_cast<PortType>(3), static_cast<PortType>(65535)};
        for (const auto from : ports) {
            for (const auto to : ports) {
                if (from == PortType::Out && (to == PortType::In || to == PortType::Aux)) {
                    continue;
                }
                SCOPED_TRACE(static_cast<std::uint16_t>(from));
                SCOPED_TRACE(static_cast<std::uint16_t>(to));
                const auto rejected = graph.connect(rawChannelID(d.id(), to), rawChannelID(c.id(), from));
                ASSERT_FALSE(rejected.has_value());
                EXPECT_EQ(rejected.error(),
                          !isValidPort(from) || !isValidPort(to)
                              ? GraphError::InvalidPort : GraphError::InvalidDirection);
                const auto existing = graph.connect(originalTo, originalFrom);
                ASSERT_FALSE(existing.has_value());
                EXPECT_EQ(existing.error(), GraphError::ConnectionExists);
                expectDisconnect(graph.disconnect(d.channel(PortType::In, index),
                                                   c.channel(PortType::Out, index)), false);
            }
        }

        expectDisconnect(graph.disconnect(originalTo, originalFrom), true);
        expectDisconnect(graph.disconnect(originalTo, originalFrom), false);
        ASSERT_TRUE(graph.connect(d.channel(PortType::Aux, index), c.channel(PortType::Out, index)));
        expectDisconnect(graph.disconnect(d.channel(PortType::Aux, index),
                                           c.channel(PortType::Out, index)), true);
    }

    TEST(PortDirections, PortAndDirectionErrorsTakePriorityOverOtherGraphErrors) {
        DAG graph;
        const auto addedA = graph.add<PortTestNode>();
        ASSERT_TRUE(addedA);
        const auto& a = *static_cast<PortTestNode*>(graph.node(*addedA));
        const auto addedB = graph.add<PortTestNode>();
        ASSERT_TRUE(addedB);
        const auto& b = *static_cast<PortTestNode*>(graph.node(*addedB));
        ASSERT_TRUE(graph.connect(b.channel(PortType::In, index), a.channel(PortType::Out, index)));

        // Direction is checked before destination occupancy.
        const auto occupied = graph.connect(b.channel(PortType::In, index),
                                            a.channel(PortType::In, index));
        ASSERT_FALSE(occupied.has_value());
        EXPECT_EQ(occupied.error(), GraphError::InvalidDirection);
        // Self-loop detection must not mask an invalid destination direction.
        const auto self = graph.connect(a.channel(PortType::Out, index),
                                        a.channel(PortType::Out, index));
        ASSERT_FALSE(self.has_value());
        EXPECT_EQ(self.error(), GraphError::InvalidDirection);
        const auto cyclic = graph.connect(a.channel(PortType::In, index),
                                          b.channel(PortType::In, index));
        ASSERT_FALSE(cyclic.has_value());
        EXPECT_EQ(cyclic.error(), GraphError::InvalidDirection);
        const auto missing = graph.connect(
            Channel{.nodeID = invalidNodeID, .channel = index, .port = PortType::Out},
            Channel{.nodeID = maxNodeID, .channel = index, .port = static_cast<PortType>(3)});
        ASSERT_FALSE(missing.has_value());
        EXPECT_EQ(missing.error(), GraphError::InvalidPort);

        expectDisconnect(graph.disconnect(b.channel(PortType::In, index),
                                           a.channel(PortType::Out, index)), true);
        expectDisconnect(graph.disconnect(a.channel(PortType::In, index),
                                           b.channel(PortType::Out, index)), false);
    }

    TEST(PortDirections, ValidDirectionsReturnDistinctStructuralErrors) {
        DAG graph;
        const auto addedA = graph.add<PortTestNode>();
        ASSERT_TRUE(addedA);
        const auto& a = *static_cast<PortTestNode*>(graph.node(*addedA));
        const auto addedB = graph.add<PortTestNode>();
        ASSERT_TRUE(addedB);
        const auto& b = *static_cast<PortTestNode*>(graph.node(*addedB));
        const auto addedC = graph.add<PortTestNode>();
        ASSERT_TRUE(addedC);
        const auto& c = *static_cast<PortTestNode*>(graph.node(*addedC));
        const auto aOut = a.channel(PortType::Out, index);
        const auto bIn = b.channel(PortType::In, index);
        const auto unknownOut = encode(maxNodeID, PortType::Out, index);
        const auto unknownIn = encode(maxNodeID, PortType::In, index);
        expectError(graph.connect(bIn, unknownOut), GraphError::NodeNotFound);
        expectError(graph.connect(unknownIn, aOut), GraphError::NodeNotFound);
        expectError(graph.connect(a.channel(PortType::In, index), aOut), GraphError::CycleDetected);
        ASSERT_TRUE(graph.connect(bIn, aOut));
        expectError(graph.connect(bIn, aOut), GraphError::ConnectionExists);
        expectError(graph.connect(bIn, c.channel(PortType::Out, index)),
                    GraphError::InputOccupied);
        expectError(graph.connect(a.channel(PortType::In, index), b.channel(PortType::Out, index)),
                    GraphError::CycleDetected);
        // Aux and In are distinct destinations even at the same channel index.
        ASSERT_TRUE(graph.connect(b.channel(PortType::Aux, index), aOut));
        expectDisconnect(graph.disconnect(bIn, aOut), true);
        expectDisconnect(graph.disconnect(b.channel(PortType::Aux, index), aOut), true);
        expectDisconnect(graph.disconnect(bIn, aOut), false);
    }
}
