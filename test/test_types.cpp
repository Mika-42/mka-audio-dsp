#include <array>
#include <concepts>
#include <cstdint>
#include <expected>
#include <limits>
#include <type_traits>

#include <gtest/gtest.h>

import mka.audio.dsp.types;

namespace {
    using namespace mka::audio::dsp;

    static_assert(std::same_as<NodeID, std::uint32_t>);
    static_assert(std::same_as<ChannelID, std::uint64_t>);
    static_assert(std::same_as<ChannelIndex, std::uint16_t>);
    static_assert(std::same_as<FrameCount, std::uint32_t>);
    static_assert(sizeof(NodeID) == 4 && sizeof(ChannelID) == 8 &&
                  sizeof(ChannelIndex) == 2 && sizeof(FrameCount) == 4);
    static_assert(std::numeric_limits<NodeID>::digits == 32);
    static_assert(std::numeric_limits<ChannelID>::digits == 64);
    static_assert(std::numeric_limits<ChannelIndex>::digits == 16);
    static_assert(std::numeric_limits<FrameCount>::digits == 32);
    static_assert(std::is_unsigned_v<NodeID> && std::is_unsigned_v<ChannelID> &&
                  std::is_unsigned_v<ChannelIndex> && std::is_unsigned_v<FrameCount>);
    static_assert(std::is_trivial_v<NodeID> && std::is_trivial_v<ChannelID> &&
                  std::is_trivial_v<ChannelIndex> && std::is_trivial_v<FrameCount>);

    static_assert(invalidNodeID == 0 && maxNodeID == UINT32_MAX);
    static_assert(maxChannelIndex == 65535);
    static_assert(toChannelIndex(0).value() == 0);
    static_assert(toChannelIndex(65535).value() == 65535);
    static_assert(!toChannelIndex(65536).has_value());
    static_assert(toChannelIndex(65536).error() == ErrorType::InvalidChannelIndex);
    static_assert(toChannelIndex(-1).error() == ErrorType::InvalidChannelIndex);
    static_assert(toChannelIndex(UINT64_MAX).error() == ErrorType::InvalidChannelIndex);
    static_assert(noexcept(toChannelIndex(std::int64_t{})));
    static_assert(noexcept(toChannelIndex(std::uint64_t{})));
    static_assert(std::same_as<decltype(toChannelIndex(0)),
                               std::expected<ChannelIndex, ErrorType>>);

    enum class NonIntegralIndex { Zero };

    template<typename T>
    concept ConvertibleIndex = requires(T value) { toChannelIndex(value); };

    static_assert(!ConvertibleIndex<float> && !ConvertibleIndex<double>);
    static_assert(!ConvertibleIndex<NonIntegralIndex> && !ConvertibleIndex<int*>);

    TEST(FundamentalTypes, PreserveExactNumericLimits) {
        EXPECT_EQ(std::numeric_limits<NodeID>::max(), UINT32_MAX);
        EXPECT_EQ(std::numeric_limits<ChannelID>::max(), UINT64_MAX);
        EXPECT_EQ(std::numeric_limits<FrameCount>::max(), UINT32_MAX);
        EXPECT_EQ(std::numeric_limits<ChannelIndex>::lowest(), 0);
        EXPECT_EQ(maxChannelIndex, 65535);
        EXPECT_EQ(invalidNodeID, 0);
        EXPECT_EQ(maxNodeID, UINT32_MAX);
    }

    TEST(ChannelIndexConversion, AcceptsBothBoundsAndTheirNeighbours) {
        for (const auto value : std::array<std::uint64_t, 4>{0, 1, 65534, 65535}) {
            SCOPED_TRACE(value);
            const auto result = toChannelIndex(value);
            ASSERT_TRUE(result.has_value());
            EXPECT_EQ(*result, value);
        }
    }

    TEST(ChannelIndexConversion, PreservesEveryRepresentableIndex) {
        // A wider loop counter is essential: ChannelIndex itself would wrap.
        for (std::uint32_t value = 0; value <= 65535; ++value) {
            const auto result = toChannelIndex(value);
            ASSERT_TRUE(result.has_value()) << value;
            ASSERT_EQ(*result, value);
            const auto narrow = toChannelIndex(static_cast<ChannelIndex>(value));
            ASSERT_TRUE(narrow.has_value()) << value;
            ASSERT_EQ(*narrow, value);
        }
    }

    TEST(ChannelIndexConversion, RejectsOverflowInsteadOfWrappingToZeroOrOne) {
        for (const auto value : std::array<std::uint64_t, 6>{
                 65536, 65537, 131071, UINT32_MAX, std::uint64_t{UINT32_MAX} + 1,
                 UINT64_MAX}) {
            SCOPED_TRACE(value);
            const auto result = toChannelIndex(value);
            ASSERT_FALSE(result.has_value());
            EXPECT_EQ(result.error(), ErrorType::InvalidChannelIndex);
        }
    }

    TEST(ChannelIndexConversion, RejectsEveryHighBitBeforeNarrowing) {
        for (unsigned bit = 16; bit < 64; ++bit) {
            for (const auto low : std::array<std::uint64_t, 3>{0, 1, 65535}) {
                const auto value = (std::uint64_t{1} << bit) | low;
                SCOPED_TRACE(value);
                const auto result = toChannelIndex(value);
                ASSERT_FALSE(result.has_value());
                EXPECT_EQ(result.error(), ErrorType::InvalidChannelIndex);
            }
        }
    }

    TEST(ChannelIndexConversion, RejectsNegativeValuesAndSignedExtremes) {
        for (const auto value : std::array<std::int64_t, 7>{
                 -1, -65535, -65536, -65537,
                 std::numeric_limits<std::int64_t>::min(), 65536,
                 std::numeric_limits<std::int64_t>::max()}) {
            SCOPED_TRACE(value);
            const auto result = toChannelIndex(value);
            ASSERT_FALSE(result.has_value());
            EXPECT_EQ(result.error(), ErrorType::InvalidChannelIndex);
        }
        EXPECT_EQ(toChannelIndex(std::int64_t{65535}).value(), 65535);
        EXPECT_EQ(toChannelIndex(std::int64_t{0}).value(), 0);
    }

    template<typename Integer>
    void checkIntegralSourceType() {
        static_assert(noexcept(toChannelIndex(Integer{})));
        EXPECT_EQ(toChannelIndex(Integer{0}).value(), 0);
        EXPECT_EQ(toChannelIndex(Integer{1}).value(), 1);

        constexpr auto maximum = std::numeric_limits<Integer>::max();
        const auto result = toChannelIndex(maximum);
        if constexpr (std::numeric_limits<Integer>::digits <= 16) {
            ASSERT_TRUE(result.has_value());
            EXPECT_EQ(*result, maximum);
        } else {
            ASSERT_FALSE(result.has_value());
            EXPECT_EQ(result.error(), ErrorType::InvalidChannelIndex);
        }
        if constexpr (std::signed_integral<Integer>) {
            const auto negative = toChannelIndex(std::numeric_limits<Integer>::lowest());
            ASSERT_FALSE(negative.has_value());
            EXPECT_EQ(negative.error(), ErrorType::InvalidChannelIndex);
        }
    }

    TEST(ChannelIndexConversion, SupportsAllStandardIntegralSourceTypes) {
        checkIntegralSourceType<signed char>();
        checkIntegralSourceType<unsigned char>();
        checkIntegralSourceType<short>();
        checkIntegralSourceType<unsigned short>();
        checkIntegralSourceType<int>();
        checkIntegralSourceType<unsigned int>();
        checkIntegralSourceType<long>();
        checkIntegralSourceType<unsigned long>();
        checkIntegralSourceType<long long>();
        checkIntegralSourceType<unsigned long long>();
        checkIntegralSourceType<char>();
        checkIntegralSourceType<wchar_t>();
        checkIntegralSourceType<char8_t>();
        checkIntegralSourceType<char16_t>();
        checkIntegralSourceType<char32_t>();
        checkIntegralSourceType<bool>();
    }
}
