/**
 * @file test_node_lifetime.cpp
 * @brief Tests for node lifetime.
 * @author Mika
 */
#include <cstddef>
#include <optional>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

import mka.audio.dsp.node;
import mka.audio.dsp.channel;
import mka.audio.dsp.process_context;
import mka.audio.dsp.execution_plan;
import mka.audio.dsp.dag;

namespace {
    using namespace mka::audio::dsp;

    static_assert(std::is_copy_constructible_v<ExecutionPlan>);
    static_assert(std::is_copy_assignable_v<ExecutionPlan>);
    static_assert(std::is_nothrow_move_constructible_v<ExecutionPlan>);
    static_assert(std::is_nothrow_move_assignable_v<ExecutionPlan>);
    static_assert(!std::is_copy_constructible_v<DAG> && !std::is_copy_assignable_v<DAG>);
    static_assert(!std::is_move_constructible_v<DAG> && !std::is_move_assignable_v<DAG>);

    /// @brief Counts construction and destruction, including destruction inside audio processing.
    struct LifetimeTrace {
        /// @brief Per-processor destruction counts retained outside the owned Nodes.
        unsigned destructions = 0;
        /// @brief Number of processing calls observed by the fixture.
        unsigned processCalls = 0;
        /// @brief Whether every recorded destructor used the owner thread.
        bool destroyedOnOwner = true;
        /// @brief Thread identity expected to destroy the fixture processors.
        std::thread::id owner = std::this_thread::get_id();
    };

    /// @brief Lifetime observer used while constructing and retiring fixture processors.
    thread_local LifetimeTrace* constructionTrace = nullptr;

    /// @brief Temporarily selects the lifetime trace used by processor instances.
    class TraceScope {
    public:
        /// @brief Install the selected lifetime trace and remember the prior trace.
        explicit TraceScope(LifetimeTrace& trace) noexcept
            : previous_(std::exchange(constructionTrace, &trace)) {}
        /// @brief Restore the previously selected synchronous observation scope.
        ~TraceScope() { constructionTrace = previous_; }

    private:
        /// @brief Previously installed lifetime observer, restored when the scope ends.
        LifetimeTrace* previous_;
    };

    /// @brief Records construction, processing and destruction through a borrowed trace.
    class LifetimeNode final : public Node {
    public:
        /// @brief Borrow the active construction trace without retaining any backend pointers.
        explicit LifetimeNode(const NodeID id)
            : Node(id, Config{}), trace_(constructionTrace) {}

        /// @brief Record fixture destruction and release owned state on the calling thread.
        ~LifetimeNode() override {
            ++trace_->destructions;
            trace_->destroyedOnOwner = trace_->destroyedOnOwner &&
                std::this_thread::get_id() == trace_->owner;
        }

        /// @brief Record the callback while leaving borrowed sample storage unchanged.
        void process(const ProcessContext&) noexcept override {
            // Test consumer deliberately performs no allocation, locks, waits,
            // I/O or pointer retention. All worker access is joined before read.
            ++trace_->processCalls;
        }

    private:
        /// @brief Borrowed lifetime or binding trace; it must outlive the fixture processor.
        LifetimeTrace* trace_;
    };

    /// @brief Require the expected graph error category without changing ownership.
    void expectGraphError(const Result& result, const ErrorType error) {
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code, error.code);
    }

    /// @brief Compile a structural snapshot or report the fixture failure and return no plan.
    std::optional<ExecutionPlan> compiledPlan(DAG& graph) {
        auto compiled = graph.compile();
        if (!compiled) {
            ADD_FAILURE() << "Expected the graph to compile";
            return std::nullopt;
        }
        return std::move(*compiled);
    }

    /// @brief Test case: removed node remains alive for an old plan and disappears from the desired graph.
    TEST(NodePlanLifetime, RemovedNodeRemainsAliveForAnOldPlanAndDisappearsFromTheDesiredGraph) {
        LifetimeTrace trace;
        const TraceScope tracking{trace};
        {
            DAG graph;
            const auto addedA = graph.add<LifetimeNode>();
            ASSERT_TRUE(addedA);
            const auto& a = *static_cast<LifetimeNode*>(graph.node(*addedA));
            const auto addedB = graph.add<LifetimeNode>();
            ASSERT_TRUE(addedB);
            const auto& b = *static_cast<LifetimeNode*>(graph.node(*addedB));
            const auto from = a.channel(PortType::Out, 0);
            const auto to = b.channel(PortType::In, 0);
            ASSERT_TRUE(graph.connect(to, from));
            auto plan = compiledPlan(graph);
            ASSERT_TRUE(plan.has_value());

            ASSERT_TRUE(graph.remove(a.id()));
            EXPECT_EQ(graph.node(a.id()), nullptr);
            EXPECT_EQ(graph.node(b.id()), &b);
            expectGraphError(graph.connect(to, from), ErrorType::NodeNotFound);
            const auto disconnected = graph.disconnect(to, from);
            ASSERT_FALSE(disconnected.has_value());
            EXPECT_EQ(disconnected.error(), ErrorType::NodeNotFound);
            auto replacement = graph.compile();
            ASSERT_TRUE(replacement);
            ASSERT_EQ(replacement->tasks().size(), 1U);
            EXPECT_EQ(replacement->tasks().front(), &b);
            ASSERT_FALSE(plan->tasks().empty());
            plan->tasks().front()->process(ProcessContext{});
            EXPECT_EQ(trace.processCalls, 1U);
            EXPECT_EQ(trace.destructions, 0U);
            EXPECT_EQ(graph.collectRetiredNodes(), 0U);
        } // Plan retires before its owning DAG is destroyed.
        EXPECT_EQ(trace.destructions, 2U);
        EXPECT_TRUE(trace.destroyedOnOwner);
    }

    /// @brief Test case: releasing plan allows exactly one polymorphic destruction.
    TEST(NodePlanLifetime, ReleasingPlanAllowsExactlyOnePolymorphicDestruction) {
        LifetimeTrace trace;
        const TraceScope tracking{trace};
        DAG graph;
        const auto addedId = graph.add<LifetimeNode>();
        ASSERT_TRUE(addedId);
        const auto id = *addedId;
        auto plan = compiledPlan(graph);
        ASSERT_TRUE(plan.has_value());
        ASSERT_TRUE(graph.remove(id));
        EXPECT_EQ(trace.destructions, 0U);
        EXPECT_EQ(graph.collectRetiredNodes(), 0U);

        plan.reset();
        // The borrow token never owns or destroys the Node itself.
        EXPECT_EQ(trace.destructions, 0U);
        EXPECT_EQ(graph.collectRetiredNodes(), 1U);
        EXPECT_EQ(graph.collectRetiredNodes(), 0U);
        EXPECT_EQ(trace.destructions, 1U);
        EXPECT_EQ(graph.node(id), nullptr);
        expectGraphError(graph.remove(id), ErrorType::NodeNotFound);
        EXPECT_EQ(trace.destructions, 1U);
        EXPECT_TRUE(trace.destroyedOnOwner);
    }

    /// @brief Test case: independent compiled plans each keep their borrow until retirement.
    TEST(NodePlanLifetime, IndependentCompiledPlansEachKeepTheirBorrowUntilRetirement) {
        LifetimeTrace trace;
        const TraceScope tracking{trace};
        DAG graph;
        const auto addedId = graph.add<LifetimeNode>();
        ASSERT_TRUE(addedId);
        const auto id = *addedId;
        auto first = compiledPlan(graph);
        auto second = compiledPlan(graph);
        ASSERT_TRUE(first.has_value());
        ASSERT_TRUE(second.has_value());
        ASSERT_TRUE(graph.remove(id));
        first.reset();
        EXPECT_EQ(graph.collectRetiredNodes(), 0U);
        EXPECT_EQ(trace.destructions, 0U);
        second.reset();
        EXPECT_EQ(graph.collectRetiredNodes(), 1U);
        EXPECT_EQ(trace.destructions, 1U);
    }

    /// @brief Test case: copy and move construction preserve borrows and empty moved from tasks.
    TEST(NodePlanLifetime, CopyAndMoveConstructionPreserveBorrowsAndEmptyMovedFromTasks) {
        LifetimeTrace trace;
        const TraceScope tracking{trace};
        DAG graph;
        const auto addedId = graph.add<LifetimeNode>();
        ASSERT_TRUE(addedId);
        const auto id = *addedId;
        auto original = compiledPlan(graph);
        ASSERT_TRUE(original.has_value());
        std::optional<ExecutionPlan> copy{*original};
        std::optional<ExecutionPlan> moved{std::move(*copy)};
        EXPECT_TRUE(copy->tasks().empty());
        ASSERT_EQ(moved->tasks().size(), 1U);
        EXPECT_EQ(moved->tasks().front()->id(), id);
        copy.reset();
        original.reset();
        ASSERT_TRUE(graph.remove(id));
        EXPECT_EQ(graph.collectRetiredNodes(), 0U);
        EXPECT_EQ(trace.destructions, 0U);
        moved.reset();
        EXPECT_EQ(graph.collectRetiredNodes(), 1U);
        EXPECT_EQ(trace.destructions, 1U);
    }

    /// @brief Test case: copy assignment releases old borrow and retains the new one including self copy.
    TEST(NodePlanLifetime, CopyAssignmentReleasesOldBorrowAndRetainsTheNewOneIncludingSelfCopy) {
        LifetimeTrace trace;
        const TraceScope tracking{trace};
        DAG graph;
        const auto addedA = graph.add<LifetimeNode>();
        ASSERT_TRUE(addedA);
        auto& a = *static_cast<LifetimeNode*>(graph.node(*addedA));
        const auto addedB = graph.add<LifetimeNode>();
        ASSERT_TRUE(addedB);
        auto& b = *static_cast<LifetimeNode*>(graph.node(*addedB));
        const auto aID = a.id();
        const auto bID = b.id();
        std::optional<ExecutionPlan> left{std::in_place, std::vector<Node*>{&a},
                                          std::vector<std::size_t>{0, 1}};
        std::optional<ExecutionPlan> right{std::in_place, std::vector<Node*>{&b},
                                           std::vector<std::size_t>{0, 1}};

        *left = *right;
        auto* samePlan = &*left;
        *left = *samePlan;
        ASSERT_TRUE(graph.remove(aID));
        EXPECT_EQ(trace.destructions, 1U);
        ASSERT_EQ(left->tasks().size(), 1U);
        EXPECT_EQ(left->tasks().front()->id(), bID);
        right.reset();
        ASSERT_TRUE(graph.remove(bID));
        EXPECT_EQ(graph.collectRetiredNodes(), 0U);
        left.reset();
        EXPECT_EQ(graph.collectRetiredNodes(), 1U);
        EXPECT_EQ(trace.destructions, 2U);
    }

    /// @brief Test case: move assignment releases old borrow and survives self move.
    TEST(NodePlanLifetime, MoveAssignmentReleasesOldBorrowAndSurvivesSelfMove) {
        LifetimeTrace trace;
        const TraceScope tracking{trace};
        DAG graph;
        const auto addedA = graph.add<LifetimeNode>();
        ASSERT_TRUE(addedA);
        auto& a = *static_cast<LifetimeNode*>(graph.node(*addedA));
        const auto addedB = graph.add<LifetimeNode>();
        ASSERT_TRUE(addedB);
        auto& b = *static_cast<LifetimeNode*>(graph.node(*addedB));
        const auto aID = a.id();
        const auto bID = b.id();
        std::optional<ExecutionPlan> left{std::in_place, std::vector<Node*>{&a},
                                          std::vector<std::size_t>{0, 1}};
        std::optional<ExecutionPlan> right{std::in_place, std::vector<Node*>{&b},
                                           std::vector<std::size_t>{0, 1}};

        *left = std::move(*right);
        EXPECT_TRUE(right->tasks().empty());
        auto* samePlan = &*left;
        *left = std::move(*samePlan);
        ASSERT_TRUE(graph.remove(aID));
        ASSERT_EQ(left->tasks().size(), 1U);
        EXPECT_EQ(left->tasks().front()->id(), bID);
        right.reset();
        ASSERT_TRUE(graph.remove(bID));
        EXPECT_EQ(graph.collectRetiredNodes(), 0U);
        left.reset();
        EXPECT_EQ(graph.collectRetiredNodes(), 1U);
        EXPECT_EQ(trace.destructions, 2U);
    }

    /// @brief Test case: worker process borrows without owning or destroying the node.
    TEST(NodePlanLifetime, WorkerProcessBorrowsWithoutOwningOrDestroyingTheNode) {
        LifetimeTrace trace;
        const TraceScope tracking{trace};
        DAG graph;
        const auto addedId = graph.add<LifetimeNode>();
        ASSERT_TRUE(addedId);
        const auto id = *addedId;
        auto plan = compiledPlan(graph);
        ASSERT_TRUE(plan.has_value());
        ASSERT_EQ(plan->tasks().size(), 1U);
        auto* borrowed = plan->tasks().front();
        std::thread worker{[borrowed] {
            borrowed->process(ProcessContext{});
        }};
        worker.join();
        EXPECT_EQ(trace.processCalls, 1U);
        EXPECT_EQ(trace.destructions, 0U);
        ASSERT_TRUE(graph.remove(id));
        EXPECT_EQ(graph.collectRetiredNodes(), 0U);
        plan.reset();
        EXPECT_EQ(graph.collectRetiredNodes(), 1U);
        EXPECT_EQ(trace.destructions, 1U);
        EXPECT_TRUE(trace.destroyedOnOwner);
    }
}
