/**
 * @file test_prepared_plan.cpp
 * @brief Tests for prepared plan.
 * @author Mika
 */
#include <array>
#include <concepts>
#include <cstddef>
#include <type_traits>
#include <utility>
#include <gtest/gtest.h>

import mka.audio.dsp.dag;
import mka.audio.process_context;

namespace {
    using namespace mka::audio::dsp;
    using mka::audio::AudioProcessContext;
    static_assert(std::is_final_v<ExecutionPlan> && !std::is_polymorphic_v<ExecutionPlan>);
    static_assert(std::same_as<decltype(static_cast<void(ExecutionPlan::*)(
        const AudioProcessContext&) noexcept>(&ExecutionPlan::exec)),
        void(ExecutionPlan::*)(const AudioProcessContext&) noexcept>);
    static_assert(noexcept(std::declval<const ExecutionPlan&>().references(NodeID{})));
    static_assert(std::same_as<decltype(std::declval<const ExecutionPlan&>().references(NodeID{})),bool>);
    static_assert(std::is_aggregate_v<AudioProcessContext>);

    /// @brief Records calls made through a prepared execution plan.
    class PreparedNode final : public Node {
    public:
        /// @brief Declare the sparse logical output used to observe stable prepared bindings.
        explicit PreparedNode(NodeID id) : Node(id,Config{.layoutOutput={42}}) {}
        /// @brief Double the first input, or produce unity, into logical and physical output buffers.
        void process(const ProcessContext& context) noexcept override {
            ++calls;
            for (FrameCount i=0;i<context.frames;++i) {
                const float sample=context.inputs.empty() ? 1.F : context.inputs[0].buffer[i]*2.F;
                context.outputs[0].buffer[i]=sample;
                for (const auto& output : context.backendOutputs) output.buffer[i]=sample;
            }
        }
        /// @brief Number of processor invocations observed by the fixture.
        unsigned calls=0;
    };

    /// @brief Test case: canonical callbacks keep stable tasks descriptors and internal samples.
    TEST(PreparedPlan, CanonicalCallbacksKeepStableTasksDescriptorsAndInternalSamples) {
        DAG graph;
        const auto id=graph.add<PreparedNode>(); ASSERT_TRUE(id);
        ASSERT_TRUE(graph.markAsOutput(*id));
        auto plan=graph.compile(BackendConfig{.maxFrames=4,.inputChannels=1,.outputChannels=2}); ASSERT_TRUE(plan);
        const auto* operations=plan->executionTasks().data();
        const auto* descriptors=operations[0].context.outputs.data();
        auto* samples=descriptors[0].buffer;
        std::array<float,4> input{1,2,3,4}, first{}, second{};
        for (unsigned block=0;block<8;++block) {
            const std::array<const float*,1> inputs{input.data()};
            const std::array outputs{block%2 ? first.data() : second.data(),block%2 ? second.data() : first.data()};
            plan->exec(AudioProcessContext{inputs,outputs,static_cast<FrameCount>(block%5)});
            EXPECT_EQ(plan->executionTasks().data(),operations);
            EXPECT_EQ(operations[0].context.outputs.data(),descriptors);
            EXPECT_EQ(descriptors[0].buffer,samples);
            EXPECT_TRUE(operations[0].context.inputs.empty());
            EXPECT_TRUE(operations[0].context.backendOutputs.empty());
            for (FrameCount i=0;i<block%5;++i) EXPECT_EQ(outputs[0][i],input[i]*2.F);
        }
        EXPECT_EQ(static_cast<PreparedNode*>(graph.node(*id))->calls,8U);
    }

    /// @brief Test case: references are generation membership rather than desired graph membership.
    TEST(PreparedPlan, ReferencesAreGenerationMembershipRatherThanDesiredGraphMembership) {
        DAG graph;
        ExecutionPlan free;
        EXPECT_FALSE(free.references(0)); EXPECT_FALSE(free.references(1));
        const auto first=graph.add<PreparedNode>(), second=graph.add<PreparedNode>();
        ASSERT_TRUE(first); ASSERT_TRUE(second);
        auto old=graph.compile(BackendConfig{.maxFrames=1}); ASSERT_TRUE(old);
        ASSERT_TRUE(graph.remove(*first));
        auto replacement=graph.compile(BackendConfig{.maxFrames=1}); ASSERT_TRUE(replacement);
        EXPECT_TRUE(old->references(*first)); EXPECT_TRUE(old->references(*second));
        EXPECT_FALSE(replacement->references(*first)); EXPECT_TRUE(replacement->references(*second));
        EXPECT_FALSE(old->references(invalidNodeID)); EXPECT_FALSE(old->references(maxNodeID));
        ExecutionPlan moved=std::move(*old);
        EXPECT_TRUE(moved.references(*first)); EXPECT_FALSE(old->references(*first));
        EXPECT_EQ(graph.collectRetiredNodes(),0U);
        moved=ExecutionPlan{};
        EXPECT_EQ(graph.collectRetiredNodes(),1U);
    }

    /// @brief Test case: failed audio budget preserves old buffers contexts and executable generation.
    TEST(PreparedPlan, FailedAudioBudgetPreservesOldBuffersContextsAndExecutableGeneration) {
        DAG graph;
        const auto id=graph.add<PreparedNode>(); ASSERT_TRUE(id);
        ASSERT_TRUE(graph.markAsOutput(*id));
        auto plan=graph.compile(BackendConfig{.maxFrames=1,.outputChannels=1}); ASSERT_TRUE(plan);
        const auto tasks=plan->executionTasks();
        auto* const samples=tasks[0].context.outputs[0].buffer;
        const auto refused=plan->prepareBackend(BackendConfig{.maxFrames=4,.outputChannels=1,.memoryBudgetBytes=0},
            graph.connections(),graph.outputNodes());
        ASSERT_FALSE(refused); EXPECT_EQ(refused.error().code,ErrorCode::MemoryBudgetExceeded);
        EXPECT_TRUE(plan->isBackendPrepared()); EXPECT_EQ(plan->executionTasks().data(),tasks.data());
        EXPECT_EQ(tasks[0].context.outputs[0].buffer,samples);
        EXPECT_EQ(static_cast<PreparedNode*>(graph.node(*id))->calls,0U);
        float output=99; const std::array outputs{&output};
        plan->exec(AudioProcessContext{.outputs=outputs,.frames=1});
        EXPECT_EQ(output,1.F);
        auto rejected=graph.compile(BackendConfig{.maxFrames=1,.memoryBudgetBytes=0});
        ASSERT_FALSE(rejected); EXPECT_EQ(rejected.error().code,ErrorCode::MemoryBudgetExceeded);
        EXPECT_TRUE(plan->references(*id));
    }
}
