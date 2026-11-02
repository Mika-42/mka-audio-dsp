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
    float input(unsigned lane,std::size_t t) {
        if(lane==0) return t==0?1.F:t==129?-2.F:t==513?.5F:0.F;
        return t==3?4.F:t==257?-.5F:t==1023?2.F:0.F;
    }
    void fill(v1_test::Bank& bank,std::size_t absolute,FrameCount frames) {
        bank.fill(frames);
        for(unsigned c=0;c<2;++c) for(FrameCount f=0;f<frames;++f) bank.input[c][f+1]=input(c,absolute+f);
    }
    void verify(const v1_test::Bank& bank,std::size_t absolute,std::size_t relative,unsigned delay,FrameCount frames,unsigned outputs) {
        for(unsigned c=0;c<6;++c) {
            for(FrameCount f=0;f<frames;++f) {
                if(c>=outputs) { EXPECT_EQ(bank.output[c][f+1],-999.F); continue; }
                float expected=0.F;
                if(relative+f>=delay && c<4) {
                    const auto lane=c%2; expected=input(lane,absolute+f-delay);
                    if(c==0 && (absolute+f)%2==0) expected*=2;
                }
                EXPECT_EQ(bank.output[c][f+1],expected)<<"physical="<<c<<" D="<<delay<<" absolute="<<absolute+f<<" relative="<<relative+f;
            }
            EXPECT_EQ(bank.output[c].front(),-999.F);
            for(std::size_t f=frames+1;f<130;++f) EXPECT_EQ(bank.output[c][f],-999.F);
        }
    }
    TEST(V1Acceptance, StereoGainDelayMixHeadphonesSpeakersLastWriterAndSilenceMatchEveryImpulseSample) {
        for(unsigned delay : {0U,64U,256U,512U}) for(unsigned outputs : {2U,6U}) {
            SCOPED_TRACE(::testing::Message()<<"D="<<delay<<" outputs="<<outputs);
            v1_test::Graph g; ASSERT_TRUE(g.make(delay));PlanSwitcher sw;
            const BackendConfig cfg{.maxFrames=128,.inputChannels=2,.outputChannels=outputs,.sampleRate=48000,.memoryBudgetBytes=1024*1024};
            ASSERT_TRUE(sw.initialize(g.dag,cfg)); const auto* plan=sw.slotAddress(0);
            EXPECT_EQ(plan->delayTaskCount(),delay ? 2U:0U); EXPECT_EQ(plan->preparedMemoryUsage().historyBytes,2U*delay*sizeof(float));
            EXPECT_EQ(plan->operationSchedule().back().node,g.writer);
            const auto writer=*plan->taskIndexOf(g.writer),router=*plan->taskIndexOf(g.router);
            std::array<v1_test::Bank,2> banks; constexpr std::array<FrameCount,6> sizes{0,1,7,128,2,7};
            std::size_t n=0,block=0; bool failureChecked=false;
            while(n<1536) {
                auto& bank=banks[block%2];const auto frames=static_cast<FrameCount>(std::min<std::size_t>(sizes[block%sizes.size()],1536-n));++block;
                fill(bank,n,frames);const auto before=bank.input;const auto stale=banks[block%2].output;
                if(n>=256 && !failureChecked) {
                    auto bad=cfg;bad.memoryBudgetBytes=0;
                    EXPECT_EQ(sw.prepareAndPublish(g.dag,bad).error().code,ErrorCode::MemoryBudgetExceeded);failureChecked=true;
                }
                for(const auto* out : bank.outputs) EXPECT_NE(plan->executionTasks()[router].context.outputs[0].buffer,out);
                v1_test::callback(sw,bank.io(frames,outputs));verify(bank,n,n,delay,frames,outputs);
                EXPECT_EQ(bank.input,before);EXPECT_EQ(banks[block%2].output,stale);
                // Writer is last: its logical outputs are still the undoubled
                // Mix value although its hardware write may have doubled lane 0.
                for(unsigned c=0;c<2;++c) for(FrameCount f=0;f<frames;++f)
                    EXPECT_EQ(plan->executionTasks()[writer].context.outputs[c].buffer[f],n+f<delay?0.F:input(c,n+f-delay));
                n+=frames;
            }
            EXPECT_TRUE(failureChecked);EXPECT_EQ(sw.callbackDiagnostics(),0U);EXPECT_EQ(g.trace.destroyedInAudio.load(),0U);
        }
    }
    TEST(V1Acceptance, BoundarySwapAndPendingEditsPreserveAudibleAAndKeepNewHistoryPrivate) {
        v1_test::Graph g;ASSERT_TRUE(g.make(0));PlanSwitcher sw;
        const BackendConfig cfg{.maxFrames=128,.inputChannels=2,.outputChannels=6,.sampleRate=48000};
        ASSERT_TRUE(sw.initialize(g.dag,cfg));v1_test::Bank bank;
        fill(bank,0,32);v1_test::callback(sw,bank.io(32));verify(bank,0,0,0,32,6);
        ASSERT_TRUE(g.replaceSlow(64));ASSERT_TRUE(sw.prepareAndPublish(g.dag,cfg));
        ASSERT_TRUE(g.replaceSlow(512));EXPECT_EQ(sw.prepareAndPublish(g.dag,cfg).error().code,ErrorCode::PlanBusy);
        bank.fill(0);v1_test::callback(sw,bank.io(0));ASSERT_TRUE(sw.retireAcknowledgedPlan());
        EXPECT_EQ(g.dag.collectRetiredNodes(),1U);
        const std::array<FrameCount,5> sizes{1,7,0,128,2};std::size_t n=0,block=0;bool refused=false;
        while(n<640) {
            if(n>=128&&!refused) {
                auto bad=cfg;bad.memoryBudgetBytes=0;EXPECT_EQ(sw.prepareAndPublish(g.dag,bad).error().code,ErrorCode::MemoryBudgetExceeded);
                const std::array huge{ConnectionDelay{{encode(g.gain,PortType::Out,0),encode(g.mix,PortType::In,0)},UINT64_MAX}};
                bad=cfg;bad.connectionDelays=huge;EXPECT_EQ(sw.prepareAndPublish(g.dag,bad).error().code,ErrorCode::ArithmeticOverflow);
                refused=true;
            }
            const auto frames=static_cast<FrameCount>(std::min<std::size_t>(sizes[block++%sizes.size()],640-n));
            fill(bank,32+n,frames);v1_test::callback(sw,bank.io(frames));verify(bank,32+n,n,64,frames,6);n+=frames;
            EXPECT_EQ(sw.audioGeneration(),2U);
        }
        EXPECT_TRUE(refused);EXPECT_EQ(g.trace.destroyedInAudio.load(),0U);
        ASSERT_TRUE(sw.shutdownAfterBackendStopped());EXPECT_GE(g.dag.collectRetiredNodes(),1U);
    }
}
