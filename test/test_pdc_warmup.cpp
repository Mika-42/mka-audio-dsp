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
#include "delay_audio_fixture.hpp"
#include "v1_fixture.hpp"
namespace {
    using namespace mka::audio::dsp;
    TEST(PdcWarmup, FreshGenerationStartsAtZeroRegardlessOfAWarmHistoryAndBlockPartition) {
        for (const std::uint64_t delay : {0U,3U,256U}) {
            SCOPED_TRACE(delay); DAG graph; PlanSwitcher sw;
            const auto a=graph.add<audio_pdc_test::MonoPlugin>(0U,0U),b=graph.add<audio_pdc_test::MonoPlugin>(0U,audio_pdc_test::noLane,0U);
            ASSERT_TRUE(a); ASSERT_TRUE(b);
            const Connection edge{encode(*a,PortType::Out,0),encode(*b,PortType::In,0)};
            ASSERT_TRUE(graph.connect(edge.to,edge.from)); ASSERT_TRUE(graph.markAsOutput(*b));
            const std::array oldDelay{ConnectionDelay{edge,3}},newDelay{ConnectionDelay{edge,delay}};
            BackendConfig cfg{.maxFrames=7,.inputChannels=2,.outputChannels=6,.connectionDelays=oldDelay};
            ASSERT_TRUE(sw.initialize(graph,cfg)); v1_test::Bank bank;
            for (unsigned block=0;block<2;++block) {
                bank.fill(4); for (unsigned f=0;f<4;++f) bank.input[0][f+1]=static_cast<float>(block*4+f+1);
                v1_test::callback(sw,bank.io(4));
            }
            cfg.connectionDelays=newDelay; auto rejected=cfg; rejected.memoryBudgetBytes=0;
            EXPECT_EQ(sw.prepareAndPublish(graph,rejected).error().code,ErrorCode::MemoryBudgetExceeded);
            ASSERT_TRUE(sw.prepareInactivePlan(graph,cfg));
            EXPECT_EQ(sw.readyPlan()->preparedMemoryUsage().historyBytes,delay*sizeof(float));
            ASSERT_TRUE(sw.publishReadyPlan()); bank.fill(0); v1_test::callback(sw,bank.io(0));
            EXPECT_EQ(sw.audioGeneration(),2U); ASSERT_TRUE(sw.retireAcknowledgedPlan());
            const std::array<FrameCount,6> sizes{0,1,2,7,0,7}; std::size_t n=0,block=0;
            while (n<280) {
                const auto frames=static_cast<FrameCount>(std::min<std::size_t>(sizes[block++%sizes.size()],280-n));
                bank.fill(frames);
                for (FrameCount f=0;f<frames;++f) bank.input[0][f+1]=static_cast<float>(9+n+f);
                v1_test::callback(sw,bank.io(frames));
                for (FrameCount f=0;f<frames;++f)
                    EXPECT_EQ(bank.output[0][f+1],n+f<delay ? 0.F:static_cast<float>(9+n+f-delay))<<"B sample="<<n+f;
                n+=frames;
            }
        }
    }
}
