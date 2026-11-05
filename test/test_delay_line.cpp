/**
 * @file test_delay_line.cpp
 * @brief Tests for delay line.
 * @author Mika
 */
#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <type_traits>
#include <utility>
#include <gtest/gtest.h>
import mka.audio.dsp.delay_line;

namespace {
    using namespace mka::audio::dsp;
    static_assert(std::is_final_v<DelayLine> && !std::is_polymorphic_v<DelayLine>);
    static_assert(std::same_as<decltype(&DelayLine::process),
        void (DelayLine::*)(const float*,float*,FrameCount) noexcept>);
    static_assert(std::same_as<decltype(std::declval<DelayLine&>().prepare(std::uint64_t{})),
        std::expected<void,PrepareError>>);
    static_assert(std::is_nothrow_move_constructible_v<DelayLine> && std::is_nothrow_move_assignable_v<DelayLine>);

    /// @brief Test case: default and prepared zero copy without history and permit identical pointer.
    TEST(DelayLine, DefaultAndPreparedZeroCopyWithoutHistoryAndPermitIdenticalPointer) {
        DelayLine line;
        const std::array<float,4> input{1,2,3,4}; std::array<float,4> output{};
        line.process(input.data(),output.data(),4); EXPECT_EQ(output,input);
        ASSERT_TRUE(line.prepare(0)); EXPECT_EQ(line.delaySamples(),0U); EXPECT_EQ(line.historyBytes(),0U);
        line.process(output.data(),output.data(),4); EXPECT_EQ(output,input);
        line.process(nullptr,nullptr,0);
    }

    /// @brief Test case: three samples persist across the two reference blocks.
    TEST(DelayLine, ThreeSamplesPersistAcrossTheTwoReferenceBlocks) {
        DelayLine line; ASSERT_TRUE(line.prepare(3));
        const std::array<float,4> first{1,2,3,4},second{5,6,7,8}; std::array<float,4> output{};
        line.process(first.data(),output.data(),4); EXPECT_EQ(output,(std::array<float,4>{0,0,0,1}));
        line.process(nullptr,nullptr,0);
        line.process(second.data(),output.data(),4); EXPECT_EQ(output,(std::array<float,4>{2,3,4,5}));
        EXPECT_EQ(line.delaySamples(),3U); EXPECT_EQ(line.historyBytes(),12U);
    }

    /// @brief Test case: delay one and delay longer than every block follow absolute stream time.
    TEST(DelayLine, DelayOneAndDelayLongerThanEveryBlockFollowAbsoluteStreamTime) {
        for (const std::uint64_t delay : {1U,3U,256U}) {
            SCOPED_TRACE(delay); DelayLine line; ASSERT_TRUE(line.prepare(delay));
            std::array<float,4> input{},output{};
            for (unsigned block=0;block<70;++block) {
                for (unsigned f=0;f<4;++f) input[f]=static_cast<float>(block*4+f+1);
                line.process(input.data(),output.data(),4);
                for (unsigned f=0;f<4;++f) {
                    const auto n=block*4+f;
                    EXPECT_EQ(output[f],n<delay ? 0.F : static_cast<float>(n-delay+1))<<"n="<<n;
                }
            }
        }
    }

    /// @brief Test case: overflow and quota failures preserve the previous cursor and samples.
    TEST(DelayLine, OverflowAndQuotaFailuresPreserveThePreviousCursorAndSamples) {
        DelayLine line; ASSERT_TRUE(line.prepare(3));
        const std::array<float,4> first{1,2,3,4},second{5,6,7,8}; std::array<float,4> output{};
        line.process(first.data(),output.data(),4);
        EXPECT_EQ(line.prepare(UINT64_MAX).error().code,ErrorCode::ArithmeticOverflow);
        EXPECT_EQ(line.prepare(64,DelayLineLimits{.memoryBudgetBytes=255}).error().code,ErrorCode::MemoryBudgetExceeded);
        EXPECT_EQ(line.prepare(4,DelayLineLimits{.maxDelaySamples=3}).error().code,ErrorCode::MemoryBudgetExceeded);
        EXPECT_EQ(line.delaySamples(),3U);
        line.process(second.data(),output.data(),4); EXPECT_EQ(output,(std::array<float,4>{2,3,4,5}));
        ASSERT_TRUE(line.prepare(64,DelayLineLimits{.maxDelaySamples=64,.memoryBudgetBytes=256}));
        EXPECT_EQ(line.historyBytes(),256U);
    }

    /// @brief Test case: successful preparation explicitly resets history and can release it.
    TEST(DelayLine, SuccessfulPreparationExplicitlyResetsHistoryAndCanReleaseIt) {
        DelayLine line; ASSERT_TRUE(line.prepare(3));
        const std::array<float,4> input{1,2,3,4}; std::array<float,4> output{};
        line.process(input.data(),output.data(),4);
        ASSERT_TRUE(line.prepare(3));
        line.process(input.data(),output.data(),4); EXPECT_EQ(output,(std::array<float,4>{0,0,0,1}));
        ASSERT_TRUE(line.prepare(0,DelayLineLimits{.maxDelaySamples=0,.memoryBudgetBytes=0}));
        EXPECT_EQ(line.historyBytes(),0U);
        line.process(input.data(),output.data(),4); EXPECT_EQ(output,input);
    }

    /// @brief Test case: copies own independent history and moves preserve the stream cursor.
    TEST(DelayLine, CopiesOwnIndependentHistoryAndMovesPreserveTheStreamCursor) {
        DelayLine original; ASSERT_TRUE(original.prepare(3));
        const std::array<float,4> first{1,2,3,4},second{5,6,7,8},third{9,10,11,12};
        std::array<float,4> output{},copyOutput{};
        original.process(first.data(),output.data(),4);
        DelayLine copy=original;
        original.process(second.data(),output.data(),4);
        copy.process(second.data(),copyOutput.data(),4); EXPECT_EQ(copyOutput,output);
        DelayLine moved=std::move(copy); EXPECT_EQ(copy.delaySamples(),0U);
        moved.process(third.data(),output.data(),4); EXPECT_EQ(output,(std::array<float,4>{6,7,8,9}));
        copy.process(third.data(),copyOutput.data(),4); EXPECT_EQ(copyOutput,third);
        copy=std::move(moved); EXPECT_EQ(moved.delaySamples(),0U);
        const std::array<float,4> fourth{13,14,15,16};
        copy.process(fourth.data(),output.data(),4); EXPECT_EQ(output,(std::array<float,4>{10,11,12,13}));
    }
}
