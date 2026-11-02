#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <gtest/gtest.h>
import mka.audio.dsp.memory_budget;

namespace {
    using namespace mka::audio::dsp;
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();

    TEST(PlanMemory, ExactBudgetIncludesSilenceHistoriesMetadataAndPadding) {
        const std::array<std::uint64_t,2> histories{3, 256};
        const std::array metadata{MemoryArray{3, 1, 8}, MemoryArray{2, 16, 16}};
        for (FrameCount frames : {0U, 1U, 4096U}) {
            const PlanMemoryRequest request{2, frames, histories, metadata};
            const auto usage = measurePlanMemory(request); ASSERT_TRUE(usage);
            EXPECT_EQ(usage->slotBytes, std::size_t{2} * frames * 4);
            EXPECT_EQ(usage->silenceBytes, std::size_t{frames} * 4);
            EXPECT_EQ(usage->historyBytes, 259U * 4);
            EXPECT_EQ(usage->metadataBytes, 35U);
            EXPECT_EQ(usage->totalBytes, usage->slotBytes + usage->silenceBytes +
                      usage->historyBytes + usage->metadataBytes + usage->paddingBytes);
            EXPECT_TRUE(measurePlanMemory(request, usage->totalBytes));
            const auto failed = measurePlanMemory(request, usage->totalBytes - 1);
            ASSERT_FALSE(failed); EXPECT_EQ(failed.error().code, ErrorCode::MemoryBudgetExceeded);
        }
        const auto zero = measurePlanMemory({}); ASSERT_TRUE(zero); EXPECT_EQ(zero->totalBytes, 0U);
        const auto silence = measurePlanMemory({.maxFrames = 4096}); ASSERT_TRUE(silence);
        EXPECT_EQ(silence->totalBytes, 4096U * 4);
    }

    TEST(PlanMemory, OverflowIsDistinctFromCapacityAndBudgetBeforeAnyAllocation) {
        EXPECT_TRUE(checkedSizeMultiply(maximum, 1));
        EXPECT_FALSE(checkedSizeMultiply(maximum, 2));
        EXPECT_TRUE(checkedSizeAdd(maximum - 1, 1));
        EXPECT_FALSE(checkedSizeAdd(maximum, 1));
        const auto capacity = measurePlanMemory({.slotCount = std::uint64_t{UINT32_MAX} + 1});
        ASSERT_FALSE(capacity); EXPECT_EQ(capacity.error().code, ErrorCode::CapacityExceeded);
        const auto overflow = measurePlanMemory({.slotCount = UINT32_MAX, .maxFrames = UINT32_MAX});
        ASSERT_FALSE(overflow); EXPECT_EQ(overflow.error().code, ErrorCode::ArithmeticOverflow);
        const std::array<std::uint64_t,1> hugeHistory{maximum};
        const auto history = measurePlanMemory({.delaySamples = hugeHistory});
        ASSERT_FALSE(history); EXPECT_EQ(history.error().code, ErrorCode::ArithmeticOverflow);
        const std::array arrays{MemoryArray{1, maximum}, MemoryArray{1, 1}};
        const auto addition = measurePlanMemory({.metadata = arrays});
        ASSERT_FALSE(addition); EXPECT_EQ(addition.error().code, ErrorCode::ArithmeticOverflow);
        const std::array padding{MemoryArray{1, maximum}, MemoryArray{1, 1, 8}};
        EXPECT_FALSE(measurePlanMemory({.metadata = padding}));
    }

    TEST(PlanMemory, NearSizeMaxAndDominantHistoryNeedNoArbitraryCeiling) {
        const std::array<std::uint64_t,1> histories{maximum / 4};
        const auto usage = measurePlanMemory({.delaySamples = histories}); ASSERT_TRUE(usage);
        EXPECT_EQ(usage->historyBytes, (maximum / 4) * 4);
        const std::array invalid{MemoryArray{1, 1, 3}};
        EXPECT_FALSE(measurePlanMemory({.metadata = invalid}));
    }
}
