#include <array>
#include <concepts>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <unordered_set>

#include <gtest/gtest.h>

// A single import also verifies that the public channel API exports its aliases.
import mka.audio.dsp.channel;

namespace {
    using namespace mka::audio::dsp;

    using EncodeFunction = ChannelID (*)(NodeID, PortType, ChannelIndex) noexcept;
    static_assert(std::same_as<decltype(&encode), EncodeFunction>);
    static_assert(std::same_as<decltype(&decodeNode), NodeID (*)(ChannelID) noexcept>);
    static_assert(std::same_as<decltype(&decodePort), PortType (*)(ChannelID) noexcept>);
    static_assert(std::same_as<decltype(&decodeChannel), ChannelIndex (*)(ChannelID) noexcept>);
    static_assert(encode(0x12345678U, PortType::Out, 0xABCDU) == 0x123456780001ABCDULL);
    static_assert(decodeNode(0x123456780001ABCDULL) == 0x12345678U);
    static_assert(decodePort(0x123456780001ABCDULL) == PortType::Out);
    static_assert(decodeChannel(0x123456780001ABCDULL) == 0xABCDU);
    static_assert(encode(0, PortType::In, 0) == 0);
    static_assert(encode(UINT32_MAX, PortType::Aux, UINT16_MAX) == 0xFFFFFFFF0002FFFFULL);
    static_assert(decodeNode(UINT64_MAX) == UINT32_MAX);
    static_assert(decodeChannel(UINT64_MAX) == UINT16_MAX);
    static_assert(static_cast<std::uint16_t>(decodePort(UINT64_MAX)) == UINT16_MAX);
    static_assert(!isValidPort(decodePort(UINT64_MAX)));
    static_assert(!isValidPort(decodePort(0x123456780003ABCDULL)));

    TEST(ChannelIDCodec, MatchesIndependentNumericGoldenValues) {
        EXPECT_EQ(encode(0x12345678U, PortType::Out, 0xABCDU), 0x123456780001ABCDULL);
        EXPECT_EQ(encode(0, PortType::In, 65535), 0x000000000000FFFFULL);
        EXPECT_EQ(encode(0, PortType::Out, 0), 0x0000000000010000ULL);
        EXPECT_EQ(encode(0, PortType::Aux, 0), 0x0000000000020000ULL);
        EXPECT_EQ(encode(1, PortType::In, 0), 0x0000000100000000ULL);
        EXPECT_EQ(encode(UINT32_MAX, PortType::Aux, UINT16_MAX), 0xFFFFFFFF0002FFFFULL);
    }

    TEST(ChannelIDCodec, RoundTripsAllBoundaryCombinations) {
        constexpr std::array<NodeID, 3> nodes{0, 1, UINT32_MAX};
        constexpr std::array ports{PortType::In, PortType::Out, PortType::Aux};
        constexpr std::array<ChannelIndex, 3> indices{0, 1, UINT16_MAX};

        for (const auto node : nodes) {
            for (const auto port : ports) {
                for (const auto index : indices) {
                    SCOPED_TRACE(node);
                    SCOPED_TRACE(static_cast<std::uint16_t>(port));
                    SCOPED_TRACE(index);
                    const auto id = encode(node, port, index);
                    EXPECT_EQ(decodeNode(id), node);
                    EXPECT_EQ(decodePort(id), port);
                    EXPECT_EQ(decodeChannel(id), index);
                    EXPECT_TRUE(isValidPort(decodePort(id)));
                }
            }
        }
    }

    TEST(ChannelIDCodec, PreservesEveryChannelIndexWithoutFieldCollisions) {
        struct Prefix {
            PortType port;
            ChannelID expected;
        };
        constexpr std::array prefixes{
            Prefix{PortType::In,  0x89ABCDEF00000000ULL},
            Prefix{PortType::Out, 0x89ABCDEF00010000ULL},
            Prefix{PortType::Aux, 0x89ABCDEF00020000ULL},
        };

        for (const auto prefix : prefixes) {
            // A wider counter prevents wrapping at the inclusive uint16 maximum.
            for (std::uint32_t index = 0; index <= 65535; ++index) {
                const auto id = encode(0x89ABCDEFU, prefix.port, static_cast<ChannelIndex>(index));
                ASSERT_EQ(id, prefix.expected + index) << index;
                ASSERT_EQ(decodeNode(id), 0x89ABCDEFU) << index;
                ASSERT_EQ(decodePort(id), prefix.port) << index;
                ASSERT_EQ(decodeChannel(id), index) << index;
            }
        }
    }

    TEST(ChannelIDCodec, ExtractsEachOfTheSixtyFourBitsFromOnlyItsOwnField) {
        for (unsigned bit = 0; bit < 64; ++bit) {
            SCOPED_TRACE(bit);
            const ChannelID id = ChannelID{1} << bit;
            const NodeID expectedNode = bit >= 32 ? NodeID{1} << (bit - 32) : 0;
            const std::uint16_t expectedPort = bit >= 16 && bit < 32
                ? static_cast<std::uint16_t>(std::uint32_t{1} << (bit - 16)) : 0;
            const ChannelIndex expectedChannel = bit < 16
                ? static_cast<ChannelIndex>(std::uint32_t{1} << bit) : 0;

            EXPECT_EQ(decodeNode(id), expectedNode);
            EXPECT_EQ(static_cast<std::uint16_t>(decodePort(id)), expectedPort);
            EXPECT_EQ(decodeChannel(id), expectedChannel);
            EXPECT_EQ(isValidPort(decodePort(id)), expectedPort <= 2);
        }
    }

    TEST(ChannelIDCodec, DistinctNodePortAndIndexTuplesNeverCollide) {
        constexpr std::array<NodeID, 8> nodes{
            0, 1, 0x0000FFFFU, 0x00010000U, 0x55555555U, 0x80000000U,
            0xAAAAAAAAU, UINT32_MAX};
        constexpr std::array ports{PortType::In, PortType::Out, PortType::Aux};
        constexpr std::array<ChannelIndex, 5> indices{0, 1, 0x7FFFU, 0x8000U, UINT16_MAX};
        std::unordered_set<ChannelID> seen;

        for (const auto node : nodes) {
            for (const auto port : ports) {
                for (const auto index : indices) {
                    const auto id = encode(node, port, index);
                    ASSERT_TRUE(seen.insert(id).second)
                        << node << ':' << static_cast<std::uint16_t>(port) << ':' << index;
                }
            }
        }
        EXPECT_EQ(seen.size(), nodes.size() * ports.size() * indices.size());
    }

    TEST(ChannelIDCodec, PreservesAndRejectsEveryUndefinedDecodedPortCode) {
        for (std::uint32_t code = 3; code <= 65535; ++code) {
            // Construct malformed identifiers directly: encode requires a valid port.
            const ChannelID id = 0x13579BDF00002468ULL + ChannelID{code} * 65536;
            const auto port = decodePort(id);
            ASSERT_EQ(static_cast<std::uint16_t>(port), code) << code;
            ASSERT_FALSE(isValidPort(port)) << code;
            ASSERT_FALSE(isInputPort(port)) << code;
            ASSERT_FALSE(isOutputPort(port)) << code;
            ASSERT_EQ(decodeNode(id), 0x13579BDFU) << code;
            ASSERT_EQ(decodeChannel(id), 0x2468U) << code;
        }
    }

    TEST(ChannelIDCodec, ArbitraryIdentifiersDecodeAndRecombineWithoutLosingBits) {
        constexpr std::array<ChannelID, 8> identifiers{
            0, UINT64_MAX, 0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL,
            0xAAAAAAAAAAAAAAAAULL, 0x5555555555555555ULL,
            0x8000000000000000ULL, 0xFFFFFFFF0001FFFFULL};

        for (const auto id : identifiers) {
            SCOPED_TRACE(id);
            // Arithmetic quotient/remainder oracle, independent of shift/mask code.
            const auto expectedNode = id / 0x100000000ULL;
            const auto expectedPort = (id / 0x10000ULL) % 0x10000ULL;
            const auto expectedChannel = id % 0x10000ULL;
            EXPECT_EQ(decodeNode(id), expectedNode);
            EXPECT_EQ(static_cast<std::uint16_t>(decodePort(id)), expectedPort);
            EXPECT_EQ(decodeChannel(id), expectedChannel);

            const auto recombined = ChannelID{decodeNode(id)} * 0x100000000ULL +
                ChannelID{static_cast<std::uint16_t>(decodePort(id))} * 0x10000ULL +
                ChannelID{decodeChannel(id)};
            EXPECT_EQ(recombined, id);
            EXPECT_EQ(isValidPort(decodePort(id)), expectedPort <= 2);
            if (isValidPort(decodePort(id))) {
                EXPECT_EQ(encode(decodeNode(id), decodePort(id), decodeChannel(id)), id);
            }
        }
    }

    TEST(ChannelIDCodec, EncoderPreservesEveryNodeBitIncludingTheHighest) {
        for (unsigned bit = 0; bit < 32; ++bit) {
            SCOPED_TRACE(bit);
            const NodeID node = NodeID{1} << bit;
            const ChannelID expected = (ChannelID{1} << (bit + 32)) + 0x0001A5A5ULL;
            const auto id = encode(node, PortType::Out, 0xA5A5U);
            EXPECT_EQ(id, expected);
            EXPECT_EQ(decodeNode(id), node);
            EXPECT_EQ(decodePort(id), PortType::Out);
            EXPECT_EQ(decodeChannel(id), 0xA5A5U);
        }
    }
}
