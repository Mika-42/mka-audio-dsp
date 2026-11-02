#include <array>
#include <gtest/gtest.h>
import mka.audio.dsp.dag;
#include "pair_routing_fixture.hpp"

namespace {
    using namespace mka::audio::dsp;
    using namespace backend_test;

    TEST(TwoSixRouting, NodePoliciesSelectPairsWithoutHardwareChannelsOrEdgesInTheEngine) {
        for (std::uint32_t outputs : {2U,6U}) {
            std::array<NodeID,3> ids;
            std::array<PairRouting,3> routing;
            std::array<Observation,3> seen;
            DAG graph; ASSERT_TRUE(buildPairRouting(graph,ids,routing,seen,outputs));
            auto plan=graph.compile(BackendConfig{.maxFrames=128,.inputChannels=6,.outputChannels=outputs,.sampleRate=48000});
            ASSERT_TRUE(plan); EXPECT_EQ(graph.connections().size(),2U);
            FakeBackend a(1.F),b(1000.F);
            const auto inputA=a.inputStorage,inputB=b.inputStorage;
            for (FrameCount frames : {0U,1U,128U,7U,128U}) for (auto* backend : {&a,&b}) {
                const auto before=backend->outputStorage;
                const auto io=backend->io(frames,6,outputs); IOScope scope(io); plan->exec(io);
                for (unsigned c=0;c<6;++c) for (FrameCount f=0;f<130;++f) {
                    const float expected=c<outputs && f>0 && f<=frames ? routedPairSample(outputs,c,f-1,*backend) : before[c][f];
                    EXPECT_EQ(backend->outputStorage[c][f],expected) << "channel=" << c << " offset=" << f;
                }
            }
            EXPECT_EQ(a.inputStorage,inputA); EXPECT_EQ(b.inputStorage,inputB);
            for (const auto& o : seen) {
                EXPECT_TRUE(o.directInputs); EXPECT_TRUE(o.directOutputs); EXPECT_TRUE(o.separateStorage);
                EXPECT_TRUE(o.physicalIDsZero); EXPECT_EQ(o.calls,10U);
            }
        }
    }

    TEST(TwoSixRouting, NodeChecksUnavailableInputAndOutputIndicesBeforeAccess) {
        PairRouting route; route.inputs={1,5}; route.destinations[0]={0,65535};
        Observation seen; seen.expectOutput=true;
        DAG graph; const auto id=graph.add<PairRouter>(route,seen); ASSERT_TRUE(id);
        ASSERT_TRUE(graph.markAsOutput(*id));
        auto plan=graph.compile(BackendConfig{.maxFrames=128,.inputChannels=1,.outputChannels=2}); ASSERT_TRUE(plan);
        FakeBackend backend;
        const auto io=backend.io(128,1,2); IOScope scope(io); plan->exec(io);
        for (unsigned c=0;c<2;++c) for (FrameCount f=0;f<128;++f) EXPECT_EQ(backend.outputs[c][f],0.F);
        EXPECT_EQ(backend.outputStorage[0].front(),guard); EXPECT_EQ(backend.outputStorage[1].back(),guard);
        EXPECT_EQ(plan->callbackDiagnostics(),0U); // Missing selected lanes are handled by this Node's policy.
    }
}
