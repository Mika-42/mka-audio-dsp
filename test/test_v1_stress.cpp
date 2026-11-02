#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iostream>
#include <span>
#include <thread>
#include <utility>
#include <vector>
#include <gtest/gtest.h>
import mka.audio.dsp.plan_switcher;
#include "v1_fixture.hpp"
namespace {
    using namespace mka::audio::dsp;
    TEST(V1Stress, TenThousandLatencyEditsPublicationsAndRetirementsAcrossTwoThreads) {
        v1_test::Graph g; ASSERT_TRUE(g.make(0)); PlanSwitcher sw;
        BackendConfig config{.maxFrames=128,.inputChannels=2,.outputChannels=6,.sampleRate=48000};
        ASSERT_TRUE(sw.initialize(g.dag,config));
        struct Command { GenerationID generation=1; unsigned delay=0,out=6; FrameCount frames=7; bool invalid=false; } cmd;
        std::atomic<unsigned> requested{0},completed{0}; std::atomic<bool> running{true};
        std::atomic<unsigned> failures{0};
        std::thread audio([&] {
            v1_test::Bank bank; unsigned ticket=0; GenerationID last=0; std::uint64_t relative=0,absolute=0;
            while (running.load(std::memory_order_acquire)) {
                const auto next=requested.load(std::memory_order_acquire);
                if (next==ticket) { std::this_thread::yield(); continue; }
                const auto work=cmd; bank.fill(work.invalid ? 128:work.frames);
                v1_test::callback(sw,bank.io(work.invalid ? 129:work.frames,work.out));
                const auto generation=sw.audioGeneration(); bool ok=generation==work.generation;
                if (last!=generation) { relative=0; last=generation; }
                const auto frames=work.invalid ? 128U:work.frames;
                for (unsigned c=0;c<work.out;++c) {
                    for (unsigned f=0;f<frames;++f) {
                        float expected=0.F;
                        if (!work.invalid && relative+f>=work.delay) {
                            if (c==0 || c==2) expected=1.F;
                            if (c==1 || c==3) expected=-2.F;
                            if (c==0 && (absolute+f)%2==0) expected*=2;
                        }
                        ok=ok && bank.output[c][f+1]==expected;
                    }
                    ok=ok && bank.output[c].front()==-999.F && bank.output[c].back()==-999.F;
                }
                if (!work.invalid) { relative+=work.frames; absolute+=work.frames; }
                if (!ok) failures.fetch_add(1);
                ticket=next; completed.store(ticket,std::memory_order_release);
            }
        });
        unsigned tickets=0,step=0,cycles=0; NodeID node=0; std::uint32_t seed=0x10CAFE42U; bool ok=true;
        const auto send=[&](Command command) {
            cmd=command; requested.store(++tickets,std::memory_order_release);
        };
        const auto wait=[&] {
            const auto until=std::chrono::steady_clock::now()+std::chrono::seconds(10);
            while (completed.load(std::memory_order_acquire)!=tickets) {
                if (std::chrono::steady_clock::now()>until) return false;
                std::this_thread::yield();
            }
            return failures.load()==0;
        };
        Command active;
        for (unsigned i=0;i<10000 && ok;++i) {
            seed=seed*1664525U+1013904223U;
            const unsigned delay=std::array<unsigned,4>{0,64,256,512}[seed%4];
            active.frames=std::array<FrameCount,4>{0,1,7,64}[(seed>>8)%4]; active.invalid=i%31==0;
            send(active); // A renders while control edits/compiles an independent B.
            step=1; node=g.slow; ok=g.replaceSlow(delay).has_value();
            auto next=config; next.outputChannels=i%2 ? 2:6; next.maxFrames=i%2 ? 64:128;
            if (ok && i%17==0) { auto bad=next; bad.memoryBudgetBytes=0; const auto r=sw.prepareInactivePlan(g.dag,bad); ok=!r && r.error().code==ErrorCode::MemoryBudgetExceeded; }
            step=2; if (ok) ok=sw.prepareInactivePlan(g.dag,next).has_value();
            if (!wait()) ok=false;
            if (!ok) break;
            const auto generation=sw.readyGeneration(); if (!generation) { ok=false; break; }
            step=3; ok=sw.publishReadyPlan().has_value();
            if (ok) ok=sw.handshakeState()==HandshakeState::Pending && !sw.controlGeneration();
            // Driver is intentionally idle here; an edit cannot alter Pending B.
            if (i%5==0) {
                const auto extra=g.dag.add<v1_test::Processor>(v1_test::Role::Extra,g.trace);
                if (!extra || !g.dag.remove(*extra)) ok=false;
            }
            if (ok) { const auto busy=sw.prepareInactivePlan(g.dag,next); ok=!busy && busy.error().code==ErrorCode::PlanBusy; }
            if (!ok) break;
            active={*generation,delay,next.outputChannels,std::array<FrameCount,4>{0,1,7,64}[i%4],false};
            send(active); step=4; ok=wait();
            if (ok) { step=5; ok=sw.retireAcknowledgedPlan().has_value(); }
            (void)g.dag.collectRetiredNodes(); ++cycles;
        }
        running.store(false,std::memory_order_release); audio.join();
        const auto final=sw.controlGeneration(); ASSERT_TRUE(sw.shutdownAfterBackendStopped());
        (void)g.dag.collectRetiredNodes();
        for (const auto id : {g.writer,g.router,g.mix,g.slow,g.gain,g.source}) ASSERT_TRUE(g.dag.remove(id));
        (void)g.dag.collectRetiredNodes();
        SCOPED_TRACE(::testing::Message()<<"seed=0x10CAFE42 cycles="<<cycles<<" step="<<step<<" NodeID="<<node<<" generation="<<final.value_or(0));
        EXPECT_TRUE(ok); EXPECT_EQ(cycles,10000U); EXPECT_EQ(failures.load(),0U);
        EXPECT_EQ(g.trace.destroyedInAudio.load(),0U); EXPECT_EQ(g.trace.created.load(),g.trace.destroyed.load());
        std::cout<<"seed=0x10CAFE42 cycles="<<cycles<<" callbacks="<<tickets<<" generation="<<final.value_or(0)<<" created="<<g.trace.created.load()<<" destroyed="<<g.trace.destroyed.load()<<'\n';
    }
}
