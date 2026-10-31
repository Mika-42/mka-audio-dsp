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
        const auto& source = graph.add<PortTestNode>();
        const auto& destination = graph.add<PortTestNode>();
        const auto pair = GetParam();
        const auto result = graph.connect(source.channel(pair.source, index),
                                          destination.channel(pair.destination, index));

        if (pair.allowed) {
            ASSERT_TRUE(result.has_value());
            const auto duplicate = graph.connect(source.channel(pair.source, index),
                                                  destination.channel(pair.destination, index));
            ASSERT_FALSE(duplicate.has_value());
            EXPECT_EQ(duplicate.error(), GraphError::ConnexionAlreadyExists);
        } else {
            ASSERT_FALSE(result.has_value());
            EXPECT_EQ(result.error(), GraphError::InvalidDirection);
            const auto absent = graph.disconnect(source.id(), destination.id());
            ASSERT_FALSE(absent.has_value());
            EXPECT_EQ(absent.error(), GraphError::ConnexionNotFound);
            // Rejection must not leave any edge, reverse path or occupied input.
            ASSERT_TRUE(graph.connect(source.channel(PortType::Out, index),
                                      destination.channel(PortType::In, index)));
        }

        ASSERT_TRUE(graph.disconnect(source.id(), destination.id()));
        const auto removed = graph.disconnect(source.id(), destination.id());
        ASSERT_FALSE(removed.has_value());
        EXPECT_EQ(removed.error(), GraphError::ConnexionNotFound);
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
                const auto& source = graph.add<PortTestNode>();
                const auto& destination = graph.add<PortTestNode>();
                const auto result = graph.connect(rawChannelID(source.id(), pair.source),
                                                  rawChannelID(destination.id(), pair.destination));
                ASSERT_FALSE(result.has_value());
                EXPECT_EQ(result.error(), GraphError::InvalidDirection);
                const auto absent = graph.disconnect(source.id(), destination.id());
                ASSERT_FALSE(absent.has_value());
                EXPECT_EQ(absent.error(), GraphError::ConnexionNotFound);
                ASSERT_TRUE(graph.connect(source.channel(PortType::Out, index),
                                          destination.channel(PortType::In, index)));
                ASSERT_TRUE(graph.disconnect(source.id(), destination.id()));
            }
        }
    }

    TEST(PortDirections, RejectionsPreserveAnExistingEdgeAndDoNotAddAnother) {
        DAG graph;
        const auto& a = graph.add<PortTestNode>();
        const auto& b = graph.add<PortTestNode>();
        const auto& c = graph.add<PortTestNode>();
        const auto& d = graph.add<PortTestNode>();
        const auto originalFrom = a.channel(PortType::Out, index);
        const auto originalTo = b.channel(PortType::In, index);
        ASSERT_TRUE(graph.connect(originalFrom, originalTo));

        constexpr std::array ports{PortType::In, PortType::Out, PortType::Aux,
                                  static_cast<PortType>(3), static_cast<PortType>(65535)};
        for (const auto from : ports) {
            for (const auto to : ports) {
                if (from == PortType::Out && (to == PortType::In || to == PortType::Aux)) {
                    continue;
                }
                SCOPED_TRACE(static_cast<std::uint16_t>(from));
                SCOPED_TRACE(static_cast<std::uint16_t>(to));
                const auto rejected = graph.connect(rawChannelID(c.id(), from), rawChannelID(d.id(), to));
                ASSERT_FALSE(rejected.has_value());
                EXPECT_EQ(rejected.error(), GraphError::InvalidDirection);
                const auto existing = graph.connect(originalFrom, originalTo);
                ASSERT_FALSE(existing.has_value());
                EXPECT_EQ(existing.error(), GraphError::ConnexionAlreadyExists);
                const auto absent = graph.disconnect(c.id(), d.id());
                ASSERT_FALSE(absent.has_value());
                EXPECT_EQ(absent.error(), GraphError::ConnexionNotFound);
            }
        }

        ASSERT_TRUE(graph.disconnect(a.id(), b.id()));
        expectError(graph.disconnect(a.id(), b.id()), GraphError::ConnexionNotFound);
        ASSERT_TRUE(graph.connect(c.channel(PortType::Out, index), d.channel(PortType::Aux, index)));
        ASSERT_TRUE(graph.disconnect(c.id(), d.id()));
    }

    TEST(PortDirections, InvalidDirectionTakesPriorityOverOtherGraphErrors) {
        DAG graph;
        const auto& a = graph.add<PortTestNode>();
        const auto& b = graph.add<PortTestNode>();
        ASSERT_TRUE(graph.connect(a.channel(PortType::Out, index), b.channel(PortType::In, index)));

        // Occupancy would previously mask the invalid source direction.
        const auto occupied = graph.connect(a.channel(PortType::In, index),
                                            b.channel(PortType::In, index));
        ASSERT_FALSE(occupied.has_value());
        EXPECT_EQ(occupied.error(), GraphError::InvalidDirection);
        // Self-loop detection must not mask an invalid destination direction.
        const auto self = graph.connect(a.channel(PortType::Out, index),
                                        a.channel(PortType::Out, index));
        ASSERT_FALSE(self.has_value());
        EXPECT_EQ(self.error(), GraphError::InvalidDirection);
        const auto cyclic = graph.connect(b.channel(PortType::In, index),
                                          a.channel(PortType::In, index));
        ASSERT_FALSE(cyclic.has_value());
        EXPECT_EQ(cyclic.error(), GraphError::InvalidDirection);
        const auto missing = graph.connect(
            Channel{.nodeID = maxNodeID, .channel = index, .port = static_cast<PortType>(3)},
            Channel{.nodeID = invalidNodeID, .channel = index, .port = PortType::Out});
        ASSERT_FALSE(missing.has_value());
        EXPECT_EQ(missing.error(), GraphError::InvalidDirection);

        ASSERT_TRUE(graph.disconnect(a.id(), b.id()));
        const auto absent = graph.disconnect(b.id(), a.id());
        ASSERT_FALSE(absent.has_value());
        EXPECT_EQ(absent.error(), GraphError::ConnexionNotFound);
    }

    TEST(PortDirections, ValidDirectionsPreserveExistingGraphErrorChecks) {
        DAG graph;
        const auto& a = graph.add<PortTestNode>();
        const auto& b = graph.add<PortTestNode>();
        const auto& c = graph.add<PortTestNode>();
        const auto aOut = a.channel(PortType::Out, index);
        const auto bIn = b.channel(PortType::In, index);
        const auto unknownOut = encode(maxNodeID, PortType::Out, index);
        const auto unknownIn = encode(maxNodeID, PortType::In, index);
        expectError(graph.connect(unknownOut, bIn), GraphError::NodeNotFound);
        expectError(graph.connect(aOut, unknownIn), GraphError::NodeNotFound);
        expectError(graph.connect(aOut, a.channel(PortType::In, index)), GraphError::CycleDetected);
        ASSERT_TRUE(graph.connect(aOut, bIn));
        expectError(graph.connect(aOut, bIn), GraphError::ConnexionAlreadyExists);
        expectError(graph.connect(c.channel(PortType::Out, index), bIn),
                    GraphError::ChannelAlreadyConnected);
        expectError(graph.connect(b.channel(PortType::Out, index), a.channel(PortType::In, index)),
                    GraphError::CycleDetected);
        // Aux and In are distinct destinations even at the same channel index.
        ASSERT_TRUE(graph.connect(aOut, b.channel(PortType::Aux, index)));
        ASSERT_TRUE(graph.disconnect(a.id(), b.id()));
        ASSERT_TRUE(graph.disconnect(a.id(), b.id()));
        expectError(graph.disconnect(a.id(), b.id()), GraphError::ConnexionNotFound);
    }
}
