/**
 * @file test_last_writer_wins.cpp
 * @brief Tests for last writer wins.
 * @author Mika
 */
#include <array>
#include <expected>
#include <vector>
#include <gtest/gtest.h>
import mka.audio.dsp.dag;
#include "last_writer_fixture.hpp"

namespace {
    using namespace mka::audio::dsp;
    using namespace backend_test;

    /// @brief Test case: effective sample writes respect topology partial ranges and independent writers.
    TEST(LastWriterWins, EffectiveSampleWritesRespectTopologyPartialRangesAndIndependentWriters) {
        for (bool reversed : {false,true}) {
            std::array<Observation,5> seen;
            std::array<WritePolicy,4> policies;
            std::array<NodeID,5> ids;
            DAG graph;
            ASSERT_TRUE(buildLastWriters(graph,ids,seen,policies,reversed));
            auto plan=graph.compile(BackendConfig{.maxFrames=128,.inputChannels=2,.outputChannels=6}); ASSERT_TRUE(plan);
            std::vector<NodeID> order;
            for (const auto& task : plan->taskInfos()) order.push_back(task.id);
            EXPECT_EQ(order,(std::vector<NodeID>{ids[1],ids[0],ids[2],ids[3],ids[4]}));
            FakeBackend a,b;
            unsigned block=0;
            for (FrameCount frames : {128U,0U,1U,8U,128U}) for (auto* backend : {&a,&b}) {
                policies[1].physicalValue=10.F+100.F*block;
                policies[0].physicalValue=20.F+100.F*block;
                policies[2].physicalValue=30.F+100.F*block;
                ++block;
                const auto before=backend->outputStorage;
                const auto io=backend->io(frames,2,6); IOScope scope(io); plan->exec(io);
                for (unsigned c=0;c<6;++c) for (FrameCount f=0;f<130;++f) {
                    const auto expected=f>0 && f<=frames ? lastWriterSample(c,f-1,policies) : before[c][f];
                    EXPECT_EQ(backend->outputStorage[c][f],expected) << "channel=" << c << " offset=" << f;
                }
                for (unsigned i=0;i<5;++i) {
                    EXPECT_TRUE(seen[i].directOutputs); EXPECT_TRUE(seen[i].separateStorage);
                    EXPECT_EQ(seen[i].physicalOutputs,i<3 ? 6U : 0U);
                }
                EXPECT_TRUE(seen[4].inputSamplesIntact);
            }
            EXPECT_EQ(graph.connections().size(),2U);
            EXPECT_EQ(plan->callbackDiagnostics(),0U);
        }
    }
}
