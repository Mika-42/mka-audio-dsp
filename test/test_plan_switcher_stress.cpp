#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iostream>
#include <optional>
#include <span>
#include <thread>
#include <gtest/gtest.h>
#include "switcher_backend_fixture.hpp"
import mka.audio.dsp.plan_switcher;
#include "plan_switcher_stress.hpp"

namespace {
    TEST(PlanSwitcherStress, TenThousandThreadedSwapsIncludeEditsFailuresRetirementStopAndPdc) {
        const auto result=switcher_test::runSwitcherStress();
        std::cout << "seed=0x7342ABC1 swaps=" << result.swaps << " published=" << result.published
                  << " canceled=" << result.canceled << " failedCandidates=" << result.failedCandidates
                  << " busyRefusals=" << result.busyRefusals << " stops=" << result.stops
                  << " callbacks=" << result.callbacks << " samples=" << result.samples
                  << " delayPlans=" << result.delayPlans[0] << '/' << result.delayPlans[1] << '/' << result.delayPlans[2] << '\n';
        EXPECT_EQ(result.failures,0U) << "control step=" << result.errorStep;
        EXPECT_EQ(result.swaps,10000U); EXPECT_EQ(result.published,result.swaps+result.canceled);
        EXPECT_GT(result.canceled,0U); EXPECT_GT(result.stops,0U); EXPECT_GT(result.failedCandidates,0U);
        EXPECT_GT(result.edits,0U); EXPECT_GT(result.samples,0U);
        for (const auto plans : result.delayPlans) EXPECT_GT(plans,0U);
        EXPECT_EQ(result.created,result.destroyed); EXPECT_EQ(result.destroyedOnAudio,0U);
    }
}
