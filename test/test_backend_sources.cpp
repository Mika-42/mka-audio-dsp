/**
 * @file test_backend_sources.cpp
 * @brief Tests for backend sources.
 * @author Mika
 */
#include <array>
#include <vector>
#include <gtest/gtest.h>
import mka.audio.dsp.dag;
#include "backend_routing_fixture.hpp"

namespace {
    using namespace mka::audio::dsp;
    using namespace backend_test;

    /// @brief Test case: compile roles from all incoming edges independently of layouts and output markers.
    TEST(BackendSources, CompileRolesFromAllIncomingEdgesIndependentlyOfLayoutsAndOutputMarkers) {
        std::array<Observation,6> seen;
        DAG graph;
        std::array<NodeID,6> ids{};
        const std::array configs{
            Config{.layoutOutput={0}}, Config{.layoutOutput={0}},
            Config{.layoutAuxiliary={33},.layoutOutput={0}}, Config{.layoutOutput={0}},
            Config{.layoutInput={42},.layoutAuxiliary={31},.layoutOutput={0}}, Config{}};
        for (std::size_t i=0;i<ids.size();++i) {
            const auto id=graph.add<ProbeNode>(seen[i],configs[i]); ASSERT_TRUE(id); ids[i]=*id;
        }
        ASSERT_TRUE(graph.connect(encode(ids[2],PortType::Aux,33),encode(ids[1],PortType::Out,0)));
        ASSERT_TRUE(graph.connect(encode(ids[4],PortType::In,42),encode(ids[1],PortType::Out,0)));
        ASSERT_TRUE(graph.connect(encode(ids[4],PortType::Aux,31),encode(ids[0],PortType::Out,0)));
        ASSERT_TRUE(graph.connect(encode(ids[5],PortType::Aux,65535),encode(ids[3],PortType::Out,65535)));
        ASSERT_TRUE(graph.markAsOutput(ids[3])); ASSERT_TRUE(graph.markAsOutput(ids[4]));
        auto plan=graph.compile(); ASSERT_TRUE(plan);
        EXPECT_EQ(std::vector<std::size_t>(plan->sourceTasks().begin(),plan->sourceTasks().end()),
                  (std::vector<std::size_t>{0,1,3}));
        EXPECT_EQ(std::vector<std::size_t>(plan->outputTasks().begin(),plan->outputTasks().end()),
                  (std::vector<std::size_t>{3,4}));
        EXPECT_EQ(graph.connections().size(),4U); EXPECT_EQ(plan->taskInfos().size(),6U);
        for (const auto& task : plan->taskInfos()) {
            EXPECT_EQ(task.isSource,task.id==ids[0] || task.id==ids[1] || task.id==ids[3]);
            EXPECT_EQ(task.isOutput,task.id==ids[3] || task.id==ids[4]);
        }
        ASSERT_TRUE(plan->prepareBackend(BackendConfig{.maxFrames=128,.inputChannels=6,.outputChannels=2},
                                        graph.connections(),graph.outputNodes()));
        const auto* sourceIndices=plan->sourceTasks().data();
        for (std::size_t i=0;i<ids.size();++i) {
            seen[i].expectSource=i==0 || i==1 || i==3;
            seen[i].expectOutput=i==3 || i==4;
        }
        FakeBackend first(1.F),second(100.F);
        for (auto* backend : {&first,&second}) {
            const auto io=backend->io(128,6,2); IOScope scope(io); plan->exec(io);
            for (std::size_t i=0;i<ids.size();++i) {
                EXPECT_EQ(seen[i].inputs,seen[i].expectSource ? 6U : configs[i].layoutInput.size());
                EXPECT_EQ(seen[i].physicalOutputs,seen[i].expectOutput ? 2U : 0U);
                EXPECT_TRUE(seen[i].directInputs); EXPECT_TRUE(seen[i].directOutputs);
            }
            EXPECT_EQ(plan->sourceTasks().data(),sourceIndices);
        }
    }

    /// @brief Test case: desired graph edits and failed preparation never change an old source list.
    TEST(BackendSources, DesiredGraphEditsAndFailedPreparationNeverChangeAnOldSourceList) {
        std::array<Observation,2> seen;
        DAG graph;
        const auto a=graph.add<ProbeNode>(seen[0],Config{.layoutOutput={0}});
        const auto b=graph.add<ProbeNode>(seen[1],Config{.layoutOutput={0}});
        ASSERT_TRUE(a); ASSERT_TRUE(b);
        const BackendConfig config{.maxFrames=1,.inputChannels=2};
        auto old=graph.compile(config); ASSERT_TRUE(old);
        const auto* indices=old->sourceTasks().data();
        ASSERT_TRUE(graph.connect(encode(*a,PortType::Aux,65535),encode(*b,PortType::Out,65535)));
        auto next=graph.compile(config); ASSERT_TRUE(next);
        EXPECT_EQ(old->sourceTasks().size(),2U); EXPECT_EQ(next->sourceTasks().size(),1U);
        EXPECT_EQ(next->taskInfos()[next->sourceTasks()[0]].id,*b);
        const std::array unknown{maxNodeID};
        const auto refused=old->prepareBackend(config,graph.connections(),unknown); ASSERT_FALSE(refused);
        EXPECT_EQ(old->sourceTasks().data(),indices); EXPECT_EQ(old->sourceTasks().size(),2U);
        FakeBackend backend;
        const auto io=backend.io(1,2,0); IOScope scope(io); old->exec(io);
        EXPECT_EQ(seen[0].inputs,2U); EXPECT_EQ(seen[1].inputs,2U);
        EXPECT_TRUE(seen[0].directInputs); EXPECT_TRUE(seen[1].directInputs);
    }
}
