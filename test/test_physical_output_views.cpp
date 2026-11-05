/**
 * @file test_physical_output_views.cpp
 * @brief Tests for physical output views.
 * @author Mika
 */
#include <array>
#include <concepts>
#include <span>
#include <gtest/gtest.h>
import mka.audio.dsp.dag;
#include "backend_routing_fixture.hpp"

namespace {
    using namespace mka::audio::dsp;
    using namespace backend_test;
    static_assert(std::same_as<decltype(OutputChannel::buffer),float*>);
    static_assert(std::same_as<decltype(ProcessContext::backendOutputs),std::span<const OutputChannel>>);

    /// @brief Test case: source output with successor writes hardware without changing logical production.
    TEST(PhysicalOutputViews, SourceOutputWithSuccessorWritesHardwareWithoutChangingLogicalProduction) {
        for (const std::uint32_t count : {0U,1U,2U,6U}) {
            std::array<Observation,3> seen;
            seen[0].expectOutput=true; seen[1].expectSource=false;
            seen[1].checkConstantInput=true; seen[1].constantInput=7.F;
            WritePolicy writer{.lanes=0x3FU,.physicalValue=100.F,.logicalValue=7.F};
            WritePolicy unmarked{.lanes=0x3FU,.physicalValue=-100.F};
            DAG graph;
            const auto source=graph.add<PhysicalWriter>(seen[0],writer);
            const auto consumer=graph.add<ProbeNode>(seen[1],Config{.layoutInput={0},.layoutOutput={0}});
            const auto other=graph.add<PhysicalWriter>(seen[2],unmarked);
            ASSERT_TRUE(source); ASSERT_TRUE(consumer); ASSERT_TRUE(other);
            ASSERT_TRUE(graph.connect(encode(*consumer,PortType::In,0),encode(*source,PortType::Out,0)));
            ASSERT_TRUE(graph.markAsOutput(*source));
            auto plan=graph.compile(BackendConfig{.maxFrames=128,.inputChannels=2,.outputChannels=count}); ASSERT_TRUE(plan);
            EXPECT_EQ(graph.connections().size(),1U);
            FakeBackend a,b(1000.F);
            for (auto* backend : {&a,&b}) {
                const auto io=backend->io(128,2,count); IOScope scope(io); plan->exec(io);
                for (std::uint32_t c=0;c<count;++c)
                    for (FrameCount f=0;f<128;++f) EXPECT_EQ(backend->outputs[c][f],100.F+c);
                EXPECT_TRUE(seen[1].inputSamplesIntact); // Check during the reader, before its slot can be recycled.
                for (const auto& o : seen) {
                    EXPECT_TRUE(o.directOutputs); EXPECT_TRUE(o.separateStorage); EXPECT_TRUE(o.physicalIDsZero);
                }
                EXPECT_EQ(seen[0].physicalOutputs,count); EXPECT_EQ(seen[1].physicalOutputs,0U); EXPECT_EQ(seen[2].physicalOutputs,0U);
                for (const auto& task : plan->executionTasks()) EXPECT_TRUE(task.context.backendOutputs.empty());
            }
        }
    }

    /// @brief Test case: marker snapshot and physical channel choice do not depend on edges.
    TEST(PhysicalOutputViews, MarkerSnapshotAndPhysicalChannelChoiceDoNotDependOnEdges) {
        Observation seen; seen.expectOutput=true;
        WritePolicy writer{.lanes=1U<<5,.physicalValue=20.F};
        DAG graph; const auto id=graph.add<PhysicalWriter>(seen,writer,Config{}); ASSERT_TRUE(id);
        ASSERT_TRUE(graph.markAsOutput(*id));
        const BackendConfig cfg{.maxFrames=128,.outputChannels=6};
        auto old=graph.compile(cfg); ASSERT_TRUE(old);
        ASSERT_TRUE(graph.unmarkAsOutput(*id));
        auto next=graph.compile(cfg); ASSERT_TRUE(next);
        FakeBackend backend;
        {
            const auto io=backend.io(128,0,6); IOScope scope(io); old->exec(io);
            EXPECT_EQ(seen.physicalOutputs,6U); EXPECT_TRUE(seen.directOutputs);
            for (unsigned c=0;c<6;++c) EXPECT_EQ(backend.outputs[c][0],c==5 ? 25.F : 0.F);
            backend.reset(); seen.expectOutput=false; next->exec(io);
            EXPECT_EQ(seen.physicalOutputs,0U); EXPECT_TRUE(seen.directOutputs);
            for (unsigned c=0;c<6;++c) EXPECT_EQ(backend.outputs[c][0],0.F);
        }
        EXPECT_TRUE(graph.connections().empty()); EXPECT_TRUE(next->executionTasks()[0].context.outputs.empty());
    }
}
