#include <array>
#include <concepts>
#include <cstddef>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>

#include <gtest/gtest.h>

import mka.audio.dsp.dag;

namespace {
    using namespace mka::audio::dsp;
    static_assert(std::is_aggregate_v<ExecutionTask>);
    static_assert(std::is_standard_layout_v<ExecutionTask>);
    static_assert(std::is_trivially_copyable_v<ExecutionTask>);
    static_assert(!std::is_polymorphic_v<ExecutionTask>);
    static_assert(std::same_as<decltype(ExecutionTask::node), Node*>);
    static_assert(std::same_as<decltype(ExecutionTask::context), ProcessContext>);
    constexpr ExecutionTask empty{};
    static_assert(empty.node == nullptr && empty.context.frames == 0);
    // Exactly the two aggregate members required by the processing contract.
    constexpr bool twoMembers() {
        const auto [node, context] = ExecutionTask{};
        return node == nullptr && context.inputs.empty();
    }
    static_assert(twoMembers());

    struct Observation {
        const ExecutionTask* tasks = nullptr;
        std::array<NodeID, 2> order{};
        std::size_t count = 0;
        const float* input = nullptr;
        float* output = nullptr;
        FrameCount frames = 0;
        bool correct = true;
    };
    // Test-only synchronous observer; the Node stores no callback pointers.
    thread_local Observation* observation = nullptr;
    class TaskNode final : public Node {
    public:
        explicit TaskNode(NodeID id, unsigned* destroyed = nullptr)
            : Node(id, Config{.layoutInput={0},.layoutOutput={0}}), destroyed_(destroyed) {}
        ~TaskNode() override { if (destroyed_) ++*destroyed_; }
        void process(const ProcessContext& ctx) noexcept override {
            ++calls;
            if (observation) {
                auto& check = *observation;
                const auto index = check.count++;
                check.order[index] = id();
                check.correct = check.correct && &ctx == &check.tasks[index].context &&
                    ctx.frames == check.frames && ctx.backendOutputs.size() == 1 &&
                    ctx.backendOutputs[0].buffer == check.output;
                if (index == 0)
                    check.correct = check.correct && ctx.inputs.size() == 1 &&
                        ctx.inputs[0].buffer == check.input;
            }
            for (FrameCount i=0; i<ctx.frames; ++i) {
                const auto sample = ctx.inputs[0].buffer[i] + 1.F;
                ctx.outputs[0].buffer[i] = sample;
                for (const auto& out : ctx.backendOutputs) out.buffer[i] = sample;
            }
        }
        unsigned calls = 0;
    private:
        unsigned* destroyed_;
    };

    TEST(ExecutionTasks, StableOwnedContextsAreProcessedExactlyOnceEvenForZeroFrames) {
        DAG graph;
        const auto first=graph.add<TaskNode>(), second=graph.add<TaskNode>();
        ASSERT_TRUE(first); ASSERT_TRUE(second);
        ASSERT_TRUE(graph.connect(encode(*second,PortType::In,0),encode(*first,PortType::Out,0)));
        ASSERT_TRUE(graph.markAsOutput(*first)); ASSERT_TRUE(graph.markAsOutput(*second));
        auto plan=graph.compile(BackendConfig{.maxFrames=4,.inputChannels=1,.outputChannels=1});
        ASSERT_TRUE(plan);
        const auto tasks=plan->executionTasks(); ASSERT_EQ(tasks.size(),2U);
        const auto* descriptors=tasks[1].context.inputs.data();
        EXPECT_EQ(tasks[1].context.inputs[0].buffer,tasks[0].context.outputs[0].buffer);
        std::array<float,4> input{1,2,3,4}, alternate{5,6,7,8}, output{}, alternateOutput{};
        for (const FrameCount frames : {0U,1U,4U}) {
            const std::array<const float*,1> inputs{frames==1 ? alternate.data() : input.data()};
            const std::array outputs{frames==1 ? alternateOutput.data() : output.data()};
            Observation check{.tasks=tasks.data(),.input=inputs[0],.output=outputs[0],.frames=frames};
            observation=&check;
            const bool accepted=plan->exec(BackendBlock{inputs,outputs,frames});
            observation=nullptr;
            ASSERT_TRUE(accepted); EXPECT_TRUE(check.correct); EXPECT_EQ(check.count,2U);
            EXPECT_EQ(check.order,(std::array<NodeID,2>{*first,*second}));
            EXPECT_EQ(plan->executionTasks().data(),tasks.data());
            EXPECT_EQ(tasks[1].context.inputs.data(),descriptors);
            EXPECT_TRUE(tasks[0].context.inputs.empty());
            for (const auto& task : tasks) {
                EXPECT_NE(task.node,nullptr); EXPECT_TRUE(task.context.backendOutputs.empty());
                EXPECT_EQ(task.context.frames,0U);
            }
            for (FrameCount i=0;i<frames;++i) EXPECT_EQ(outputs[0][i],inputs[0][i]+2.F);
        }
        EXPECT_EQ(static_cast<TaskNode*>(graph.node(*first))->calls,3U);
        EXPECT_EQ(static_cast<TaskNode*>(graph.node(*second))->calls,3U);
    }

    TEST(ExecutionTasks, CopyAndMoveKeepInternalContextsBoundToTheirOwnDescriptors) {
        DAG graph;
        const auto first=graph.add<TaskNode>(), second=graph.add<TaskNode>();
        ASSERT_TRUE(first); ASSERT_TRUE(second);
        ASSERT_TRUE(graph.connect(encode(*second,PortType::In,0),encode(*first,PortType::Out,0)));
        auto original=graph.compile(BackendConfig{.maxFrames=2,.inputChannels=1}); ASSERT_TRUE(original);
        ExecutionPlan copy=*original;
        const auto a=original->executionTasks(), b=copy.executionTasks();
        EXPECT_NE(a[1].context.inputs.data(),b[1].context.inputs.data());
        EXPECT_NE(a[0].context.outputs[0].buffer,b[0].context.outputs[0].buffer);
        EXPECT_EQ(b[1].context.inputs[0].buffer,b[0].context.outputs[0].buffer);
        const auto* stable=b[1].context.inputs.data();
        ExecutionPlan moved=std::move(copy);
        EXPECT_TRUE(copy.executionTasks().empty());
        EXPECT_EQ(moved.executionTasks()[1].context.inputs.data(),stable);
        const float sample=9; const std::array inputs{&sample};
        ASSERT_TRUE(moved.exec(BackendBlock{.inputs=inputs,.frames=1}));
        EXPECT_EQ(moved.executionTasks()[1].context.outputs[0].buffer[0],11.F);
        EXPECT_EQ(a[1].context.outputs[0].buffer[0],0.F);
    }

    TEST(ExecutionTasks, RemovedNodesSurviveEveryBorrowingGenerationUntilControlRetirement) {
        unsigned destroyed=0;
        DAG graph;
        const auto id=graph.add<TaskNode>(&destroyed); ASSERT_TRUE(id);
        auto active=graph.compile(BackendConfig{.maxFrames=1,.inputChannels=1}); ASSERT_TRUE(active);
        std::optional<ExecutionPlan> pending{*active};
        ASSERT_TRUE(graph.remove(*id));
        EXPECT_EQ(graph.node(*id),nullptr); EXPECT_EQ(graph.collectRetiredNodes(),0U);
        const float input=1; const std::array inputs{&input};
        ASSERT_TRUE(active->exec(BackendBlock{.inputs=inputs,.frames=1}));
        ASSERT_TRUE(pending->exec(BackendBlock{.inputs=inputs,.frames=1}));
        EXPECT_EQ(destroyed,0U);
        *active=ExecutionPlan{}; EXPECT_EQ(graph.collectRetiredNodes(),0U);
        pending.reset(); EXPECT_EQ(graph.collectRetiredNodes(),1U); EXPECT_EQ(destroyed,1U);
    }
}
