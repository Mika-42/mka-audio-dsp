#include <algorithm>
#include <array>
#include <cstddef>
#include <new>
#include <stdexcept>
#include <utility>
#include <vector>
#include <gtest/gtest.h>
import mka.audio.dsp.dag;
#include "pool_pipeline.hpp"

namespace {
    using namespace mka::audio::dsp;
    using mka::audio::AudioProcessContext;

    TEST(PoolIntegration, ChainUsesFourMonoSlotsWithStableBindingsAndExactStereoRender) {
        pool_test::Pipeline p;
        auto previous = p.add();
        for (unsigned i = 0; i < 30; ++i) {
            const auto next = p.add();
            p.edge(previous, next, PortType::In, 0, 0);
            p.edge(previous, next, PortType::In, 1, 1);
            previous = next;
        }
        ASSERT_TRUE(p.graph.markAsOutput(previous));
        BackendConfig cfg{.maxFrames=128,.inputChannels=2,.outputChannels=2};
        auto plan = p.graph.compile(cfg); ASSERT_TRUE(plan);
        cfg.slotStrategy = SlotStrategy::Unique;
        auto reference = p.graph.compile(cfg); ASSERT_TRUE(reference);
        EXPECT_EQ(plan->bufferSlotCount(), 4U);
        EXPECT_EQ(reference->bufferSlotCount(), 62U);
        const auto* tasks = plan->executionTasks().data();
        auto* source = tasks[0].context.outputs[0].buffer;
        std::array<std::array<float,130>,2> input{}, optimized{}, expected{};
        const std::array<const float*,2> inputs{input[0].data()+1,input[1].data()+1};
        const std::array outputs{optimized[0].data()+1,optimized[1].data()+1};
        const std::array oracle{expected[0].data()+1,expected[1].data()+1};
        for (FrameCount frames : {0U,1U,128U,17U,128U}) {
            for (auto& channel : input) channel.fill(2.F);
            for (auto& channel : optimized) channel.fill(-77.F);
            for (auto& channel : expected) channel.fill(-77.F);
            plan->exec(AudioProcessContext{inputs,outputs,frames});
            reference->exec(AudioProcessContext{inputs,oracle,frames});
            EXPECT_EQ(optimized, expected);
            for (const auto& channel : optimized) {
                EXPECT_EQ(channel[0], -77.F); EXPECT_EQ(channel[129], -77.F);
            }
            EXPECT_EQ(plan->executionTasks().data(), tasks);
            EXPECT_EQ(tasks[0].context.outputs[0].buffer, source);
        }
    }

    TEST(PoolIntegration, FanoutAuxMultipleSourcesOutputSuccessorAndExplicitDelayExecuteTheFinalOrder) {
        pool_test::Pipeline p;
        const auto a=p.add(), b=p.add(), early=p.add(), late=p.add(), sink=p.add();
        p.edge(a, early); p.edge(a, late, PortType::Aux);
        p.edge(b, late);
        const auto delayed = p.edge(early, sink);
        p.edge(late, sink, PortType::Aux);
        ASSERT_TRUE(p.graph.markAsOutput(early)); // Still produces logical storage for sink.
        ASSERT_TRUE(p.graph.markAsOutput(sink));
        const std::array delays{ConnectionDelay{delayed,3}};
        BackendConfig cfg{.maxFrames=128,.inputChannels=1,.outputChannels=1,.connectionDelays=delays};
        auto plan=p.graph.compile(cfg); ASSERT_TRUE(plan);
        cfg.slotStrategy=SlotStrategy::Unique;
        auto reference=p.graph.compile(cfg); ASSERT_TRUE(reference);
        EXPECT_EQ(plan->signalLifetimes().size(), 11U); // 10 Node outputs + delay production.
        EXPECT_EQ(plan->preparedMemoryUsage().historyBytes, 3U*sizeof(float));
        EXPECT_LE(plan->bufferSlotCount(), reference->bufferSlotCount());
        std::array<float,128> input{}, output{}, expected{};
        const std::array<const float*,1> inputs{input.data()};
        const std::array outputs{output.data()}, oracle{expected.data()};
        for (unsigned block=0;block<16;++block) {
            const FrameCount frames=block%3 == 0 ? 1 : 128;
            for (FrameCount f=0;f<128;++f) input[f]=static_cast<float>(block*128+f);
            plan->exec(AudioProcessContext{inputs,outputs,frames});
            reference->exec(AudioProcessContext{inputs,oracle,frames});
            for (FrameCount f=0;f<frames;++f) EXPECT_EQ(output[f],expected[f]);
        }
        ExecutionPlan copy=*plan;
        ExecutionPlan moved=std::move(copy); // Heap AudioState itself never moves.
        plan->exec(AudioProcessContext{inputs,outputs,128});
        moved.exec(AudioProcessContext{inputs,oracle,128});
        EXPECT_EQ(output,expected);
    }

    TEST(PoolIntegration, BudgetAndCapacityRefusalKeepActiveStorageAndExactBudgetSucceeds) {
        pool_test::Pipeline p;
        const auto a=p.add(), b=p.add();
        const auto edge=p.edge(a,b);
        const std::array delays{ConnectionDelay{edge,256}};
        BackendConfig cfg{.maxFrames=128,.outputChannels=1,.connectionDelays=delays};
        ASSERT_TRUE(p.graph.markAsOutput(b));
        auto plan=p.graph.compile(cfg); ASSERT_TRUE(plan);
        const auto bytes=plan->preparedMemoryUsage().totalBytes;
        auto* old=plan->executionTasks()[0].context.outputs[0].buffer;
        cfg.memoryBudgetBytes=bytes-1;
        auto result=plan->prepareBackend(cfg,p.graph.connections(),p.graph.outputNodes());
        ASSERT_FALSE(result); EXPECT_EQ(result.error().code,ErrorCode::MemoryBudgetExceeded);
        EXPECT_EQ(plan->executionTasks()[0].context.outputs[0].buffer,old);
        cfg.memoryBudgetBytes=bytes;
        ASSERT_TRUE(plan->prepareBackend(cfg,p.graph.connections(),p.graph.outputNodes()));
        cfg.maxBufferSlots=1;
        result=plan->prepareBackend(cfg,p.graph.connections(),p.graph.outputNodes());
        ASSERT_FALSE(result); EXPECT_EQ(result.error().code,ErrorCode::CapacityExceeded);
        auto invalid=delays;
        invalid[0].connection.to=encode(b,PortType::In,99);
        cfg.connectionDelays=invalid;
        result=plan->prepareBackend(cfg,p.graph.connections(),p.graph.outputNodes());
        ASSERT_FALSE(result); EXPECT_EQ(result.error().code,ErrorCode::InvalidParameter);
    }
}
