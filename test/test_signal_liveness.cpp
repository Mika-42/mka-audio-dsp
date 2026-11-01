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
            auto last = ops.size();
            while (last > producer + 1) {
                --last;
                if (std::ranges::find(ops[last].reads, s) != ops[last].reads.end()) break;
            }
            if (last == producer + 1 &&
                std::ranges::find(ops[last].reads, s) == ops[last].reads.end()) last = producer;
            if (last == ops.size()) last = producer;
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
}
