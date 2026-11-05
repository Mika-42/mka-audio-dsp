/**
 * @file test_delay_task.cpp
 * @brief Tests for delay task.
 * @author Mika
 */
#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>
#include <gtest/gtest.h>
import mka.audio.dsp.dag;

namespace {
    using namespace mka::audio::dsp;
    using detail::DelayTask;
    static_assert(!std::is_polymorphic_v<DelayTask> && !std::is_base_of_v<Node,DelayTask>);
    static_assert(std::same_as<decltype(DelayTask{}.source),const float*>);
    static_assert(std::same_as<decltype(DelayTask{}.destination),float*>);
    static_assert(std::same_as<decltype(DelayTask{}.delaySamples),std::uint64_t>);
    static_assert(std::same_as<decltype(DelayTask{}.line),DelayLine>);
    static_assert(std::same_as<decltype(&DelayTask::execute),void (DelayTask::*)(FrameCount) noexcept>);
    static_assert(std::is_nothrow_move_constructible_v<DelayTask>);

    /// @brief Forwards one logical signal to its output without changing its samples.
    class ForwardNode final : public Node {
    public:
        /// @brief Declare one main input and logical output for pass-through rendering.
        explicit ForwardNode(NodeID id) : Node(id,Config{.layoutInput={0},.layoutOutput={0}}) {}
        /// @brief Forward the first main input into all logical and physical outputs.
        void process(const ProcessContext& ctx) noexcept override {
            for (FrameCount f=0;f<ctx.frames;++f) {
                const auto value=ctx.inputs.empty() ? 0.F : ctx.inputs[0].buffer[f];
                for (const auto& output : ctx.outputs) output.buffer[f]=value;
                for (const auto& output : ctx.backendOutputs) output.buffer[f]=value;
            }
        }
    };

    /// @brief Test case: positive delays persist and render exactly across many short blocks.
    TEST(DelayTask, PositiveDelaysPersistAndRenderExactlyAcrossManyShortBlocks) {
        for (const std::uint64_t delay : {1U,3U,256U}) {
            SCOPED_TRACE(delay); DelayTask task; ASSERT_TRUE(task.prepare(delay));
            std::array<float,4> input{},output{};
            ASSERT_TRUE(task.bind(input.data(),output.data(),4));
            const auto* const source=task.source; auto* const destination=task.destination;
            for (unsigned block=0;block<70;++block) {
                for (unsigned f=0;f<4;++f) input[f]=static_cast<float>(block*4+f+1);
                const auto before=input; task.execute(4);
                for (unsigned f=0;f<4;++f) {
                    const auto n=block*4+f;
                    EXPECT_EQ(output[f],n<delay ? 0.F : static_cast<float>(n-delay+1))<<"n="<<n;
                }
                EXPECT_EQ(input,before); EXPECT_EQ(task.source,source); EXPECT_EQ(task.destination,destination);
            }
        }
    }

    /// @brief Test case: reference blocks share the same history including zero frame executions.
    TEST(DelayTask, ReferenceBlocksShareTheSameHistoryIncludingZeroFrameExecutions) {
        DelayTask task; ASSERT_TRUE(task.prepare(3)); std::array<float,4> input{1,2,3,4},output{};
        ASSERT_TRUE(task.bind(input.data(),output.data(),4));
        task.execute(4); EXPECT_EQ(output,(std::array<float,4>{0,0,0,1}));
        task.execute(0); input={5,6,7,8}; task.execute(4);
        EXPECT_EQ(output,(std::array<float,4>{2,3,4,5}));
    }

    /// @brief Test case: invalid null identical partial and overflowing bindings preserve the old views and history.
    TEST(DelayTask, InvalidNullIdenticalPartialAndOverflowingBindingsPreserveTheOldViewsAndHistory) {
        DelayTask task; ASSERT_TRUE(task.prepare(3));
        std::array<float,4> input{1,2,3,4},output{}; std::array<float,8> shared{};
        ASSERT_TRUE(task.bind(input.data(),output.data(),4)); task.execute(4);
        EXPECT_EQ(task.bind(input.data(),input.data(),4).error().code,ErrorCode::InvalidParameter);
        EXPECT_EQ(task.bind(shared.data(),shared.data()+1,4).error().code,ErrorCode::InvalidParameter);
        EXPECT_EQ(task.bind(shared.data()+1,shared.data(),4).error().code,ErrorCode::InvalidParameter);
        EXPECT_EQ(task.bind(nullptr,output.data(),4).error().code,ErrorCode::InvalidParameter);
        EXPECT_EQ(task.bind(input.data(),nullptr,4).error().code,ErrorCode::InvalidParameter);
        const auto impossible=std::numeric_limits<std::uintptr_t>::max()-1;
        EXPECT_EQ(task.bind(reinterpret_cast<const float*>(impossible),output.data(),4).error().code,ErrorCode::ArithmeticOverflow);
        EXPECT_EQ(task.bind(input.data(),reinterpret_cast<float*>(impossible),4).error().code,ErrorCode::ArithmeticOverflow);
        EXPECT_EQ(task.source,input.data()); EXPECT_EQ(task.destination,output.data());
        input={5,6,7,8}; task.execute(4); EXPECT_EQ(output,(std::array<float,4>{2,3,4,5}));
        EXPECT_TRUE(detail::validateDelayBinding(shared.data(),shared.data()+4,4));
        EXPECT_TRUE(detail::validateDelayBinding(shared.data()+4,shared.data(),4));
    }

    /// @brief Test case: rejected preparation preserves both declared delay and running history.
    TEST(DelayTask, RejectedPreparationPreservesBothDeclaredDelayAndRunningHistory) {
        DelayTask task; std::array<float,4> input{1,2,3,4},output{};
        EXPECT_EQ(task.bind(input.data(),output.data(),4).error().code,ErrorCode::InvalidParameter);
        EXPECT_EQ(task.prepare(0).error().code,ErrorCode::InvalidParameter);
        ASSERT_TRUE(task.prepare(3)); ASSERT_TRUE(task.bind(input.data(),output.data(),4)); task.execute(4);
        EXPECT_EQ(task.prepare(0).error().code,ErrorCode::InvalidParameter);
        EXPECT_EQ(task.prepare(256,DelayLineLimits{.memoryBudgetBytes=1023}).error().code,ErrorCode::MemoryBudgetExceeded);
        EXPECT_EQ(task.prepare(UINT64_MAX).error().code,ErrorCode::ArithmeticOverflow);
        EXPECT_EQ(task.delaySamples,3U); EXPECT_EQ(task.line.delaySamples(),3U);
        input={5,6,7,8}; task.execute(4); EXPECT_EQ(output,(std::array<float,4>{2,3,4,5}));
    }

    /// @brief Test case: copies and moves own independent history and can rebind their borrowed views.
    TEST(DelayTask, CopiesAndMovesOwnIndependentHistoryAndCanRebindTheirBorrowedViews) {
        DelayTask original; ASSERT_TRUE(original.prepare(3));
        std::array<float,4> input{1,2,3,4},a{},b{};
        ASSERT_TRUE(original.bind(input.data(),a.data(),4)); original.execute(4);
        auto copy=original; ASSERT_TRUE(copy.bind(input.data(),b.data(),4));
        input={5,6,7,8}; original.execute(4); copy.execute(4); EXPECT_EQ(a,b);
        auto moved=std::move(copy); input={9,10,11,12}; moved.execute(4);
        EXPECT_EQ(b,(std::array<float,4>{6,7,8,9}));
        EXPECT_FALSE(copy.bind(input.data(),a.data(),4)); // Moved-from task needs preparation again.
        ASSERT_TRUE(copy.prepare(1)); ASSERT_TRUE(copy.bind(input.data(),a.data(),4)); copy.execute(4);
        EXPECT_EQ(a,(std::array<float,4>{0,9,10,11}));
    }

    /// @brief Test case: zero capacity permits null views but no nonempty callback can use them.
    TEST(DelayTask, ZeroCapacityPermitsNullViewsButNoNonemptyCallbackCanUseThem) {
        DelayTask task; ASSERT_TRUE(task.prepare(256)); ASSERT_TRUE(task.bind(nullptr,nullptr,0));
        task.execute(0); EXPECT_EQ(task.line.delaySamples(),256U);
        EXPECT_FALSE(task.bind(nullptr,nullptr,1));
    }

    /// @brief Test case: plan copies rebind to independent pools and wrapper moves keep stable internal pointers.
    TEST(DelayTask, PlanCopiesRebindToIndependentPoolsAndWrapperMovesKeepStableInternalPointers) {
        DAG graph; const auto source=graph.add<ForwardNode>(),sink=graph.add<ForwardNode>();
        ASSERT_TRUE(source); ASSERT_TRUE(sink); ASSERT_TRUE(graph.markAsOutput(*sink));
        const Connection edge{encode(*source,PortType::Out,0),encode(*sink,PortType::In,0)};
        ASSERT_TRUE(graph.connect(edge.to,edge.from));
        const std::array delays{ConnectionDelay{edge,3}};
        const BackendConfig cfg{.maxFrames=4,.inputChannels=1,.outputChannels=1,.connectionDelays=delays};
        auto plan=graph.compile(cfg); ASSERT_TRUE(plan);
        std::array<float,4> input{1,2,3,4},a{},b{};
        const std::array<const float*,1> inputs{input.data()};
        const std::array first{a.data()},second{b.data()};
        ASSERT_TRUE(plan->exec(BackendBlock{inputs,first,4})); EXPECT_EQ(a,(std::array<float,4>{0,0,0,1}));
        ExecutionPlan copy=*plan;
        EXPECT_NE(copy.executionTasks()[1].context.inputs[0].buffer,plan->executionTasks()[1].context.inputs[0].buffer);
        input={5,6,7,8}; ASSERT_TRUE(plan->exec(BackendBlock{inputs,first,4}));
        ASSERT_TRUE(copy.exec(BackendBlock{inputs,second,4})); EXPECT_EQ(a,b); EXPECT_EQ(a,(std::array<float,4>{2,3,4,5}));
        const auto* stable=copy.executionTasks()[1].context.inputs[0].buffer;
        ExecutionPlan moved=std::move(copy);
        EXPECT_EQ(moved.executionTasks()[1].context.inputs[0].buffer,stable);
        input={9,10,11,12}; ASSERT_TRUE(moved.exec(BackendBlock{inputs,second,4}));
        EXPECT_EQ(b,(std::array<float,4>{6,7,8,9}));
        EXPECT_NE(stable,input.data()); EXPECT_NE(stable,a.data()); EXPECT_NE(stable,b.data());
    }

    /// @brief Test case: zero requested delay remains A direct plan binding without task or history.
    TEST(DelayTask, ZeroRequestedDelayRemainsADirectPlanBindingWithoutTaskOrHistory) {
        DAG graph; const auto source=graph.add<ForwardNode>(),sink=graph.add<ForwardNode>(); ASSERT_TRUE(source); ASSERT_TRUE(sink);
        const Connection edge{encode(*source,PortType::Out,0),encode(*sink,PortType::In,0)};
        ASSERT_TRUE(graph.connect(edge.to,edge.from));
        const std::array delays{ConnectionDelay{edge,0}};
        auto plan=graph.compile(BackendConfig{.maxFrames=4,.connectionDelays=delays}); ASSERT_TRUE(plan);
        EXPECT_EQ(plan->delayTaskCount(),0U); EXPECT_EQ(plan->preparedMemoryUsage().historyBytes,0U);
        EXPECT_EQ(plan->executionTasks()[1].context.inputs[0].buffer,plan->executionTasks()[0].context.outputs[0].buffer);
    }
}
