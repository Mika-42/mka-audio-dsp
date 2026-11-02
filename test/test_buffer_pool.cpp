#include <algorithm>
#include <concepts>
#include <cstdint>
#include <limits>
#include <type_traits>
#include <utility>
#include <gtest/gtest.h>

import mka.audio.dsp.buffer_pool;

namespace {
    using namespace mka::audio::dsp;
    static_assert(std::same_as<BufferId, std::uint32_t>);
    static_assert(std::is_final_v<BufferPool> && !std::is_polymorphic_v<BufferPool>);
    static_assert(!std::is_move_constructible_v<BufferPool>);
    static_assert(noexcept(std::declval<BufferPool&>().buffer(BufferId{})));

    TEST(BufferPool, ExactMonoCapacitiesAreIsolatedAndStable) {
        for (auto slots : {0U, 1U, 7U}) for (auto frames : {1U, 128U, 4096U}) {
            BufferPool pool;
            ASSERT_TRUE(pool.prepare(slots, frames));
            EXPECT_EQ(pool.slotCapacity(), slots);
            EXPECT_EQ(pool.maxFrames(), frames);
            EXPECT_EQ(pool.allocatedBytes(), (std::size_t{slots} + 1) * frames * sizeof(float));
            for (BufferId id = 0; id < slots; ++id) {
                auto* samples = pool.buffer(id);
                EXPECT_TRUE(std::all_of(samples, samples + frames, [](float x) { return x == 0.F; }));
                std::fill_n(samples, frames, static_cast<float>(id + 1));
                if (id) EXPECT_EQ(samples - pool.buffer(id - 1), frames);
            }
            for (unsigned callback = 0; callback < 64; ++callback)
                for (BufferId id = 0; id < slots; ++id) {
                    EXPECT_EQ(pool.buffer(id), std::as_const(pool).buffer(id));
                    for (FrameCount f = 0; f < frames; ++f) EXPECT_EQ(pool.buffer(id)[f], id + 1.F);
                }
        }
    }

    TEST(BufferPool, FailedBudgetAndOverflowPreserveOldStorage) {
        BufferPool pool;
        ASSERT_TRUE(pool.prepare(1, 128));
        auto* old = pool.buffer(0);
        old[0] = 42.F;
        auto failed = pool.prepare(2, 128, 1535);
        ASSERT_FALSE(failed);
        EXPECT_EQ(failed.error().code, ErrorCode::MemoryBudgetExceeded);
        failed = pool.prepare(UINT32_MAX, UINT32_MAX);
        ASSERT_FALSE(failed);
        EXPECT_EQ(failed.error().code, ErrorCode::ArithmeticOverflow);
        EXPECT_EQ(pool.buffer(0), old);
        EXPECT_EQ(old[0], 42.F);
        EXPECT_EQ(pool.slotCapacity(), 1U);
        EXPECT_EQ(pool.maxFrames(), 128U);
        EXPECT_TRUE(pool.prepare(2, 128, 1536));
    }

    TEST(BufferPool, ZeroFramesAndIndependentControlCopy) {
        BufferPool empty;
        ASSERT_TRUE(empty.prepare(2, 0, 0));
        EXPECT_EQ(empty.buffer(1), nullptr);
        EXPECT_EQ(empty.allocatedBytes(), 0U);
        ASSERT_TRUE(empty.prepare(1, 1));
        empty.buffer(0)[0] = 9.F;
        const BufferPool copy(empty);
        EXPECT_NE(copy.buffer(0), empty.buffer(0));
        EXPECT_EQ(copy.buffer(0)[0], 9.F);
    }

    static_assert(std::same_as<decltype(std::declval<const BufferPool&>().silence()), const float*>);
    static_assert(noexcept(std::declval<const BufferPool&>().silence()));

    TEST(BufferPool, SilenceIsSharedConstAndExcludedFromEveryWritableSlot) {
        for (FrameCount frames : {1U, 128U, 4096U}) {
            BufferPool pool;
            ASSERT_TRUE(pool.prepare(7, frames));
            const auto* zero = pool.silence();
            ASSERT_NE(zero, nullptr);
            for (unsigned callback = 0; callback < 64; ++callback) {
                for (BufferId id = 0; id < pool.slotCapacity(); ++id) {
                    EXPECT_NE(pool.buffer(id), zero);
                    std::fill_n(pool.buffer(id), frames, 123.F + id);
                }
                EXPECT_EQ(pool.silence(), zero);
                EXPECT_TRUE(std::all_of(zero, zero + frames, [](float f) { return f == 0.F; }));
            }
            const auto failed = pool.prepare(100, frames, 0);
            ASSERT_FALSE(failed);
            EXPECT_EQ(pool.silence(), zero);
        }
    }
}
