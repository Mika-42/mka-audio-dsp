#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <utility>
#include <vector>
#include <gtest/gtest.h>
import mka.audio.dsp.plan_switcher;
#include "v1_fixture.hpp"
namespace {
    using namespace mka::audio::dsp;
    TEST(LatencyRecompile, Zero64Then256ThenZeroUsesNewDSPAndNewCompensationOnlyAtBoundary) {
        v1_test::Graph g; ASSERT_TRUE(g.make(0)); PlanSwitcher sw;
        const BackendConfig cfg{.maxFrames=128,.inputChannels=2,.outputChannels=6,.sampleRate=48000};
        ASSERT_TRUE(sw.initialize(g.dag,cfg)); v1_test::Bank bank;
        for (const LatencySamples delay : {64U,256U,0U}) {
            const auto old=g.slow; ASSERT_TRUE(g.replaceSlow(delay));
            auto rejected=cfg; rejected.memoryBudgetBytes=0;
            EXPECT_EQ(sw.prepareAndPublish(g.dag,rejected).error().code,ErrorCode::MemoryBudgetExceeded);
            ASSERT_TRUE(sw.prepareInactivePlan(g.dag,cfg)); const auto* ready=sw.readyPlan(); ASSERT_NE(ready,nullptr);
            EXPECT_EQ(ready->latencySnapshot()->find(g.slow)->samples,delay);
            EXPECT_EQ(ready->pdcAnalysis()->find(g.mix)->arrival,delay);
            EXPECT_EQ(ready->preparedMemoryUsage().historyBytes,2U*delay*sizeof(float));
            EXPECT_EQ(ready->latencySnapshot()->find(old),nullptr);
            ASSERT_TRUE(sw.publishReadyPlan()); EXPECT_EQ(g.dag.collectRetiredNodes(),0U);
            EXPECT_EQ(sw.prepareAndPublish(g.dag,cfg).error().code,ErrorCode::PlanBusy);
            std::size_t n=0;
            for (unsigned block=0;block<6;++block) {
                bank.fill(64); v1_test::callback(sw,bank.io(64));
                for (unsigned f=0;f<64;++f) {
                    EXPECT_EQ(bank.output[2][f+1],n+f<delay ? 0.F:1.F)<<"D="<<delay<<" n="<<n+f;
                    EXPECT_EQ(bank.output[3][f+1],n+f<delay ? 0.F:-2.F);
                }
                if (block==0) { ASSERT_TRUE(sw.retireAcknowledgedPlan()); EXPECT_EQ(g.dag.collectRetiredNodes(),1U); }
                n+=64;
            }
        }
        EXPECT_EQ(g.trace.destroyedInAudio.load(),0U);
        EXPECT_EQ(g.dag.setConfig(g.gain,Config{}).error().code,ErrorCode::NodeInUse);
    }
    TEST(LatencyRecompile, DesiredLatencyChangeDuringPendingCannotAlterPublishedSnapshot) {
        v1_test::Graph g; ASSERT_TRUE(g.make(0)); PlanSwitcher sw;
        const BackendConfig cfg{.maxFrames=128,.inputChannels=2,.outputChannels=6};
        ASSERT_TRUE(sw.initialize(g.dag,cfg)); ASSERT_TRUE(g.replaceSlow(64)); const auto b=g.slow;
        ASSERT_TRUE(sw.prepareAndPublish(g.dag,cfg)); ASSERT_TRUE(g.replaceSlow(256));
        EXPECT_EQ(sw.prepareAndPublish(g.dag,cfg).error().code,ErrorCode::PlanBusy);
        v1_test::Bank bank; bank.fill(128); v1_test::callback(sw,bank.io(128));
        EXPECT_EQ(bank.output[2][64],0.F); EXPECT_EQ(bank.output[2][65],1.F);
        ASSERT_TRUE(sw.retireAcknowledgedPlan()); EXPECT_GE(g.dag.collectRetiredNodes(),1U);
        ASSERT_TRUE(sw.prepareInactivePlan(g.dag,cfg));
        EXPECT_EQ(sw.readyPlan()->latencySnapshot()->find(g.slow)->samples,256U);
        EXPECT_EQ(sw.readyPlan()->latencySnapshot()->find(b),nullptr);
    }
}
