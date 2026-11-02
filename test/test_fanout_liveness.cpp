#include <algorithm>
#include <array>
#include <cstddef>
#include <vector>
#include <gtest/gtest.h>

import mka.audio.dsp.signal_liveness;

namespace {
    using namespace mka::audio::dsp;

    TEST(FanoutLiveness, LateAuxDelayAndMarkedOutputSuccessorPreserveSourceSamples) {
        for (std::size_t branches : {2U, 10U}) {
            std::vector<SignalOperation> ops{{OperationKind::Node, {}, {0}}};
            for (SignalID s = 1; s <= branches; ++s)
                ops.push_back({OperationKind::Node, {0}, {s}});
            const SignalID delayed = branches + 1, output = branches + 2, successor = branches + 3;
            ops.push_back({OperationKind::Delay, {0}, {delayed}});
            ops.push_back({OperationKind::Node, {1, branches, delayed}, {output}}); // Backend writer.
            ops.push_back({OperationKind::Node, {output, 0}, {successor}}); // Late Aux source reader.
            ops.push_back({OperationKind::Readback, {successor}, {}});
            const auto life = calculateLifetimes(ops, branches + 4); ASSERT_TRUE(life);
            const auto slots = assignBufferSlots(*life, ops.size()); ASSERT_TRUE(slots);
            ASSERT_TRUE(validateSlotAssignment(*life, *slots));
            EXPECT_EQ((*life)[0].lastUse, ops.size() - 2);
            EXPECT_NE(slots->slotOf[0], slots->slotOf[delayed]);
            EXPECT_NE(slots->slotOf[output], slots->slotOf[successor]);
            // Guard each physical slot; compare every intermediate read, not
            // only the final output, against a completely unique allocation.
            constexpr std::size_t stride = 130;
            std::vector<float> optimized(slots->peakSlots * stride, -777.F);
            std::vector<float> oracle((branches + 4) * stride, -777.F);
            for (FrameCount frames : {0U, 1U, 128U, 17U, 128U}) {
                for (std::size_t op = 0; op < ops.size(); ++op) {
                    for (FrameCount f = 0; f < frames; ++f) {
                        float value = 1.F + f, expected = 1.F + f;
                        for (const auto s : ops[op].reads) {
                            value += optimized[slots->slotOf[s] * stride + f + 1];
                            expected += oracle[s * stride + f + 1];
                        }
                        ASSERT_EQ(value, expected) << "task=" << op << " offset=" << f;
                        for (const auto s : ops[op].writes) {
                            optimized[slots->slotOf[s] * stride + f + 1] = value;
                            oracle[s * stride + f + 1] = expected;
                        }
                    }
                }
                for (BufferId slot = 0; slot < slots->peakSlots; ++slot) {
                    EXPECT_EQ(optimized[slot * stride], -777.F);
                    EXPECT_EQ(optimized[(slot + 1) * stride - 1], -777.F);
                }
            }
        }
    }

    TEST(FanoutLiveness, RejectsPrematureRecyclingAtTheLastConsumerBeforeBinding) {
        const std::vector<SignalLifetime> life{{0,4},{1,2},{2,3},{3,4},{4,4}};
        auto result = assignBufferSlots(life, 5); ASSERT_TRUE(result);
        result->slotOf[1] = result->slotOf[0]; // Corrupt a live fan-out source.
        const auto valid = validateSlotAssignment(life, *result);
        ASSERT_FALSE(valid); EXPECT_EQ(valid.error().code, ErrorCode::InvalidParameter);
        result = assignBufferSlots(life, 5); ASSERT_TRUE(result);
        result->slotOf[4] = result->slotOf[0]; // Even the final reader may not alias.
        EXPECT_FALSE(validateSlotAssignment(life, *result));
        result->slotOf.pop_back();
        EXPECT_FALSE(validateSlotAssignment(life, *result));
    }
}
