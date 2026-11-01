#include <algorithm>
#include <cstddef>
#include <vector>
#include <gtest/gtest.h>

import mka.audio.dsp.signal_liveness;

namespace {
    using namespace mka::audio::dsp;

    // Deliberately independent: find each producer, then scan backwards for the
    // last matching read instead of updating forward maxima like production.
    std::vector<SignalLifetime> backwardsOracle(const std::vector<SignalOperation>& ops,
                                               std::size_t count) {
        std::vector<SignalLifetime> expected;
        for (SignalID s = 0; s < count; ++s) {
            std::size_t producer = 0;
            for (; producer < ops.size(); ++producer)
                if (std::ranges::find(ops[producer].writes, s) != ops[producer].writes.end()) break;
            auto last = producer;
            for (auto consumer = ops.size(); consumer > producer + 1;) {
                --consumer;
                if (std::ranges::find(ops[consumer].reads, s) != ops[consumer].reads.end()) {
                    last = consumer;
                    break;
                }
            }
            expected.push_back({producer, last});
        }
        return expected;
    }

    TEST(SignalLiveness, ChainDiamondAuxAndUnconsumedOutputsHaveExactIndices) {
        const std::vector<SignalOperation> ops{
            {OperationKind::Node, {}, {0,1}},
            {OperationKind::Node, {0}, {2}},
            {OperationKind::Node, {0,1}, {3}}, // Second source read includes Aux.
            {OperationKind::Node, {2,3}, {4}},
            {OperationKind::Node, {}, {5}}, // Never consumed.
            {OperationKind::Readback, {4}, {}}};
        const auto result = calculateLifetimes(ops, 6);
        ASSERT_TRUE(result);
        EXPECT_EQ(*result, (std::vector<SignalLifetime>{{0,2},{0,2},{1,3},{2,3},{3,5},{4,4}}));
        EXPECT_EQ(*result, backwardsOracle(ops, 6));
    }

    TEST(SignalLiveness, TenWayFanoutDelayAndLateTerminalReaderExtendEveryProduction) {
        std::vector<SignalOperation> ops{{OperationKind::Node, {}, {0}}};
        for (SignalID s = 1; s <= 10; ++s) ops.push_back({OperationKind::Node, {0}, {s}});
        ops.push_back({OperationKind::Delay, {0}, {11}});
        ops.push_back({OperationKind::Node, {1,10,11}, {12}});
        ops.push_back({OperationKind::Readback, {0,12}, {}});
        const auto result = calculateLifetimes(ops, 13);
        ASSERT_TRUE(result);
        EXPECT_EQ((*result)[0], (SignalLifetime{0,13}));
        EXPECT_EQ((*result)[11], (SignalLifetime{11,12}));
        EXPECT_EQ((*result)[12], (SignalLifetime{12,13}));
        EXPECT_EQ(*result, backwardsOracle(ops, 13));
    }

    TEST(SignalLiveness, RejectsMissingDuplicateFutureAndInvalidProductions) {
        EXPECT_FALSE(calculateLifetimes({}, 1));
        EXPECT_TRUE(calculateLifetimes({}, 0));
        const std::vector<SignalOperation> duplicate{{OperationKind::Node, {}, {0,0}}};
        EXPECT_FALSE(calculateLifetimes(duplicate, 1));
        const std::vector<SignalOperation> future{{OperationKind::Node, {0}, {1}},
                                                 {OperationKind::Node, {}, {0}}};
        EXPECT_FALSE(calculateLifetimes(future, 2));
        const std::vector<SignalOperation> invalid{{OperationKind::Node, {}, {1}}};
        EXPECT_FALSE(calculateLifetimes(invalid, 1));
        const std::vector<SignalOperation> self{{OperationKind::Node, {0}, {0}}};
        EXPECT_FALSE(calculateLifetimes(self, 1));
    }

    void expectNoOverlap(const std::vector<SignalLifetime>& life, const SlotAssignment& slots) {
        for (SignalID a = 0; a < life.size(); ++a) for (SignalID b = a + 1; b < life.size(); ++b)
            if (life[a].producedAt <= life[b].lastUse && life[b].producedAt <= life[a].lastUse)
                EXPECT_NE(slots.slotOf[a], slots.slotOf[b]) << "signals " << a << ", " << b;
    }

    TEST(StaticSlots, LastInputAndNewOutputNeverAliasWithinOneOperation) {
        std::vector<SignalOperation> ops;
        for (SignalID s = 0; s < 100; ++s)
            ops.push_back({OperationKind::Node, s == 0 ? std::vector<SignalID>{} :
                           std::vector<SignalID>{s - 1}, {s}});
        const auto life = calculateLifetimes(ops, 100);
        ASSERT_TRUE(life);
        const auto slots = assignBufferSlots(*life, ops.size());
        const auto unique = assignBufferSlots(*life, ops.size(), SlotStrategy::Unique);
        ASSERT_TRUE(slots); ASSERT_TRUE(unique);
        EXPECT_EQ(slots->peakSlots, 2U);
        EXPECT_EQ(slots->peakLiveSlots, 2U);
        EXPECT_EQ(unique->peakSlots, 100U);
        EXPECT_EQ(slots->slotOf[0], slots->slotOf[2]);
        expectNoOverlap(*life, *slots);
        expectNoOverlap(*life, *unique);
        EXPECT_EQ(*life, backwardsOracle(ops, 100));
    }

    TEST(StaticSlots, DiamondLateReadersAndDelayMatchANonRecyclingSampleOracle) {
        const std::vector<SignalOperation> ops{
            {OperationKind::Node, {}, {0}}, {OperationKind::Node, {0}, {1}},
            {OperationKind::Node, {0}, {2}}, {OperationKind::Delay, {0}, {3}},
            {OperationKind::Node, {1,2,3}, {4}}, {OperationKind::Readback, {0,4}, {}}};
        const auto life = calculateLifetimes(ops, 5); ASSERT_TRUE(life);
        const auto slots = assignBufferSlots(*life, ops.size()); ASSERT_TRUE(slots);
        const auto unique = assignBufferSlots(*life, ops.size(), SlotStrategy::Unique); ASSERT_TRUE(unique);
        expectNoOverlap(*life, *slots);
        EXPECT_LE(slots->peakSlots, unique->peakSlots);
        std::vector<float> memory(slots->peakSlots, -999.F), oracle(5, -999.F);
        for (std::size_t op = 0; op < ops.size(); ++op) {
            float value = 1.F, expected = 1.F;
            for (const auto input : ops[op].reads) {
                value += memory[slots->slotOf[input]];
                expected += oracle[input];
            }
            for (const auto output : ops[op].writes) {
                memory[slots->slotOf[output]] = value;
                oracle[output] = expected;
                EXPECT_EQ(value, expected) << "op " << op << " signal " << output;
            }
            EXPECT_EQ(value, expected) << "readback " << op;
        }
    }

    TEST(StaticSlots, InvalidIntervalsAndCapacityFailWithoutNarrowing) {
        EXPECT_TRUE(assignBufferSlots({}, 0));
        const std::vector<SignalLifetime> overlap{{0,1},{1,1}};
        const auto result = assignBufferSlots(overlap, 2, SlotStrategy::Reuse, 1);
        ASSERT_FALSE(result); EXPECT_EQ(result.error().code, ErrorCode::CapacityExceeded);
        EXPECT_TRUE(assignBufferSlots(overlap, 2, SlotStrategy::Reuse, 2));
        const std::vector<SignalLifetime> backwards{{2,1}}, outside{{0,2}};
        EXPECT_FALSE(assignBufferSlots(backwards, 3));
        EXPECT_FALSE(assignBufferSlots(outside, 2));
    }
}
