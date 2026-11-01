#include <array>
#include <concepts>
#include <cstddef>
#include <expected>
#include <limits>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

import mka.audio.dsp.dag;
import mka.audio.dsp.structural_compile;

namespace {
    using namespace mka::audio::dsp;

    class StructuralNode final : public Node {
    public:
        explicit StructuralNode(NodeID id, const Config& config = Config{}) : Node(id, config) {}
        void process(const ProcessContext& context) noexcept override {
            ++calls;
            for (const auto& output : context.backendOutputs)
                for (FrameCount i = 0; i < context.frames; ++i) output.buffer[i] = 17.F;
        }
        unsigned calls = 0;
    };

    static_assert(std::same_as<decltype(std::declval<const DAG&>().compile(
        std::declval<ExecutionPlan&>(), std::declval<const CompileConfig&>())), Result>);
    static_assert(std::same_as<decltype(std::declval<const ExecutionPlan&>().executionTasks()),
        std::span<const ExecutionTask>>);
    static_assert(std::same_as<decltype(std::declval<const ExecutionPlan&>().sourceTasks()),
        std::span<const std::size_t>>);
    static_assert(std::same_as<decltype(std::declval<const ExecutionPlan&>().outputTasks()),
        std::span<const std::size_t>>);
    static_assert(std::same_as<decltype(std::declval<const ExecutionPlan&>().taskIndexOf(NodeID{})),
        std::optional<std::size_t>>);
    static_assert(std::is_nothrow_default_constructible_v<ExecutionPlan>);
    static_assert(std::is_nothrow_move_constructible_v<ExecutionPlan>);
    static_assert(std::is_nothrow_move_assignable_v<ExecutionPlan>);

    std::vector<NodeID> taskIDs(const ExecutionPlan& plan) {
        std::vector<NodeID> ids;
        for (const auto& task : plan.executionTasks()) ids.push_back(task.id);
        return ids;
    }

    /** Retain values and storage addresses to detect any failed partial commit. */
    struct PlanImage {
        std::vector<ExecutionTask> tasks;
        std::vector<std::size_t> sources, outputs;
        Node* const* pointerStorage;
        const ExecutionTask* taskStorage;
        const std::size_t* sourceStorage;
        const std::size_t* outputStorage;
        bool prepared;
        std::size_t layers;

        explicit PlanImage(const ExecutionPlan& plan)
            : tasks(plan.executionTasks().begin(), plan.executionTasks().end()),
              sources(plan.sourceTasks().begin(), plan.sourceTasks().end()),
              outputs(plan.outputTasks().begin(), plan.outputTasks().end()),
              pointerStorage(plan.tasks().data()), taskStorage(plan.executionTasks().data()),
              sourceStorage(plan.sourceTasks().data()), outputStorage(plan.outputTasks().data()),
              prepared(plan.isBackendPrepared()), layers(plan.layerCount()) {}

        void expectUnchanged(const ExecutionPlan& plan) const {
            EXPECT_EQ(plan.tasks().data(), pointerStorage);
            EXPECT_EQ(plan.executionTasks().data(), taskStorage);
            EXPECT_EQ(plan.sourceTasks().data(), sourceStorage);
            EXPECT_EQ(plan.outputTasks().data(), outputStorage);
            EXPECT_EQ(plan.isBackendPrepared(), prepared);
            EXPECT_EQ(plan.layerCount(), layers);
            ASSERT_EQ(plan.tasks().size(), tasks.size());
            ASSERT_EQ(plan.executionTasks().size(), tasks.size());
            EXPECT_EQ((std::vector<std::size_t>{plan.sourceTasks().begin(), plan.sourceTasks().end()}), sources);
            EXPECT_EQ((std::vector<std::size_t>{plan.outputTasks().begin(), plan.outputTasks().end()}), outputs);
            for (std::size_t i = 0; i < tasks.size(); ++i) {
                const auto& actual = plan.executionTasks()[i];
                EXPECT_EQ(actual.node, tasks[i].node);
                EXPECT_EQ(actual.id, tasks[i].id);
                EXPECT_EQ(actual.index, tasks[i].index);
                EXPECT_EQ(actual.isSource, tasks[i].isSource);
                EXPECT_EQ(actual.isOutput, tasks[i].isOutput);
                EXPECT_EQ(plan.tasks()[i], tasks[i].node);
                EXPECT_EQ(plan.taskIndexOf(tasks[i].id), i);
            }
        }
    };

    void expectIndexed(const ExecutionPlan& plan) {
        ASSERT_EQ(plan.executionTasks().size(), plan.tasks().size());
        std::vector<std::size_t> sources, outputs;
        for (std::size_t i = 0; i < plan.executionTasks().size(); ++i) {
            const auto& task = plan.executionTasks()[i];
            EXPECT_EQ(task.node, plan.tasks()[i]);
            EXPECT_EQ(task.id, task.node->id());
            EXPECT_EQ(task.index, i);
            EXPECT_EQ(plan.taskIndexOf(task.id), i);
            ASSERT_EQ(plan.layer(i).size(), 1U);
            EXPECT_EQ(plan.layer(i)[0], task.node);
            if (task.isSource) sources.push_back(i);
            if (task.isOutput) outputs.push_back(i);
        }
        EXPECT_EQ(plan.layerCount(), plan.tasks().size());
        EXPECT_EQ((std::vector<std::size_t>{plan.sourceTasks().begin(), plan.sourceTasks().end()}), sources);
        EXPECT_EQ((std::vector<std::size_t>{plan.outputTasks().begin(), plan.outputTasks().end()}), outputs);
        EXPECT_FALSE(plan.taskIndexOf(invalidNodeID));
        EXPECT_FALSE(plan.taskIndexOf(maxNodeID));
    }

    TEST(StructuralCompilation, DefaultAndEmptyTargetsHaveNoAudioOrTasks) {
        DAG graph;
        ExecutionPlan target;
        EXPECT_TRUE(target.tasks().empty());
        EXPECT_TRUE(target.executionTasks().empty());
        EXPECT_TRUE(target.sourceTasks().empty());
        EXPECT_TRUE(target.outputTasks().empty());
        EXPECT_FALSE(target.isBackendPrepared());
        EXPECT_FALSE(target.taskIndexOf(1));
        ASSERT_TRUE(graph.compile(target, CompileConfig{}));
        EXPECT_FALSE(target.isBackendPrepared());
        ASSERT_NO_FATAL_FAILURE(expectIndexed(target));
        auto legacy = graph.compile();
        ASSERT_TRUE(legacy);
        EXPECT_TRUE(legacy->executionTasks().empty());
        EXPECT_FALSE(legacy->isBackendPrepared());
    }

    TEST(StructuralCompilation, IsolatesIncludeIdentityGapsAndIndependentOutputMembership) {
        DAG graph;
        const auto a = graph.add<StructuralNode>(), removed = graph.add<StructuralNode>();
        const auto c = graph.add<StructuralNode>();
        ASSERT_TRUE(a); ASSERT_TRUE(removed); ASSERT_TRUE(c);
        ASSERT_TRUE(graph.remove(*removed));
        ASSERT_TRUE(graph.markAsOutput(*c));
        ExecutionPlan target;
        ASSERT_TRUE(graph.compile(target, CompileConfig{}));
        EXPECT_EQ(taskIDs(target), (std::vector<NodeID>{*a, *c}));
        EXPECT_FALSE(target.taskIndexOf(*removed));
        ASSERT_NO_FATAL_FAILURE(expectIndexed(target));
        EXPECT_EQ((std::vector<std::size_t>{target.sourceTasks().begin(), target.sourceTasks().end()}),
                  (std::vector<std::size_t>{0, 1}));
        EXPECT_EQ((std::vector<std::size_t>{target.outputTasks().begin(), target.outputTasks().end()}),
                  (std::vector<std::size_t>{1}));
        EXPECT_EQ(static_cast<StructuralNode*>(graph.node(*a))->calls, 0U);
        EXPECT_EQ(static_cast<StructuralNode*>(graph.node(*c))->calls, 0U);
    }

    TEST(StructuralCompilation, UndeclaredAuxiliaryDependenciesUseGlobalPriorityAndIndependentRoles) {
        DAG graph;
        const auto sink = graph.add<StructuralNode>(), source = graph.add<StructuralNode>();
        const auto isolated = graph.add<StructuralNode>(), tail = graph.add<StructuralNode>();
        ASSERT_TRUE(sink); ASSERT_TRUE(source); ASSERT_TRUE(isolated); ASSERT_TRUE(tail);
        ASSERT_TRUE(graph.connect(encode(*sink, PortType::Aux, 65535), encode(*source, PortType::Out, 42)));
        ASSERT_TRUE(graph.connect(encode(*tail, PortType::In, 65535), encode(*sink, PortType::Out, 0)));
        ASSERT_TRUE(graph.markAsOutput(*source)); // Source and output, with a successor.
        ASSERT_TRUE(graph.markAsOutput(*sink));   // Output with both predecessor and successor.
        ExecutionPlan target;
        ASSERT_TRUE(graph.compile(target, CompileConfig{}));
        EXPECT_EQ(taskIDs(target), (std::vector<NodeID>{*source, *sink, *isolated, *tail}));
        ASSERT_NO_FATAL_FAILURE(expectIndexed(target));
        EXPECT_EQ((std::vector<std::size_t>{target.sourceTasks().begin(), target.sourceTasks().end()}),
                  (std::vector<std::size_t>{0, 2}));
        EXPECT_EQ((std::vector<std::size_t>{target.outputTasks().begin(), target.outputTasks().end()}),
                  (std::vector<std::size_t>{0, 1}));
        for (const auto& task : target.executionTasks())
            EXPECT_EQ(static_cast<StructuralNode*>(task.node)->calls, 0U);
        EXPECT_FALSE(target.isBackendPrepared());
    }

    TEST(StructuralCompilation, DiamondAndParallelLogicalEdgesYieldOneTaskPerNode) {
        DAG graph;
        std::array<NodeID, 4> ids{};
        for (auto& id : ids) {
            const auto added = graph.add<StructuralNode>();
            ASSERT_TRUE(added);
            id = *added;
        }
        const std::array edges{
            Connection{encode(ids[0], PortType::Out, 1), encode(ids[2], PortType::Aux, 65535)},
            Connection{encode(ids[2], PortType::Out, 2), encode(ids[3], PortType::In, 0)},
            Connection{encode(ids[0], PortType::Out, 0), encode(ids[1], PortType::In, 0)},
            Connection{encode(ids[1], PortType::Out, 0), encode(ids[3], PortType::Aux, 0)},
            Connection{encode(ids[0], PortType::Out, 0), encode(ids[1], PortType::Aux, 0)}};
        for (const auto& edge : edges) ASSERT_TRUE(graph.connect(edge.to, edge.from));
        ASSERT_TRUE(graph.markAsOutput(ids[3]));
        ExecutionPlan target;
        ASSERT_TRUE(graph.compile(target, CompileConfig{}));
        EXPECT_EQ(taskIDs(target), (std::vector<NodeID>{ids.begin(), ids.end()}));
        ASSERT_NO_FATAL_FAILURE(expectIndexed(target));
        ASSERT_EQ(target.sourceTasks().size(), 1U);
        EXPECT_EQ(target.sourceTasks()[0], 0U);
        ASSERT_EQ(target.outputTasks().size(), 1U);
        EXPECT_EQ(target.outputTasks()[0], 3U);
        for (const auto& task : target.executionTasks())
            EXPECT_EQ(static_cast<StructuralNode*>(task.node)->calls, 0U);
    }

    TEST(StructuralCompilation, DetachedOwnerOrderDoesNotAffectRanksOrRoleIndexes) {
        StructuralNode three{3}, nine{9}, twenty{20}, fifty{50};
        const std::array<Node*, 4> owners{&fifty, &nine, &three, &twenty};
        const detail::GraphSnapshot graph{
            .nodes = {20, 50, 3, 9},
            .connections = {{encode(9, PortType::Out, 42), encode(3, PortType::Aux, 65535)},
                            {encode(20, PortType::Out, 0), encode(50, PortType::In, 7)}},
            .outputs = {50, 9}};
        {
            ExecutionPlan target;
            ASSERT_TRUE(detail::compileSnapshot(owners, graph, target, CompileConfig{}));
            EXPECT_EQ(taskIDs(target), (std::vector<NodeID>{9, 3, 20, 50}));
            ASSERT_NO_FATAL_FAILURE(expectIndexed(target));
            EXPECT_EQ((std::vector<std::size_t>{target.sourceTasks().begin(), target.sourceTasks().end()}),
                      (std::vector<std::size_t>{0, 2}));
            EXPECT_EQ((std::vector<std::size_t>{target.outputTasks().begin(), target.outputTasks().end()}),
                      (std::vector<std::size_t>{0, 3}));
            for (auto* node : owners) EXPECT_TRUE(detail::NodeBorrowAccess::inUse(*node));
        }
        for (auto* node : owners) {
            EXPECT_FALSE(detail::NodeBorrowAccess::inUse(*node));
            EXPECT_EQ(static_cast<StructuralNode*>(node)->calls, 0U);
        }
    }

    TEST(StructuralCompilation, InvalidDetachedSnapshotsPreservePreparedTargetAndAllExistingBorrows) {
        StructuralNode one{1}, two{2}, three{3};
        const std::array<Node*, 3> owners{&one, &two, &three};
        const detail::GraphSnapshot valid{
            .nodes = {1, 2, 3},
            .connections = {{encode(1, PortType::Out, 0), encode(2, PortType::In, 0)},
                            {encode(2, PortType::Out, 0), encode(3, PortType::Aux, 65535)}},
            .outputs = {1, 3}};
        struct Case { detail::GraphSnapshot graph; Error error; };
        std::vector<Case> cases;
        const auto add = [&](detail::GraphSnapshot graph, Error error) { cases.push_back({std::move(graph), error}); };
        auto fixture = valid;
        fixture.nodes = {1, 1, 3}; add(fixture, {ErrorCode::InvalidParameter, 1});
        fixture = valid; fixture.nodes = {0, 2, 3}; add(fixture, {ErrorCode::InvalidParameter});
        fixture = valid; fixture.nodes = {1, 2, 9}; add(fixture, {ErrorCode::NodeNotFound, 9});
        fixture = valid; fixture.nodes = {1, 2}; fixture.outputs = {1};
        add(fixture, {ErrorCode::NodeNotFound, 3, encode(3, PortType::Aux, 65535)});
        fixture = valid; fixture.outputs = {9}; add(fixture, {ErrorCode::NodeNotFound, 9});
        fixture = valid; fixture.outputs = {2, 2}; add(fixture, {ErrorCode::InvalidParameter, 2});
        fixture = valid; fixture.connections.push_back(fixture.connections.front());
        add(fixture, {ErrorCode::ConnectionExists, 2, encode(2, PortType::In, 0)});
        fixture = valid; fixture.connections.push_back({encode(3, PortType::Out, 0), encode(2, PortType::In, 0)});
        add(fixture, {ErrorCode::InputOccupied, 2, encode(2, PortType::In, 0)});
        const ChannelID malformed = (ChannelID{1} << 32U) | (ChannelID{0x8001} << 16U) | 65535U;
        fixture = valid; fixture.connections[0].from = malformed;
        add(fixture, {ErrorCode::InvalidPort, 1, malformed});
        fixture = valid; fixture.connections[0].to = malformed;
        add(fixture, {ErrorCode::InvalidPort, 1, malformed});
        fixture = valid; fixture.connections[0].from = encode(1, PortType::Aux, 0);
        add(fixture, {ErrorCode::InvalidDirection, 1, encode(1, PortType::Aux, 0)});
        fixture = valid; fixture.connections[0].to = encode(2, PortType::Out, 0);
        add(fixture, {ErrorCode::InvalidDirection, 2, encode(2, PortType::Out, 0)});
        fixture = valid; fixture.connections[0].from = encode(9, PortType::Out, 0);
        add(fixture, {ErrorCode::NodeNotFound, 9, encode(9, PortType::Out, 0)});
        fixture = valid; fixture.connections[0].to = encode(9, PortType::In, 0);
        add(fixture, {ErrorCode::NodeNotFound, 9, encode(9, PortType::In, 0)});
        fixture = valid; fixture.connections.push_back({encode(3, PortType::Out, 0), encode(1, PortType::Aux, 0)});
        add(fixture, {ErrorCode::CycleDetected});

        DAG oldGraph;
        const auto oldID = oldGraph.add<StructuralNode>();
        ASSERT_TRUE(oldID);
        ASSERT_TRUE(oldGraph.markAsOutput(*oldID));
        auto prepared = oldGraph.compile(BackendConfig{.maxFrames = 1, .outputChannels = 1});
        ASSERT_TRUE(prepared);
        ExecutionPlan target = std::move(*prepared);
        ExecutionPlan active = target;
        const PlanImage before{target}, activeBefore{active};
        auto* oldNode = static_cast<StructuralNode*>(oldGraph.node(*oldID));
        for (std::size_t i = 0; i < cases.size(); ++i) {
            SCOPED_TRACE(i);
            const auto callsBefore = oldNode->calls;
            const auto inputBefore = cases[i].graph;
            const auto result = detail::compileSnapshot(owners, cases[i].graph, target, CompileConfig{});
            ASSERT_FALSE(result);
            EXPECT_EQ(result.error(), cases[i].error);
            EXPECT_EQ(cases[i].graph.nodes, inputBefore.nodes);
            EXPECT_EQ(cases[i].graph.connections, inputBefore.connections);
            EXPECT_EQ(cases[i].graph.outputs, inputBefore.outputs);
            ASSERT_NO_FATAL_FAILURE(before.expectUnchanged(target));
            ASSERT_NO_FATAL_FAILURE(activeBefore.expectUnchanged(active));
            EXPECT_EQ(oldNode->calls, callsBefore);
            EXPECT_FALSE(detail::NodeBorrowAccess::inUse(one));
            EXPECT_FALSE(detail::NodeBorrowAccess::inUse(two));
            EXPECT_FALSE(detail::NodeBorrowAccess::inUse(three));
            EXPECT_EQ(one.calls + two.calls + three.calls, 0U);
            float sample = -1.F;
            const std::array outputs{&sample};
            ASSERT_TRUE(target.exec(BackendBlock{.outputs = outputs, .frames = 1}));
            EXPECT_EQ(sample, 17.F);
            EXPECT_EQ(oldNode->calls, callsBefore + 1);
        }
    }

    TEST(StructuralCompilation, InvalidOwnersAndIntrinsicConfigDoNotAcquirePartialBorrows) {
        StructuralNode one{1}, two{2}, duplicate{1}, zero{0};
        // A deliberately malformed detached fixture exercises validation only;
        // it is never installed into a DAG or executed.
        StructuralNode invalidConfig{3, Config{.layoutAuxiliary = {42, 42}}};
        const detail::GraphSnapshot graph{.nodes = {1, 2}};
        ExecutionPlan target;
        const PlanImage empty{target};
        const std::array<std::array<Node*, 2>, 3> invalidOwners{{
            {&one, nullptr}, {&one, &duplicate}, {&one, &zero}}};
        const std::array<NodeID, 3> expectedIDs{0, 1, 0};
        for (std::size_t i = 0; i < invalidOwners.size(); ++i) {
            const auto result = detail::compileSnapshot(invalidOwners[i], graph, target, CompileConfig{});
            ASSERT_FALSE(result);
            EXPECT_EQ(result.error(), (Error{ErrorCode::InvalidParameter, expectedIDs[i]}));
            ASSERT_NO_FATAL_FAILURE(empty.expectUnchanged(target));
            EXPECT_FALSE(detail::NodeBorrowAccess::inUse(one));
        }
        const std::array<Node*, 3> owners{&one, &two, &invalidConfig};
        const detail::GraphSnapshot malformedConfig{.nodes = {1, 2, 3}};
        const auto result = detail::compileSnapshot(owners, malformedConfig, target, CompileConfig{});
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error(), (Error{ErrorCode::InvalidParameter, 3}));
        ASSERT_NO_FATAL_FAILURE(empty.expectUnchanged(target));
        EXPECT_FALSE(detail::NodeBorrowAccess::inUse(one));
        EXPECT_FALSE(detail::NodeBorrowAccess::inUse(two));
        EXPECT_FALSE(detail::NodeBorrowAccess::inUse(invalidConfig));
    }

    TEST(StructuralCompilation, CapacityBudgetAndCheckedArithmeticRejectBeforeReplacingTheTarget) {
        DAG graph;
        std::array<NodeID, 3> ids{};
        for (auto& id : ids) {
            const auto added = graph.add<StructuralNode>();
            ASSERT_TRUE(added);
            id = *added;
        }
        ASSERT_TRUE(graph.connect(encode(ids[1], PortType::In, 0), encode(ids[0], PortType::Out, 0)));
        ASSERT_TRUE(graph.connect(encode(ids[2], PortType::Aux, 0), encode(ids[1], PortType::Out, 0)));
        auto targetResult = graph.compile(BackendConfig{.maxFrames = 1});
        ASSERT_TRUE(targetResult);
        ExecutionPlan target = std::move(*targetResult);
        const PlanImage before{target};
        const auto bytes = detail::structuralStorageBytes(ids.size());
        ASSERT_TRUE(bytes);
        ASSERT_GT(*bytes, 0U);
        const std::array configs{CompileConfig{.maxNodes = 2}, CompileConfig{.maxConnections = 1},
                                CompileConfig{.memoryBudgetBytes = *bytes - 1}};
        for (std::size_t i = 0; i < configs.size(); ++i) {
            const auto result = graph.compile(target, configs[i]);
            ASSERT_FALSE(result);
            EXPECT_EQ(result.error().code, i == 2 ? ErrorCode::MemoryBudgetExceeded : ErrorCode::CapacityExceeded);
            ASSERT_NO_FATAL_FAILURE(before.expectUnchanged(target));
        }
        const auto overflow = detail::structuralStorageBytes(std::numeric_limits<std::size_t>::max());
        ASSERT_FALSE(overflow);
        EXPECT_EQ(overflow.error(), (Error{ErrorCode::ArithmeticOverflow}));
        ASSERT_TRUE(graph.compile(target, CompileConfig{.maxNodes = 3, .maxConnections = 2,
                                                       .memoryBudgetBytes = *bytes}));
        EXPECT_FALSE(target.isBackendPrepared());
        ASSERT_NO_FATAL_FAILURE(expectIndexed(target));
        for (const auto& task : target.executionTasks())
            EXPECT_EQ(static_cast<StructuralNode*>(task.node)->calls, 0U);
    }

    TEST(StructuralCompilation, ReplacingTheCandidateKeepsTheSeparatePreparedGenerationIntact) {
        DAG graph;
        const auto first = graph.add<StructuralNode>();
        ASSERT_TRUE(first);
        ASSERT_TRUE(graph.markAsOutput(*first));
        auto prepared = graph.compile(BackendConfig{.maxFrames = 1, .outputChannels = 1});
        ASSERT_TRUE(prepared);
        ExecutionPlan active = std::move(*prepared);
        ExecutionPlan candidate = active;
        const PlanImage activeBefore{active};
        const auto second = graph.add<StructuralNode>();
        ASSERT_TRUE(second);
        ASSERT_TRUE(graph.connect(encode(*second, PortType::Aux, 65535), encode(*first, PortType::Out, 65535)));
        ASSERT_TRUE(graph.compile(candidate, CompileConfig{}));
        EXPECT_FALSE(candidate.isBackendPrepared());
        EXPECT_EQ(taskIDs(candidate), (std::vector<NodeID>{*first, *second}));
        ASSERT_NO_FATAL_FAILURE(expectIndexed(candidate));
        ASSERT_NO_FATAL_FAILURE(activeBefore.expectUnchanged(active));
        EXPECT_FALSE(active.taskIndexOf(*second));
        float sample = -1.F;
        const std::array outputs{&sample};
        ASSERT_TRUE(active.exec(BackendBlock{.outputs = outputs, .frames = 1}));
        EXPECT_EQ(sample, 17.F);
        EXPECT_EQ(static_cast<StructuralNode*>(graph.node(*first))->calls, 1U);
        EXPECT_EQ(static_cast<StructuralNode*>(graph.node(*second))->calls, 0U);
    }

    TEST(StructuralCompilation, CopyMoveAndLegacyPreparationKeepMetadataIndexesConsistent) {
        StructuralNode one{1}, two{2};
        const std::array edges{Connection{encode(1, PortType::Out, 0), encode(2, PortType::Aux, 65535)}};
        const std::array<NodeID, 1> outputs{1};
        const auto expectRoles = [](const ExecutionPlan& plan) {
            ASSERT_EQ(plan.executionTasks().size(), 2U);
            EXPECT_TRUE(plan.executionTasks()[0].isSource);
            EXPECT_TRUE(plan.executionTasks()[0].isOutput);
            EXPECT_FALSE(plan.executionTasks()[1].isSource);
            EXPECT_FALSE(plan.executionTasks()[1].isOutput);
        };
        ExecutionPlan legacy{{&one, &two}, {0, 1, 2}};
        EXPECT_EQ(legacy.sourceTasks().size(), 2U);
        EXPECT_TRUE(legacy.outputTasks().empty());
        ASSERT_TRUE(legacy.prepareBackend(BackendConfig{.maxFrames = 1}, edges, outputs));
        ASSERT_NO_FATAL_FAILURE(expectIndexed(legacy));
        ASSERT_NO_FATAL_FAILURE(expectRoles(legacy));
        ASSERT_EQ(legacy.sourceTasks().size(), 1U);
        EXPECT_EQ(legacy.sourceTasks()[0], 0U);
        ASSERT_EQ(legacy.outputTasks().size(), 1U);
        EXPECT_EQ(legacy.outputTasks()[0], 0U);
        ExecutionPlan copy = legacy;
        ASSERT_NO_FATAL_FAILURE(expectIndexed(copy));
        ASSERT_NO_FATAL_FAILURE(expectRoles(copy));
        EXPECT_EQ(taskIDs(copy), taskIDs(legacy));
        EXPECT_NE(copy.executionTasks().data(), legacy.executionTasks().data());
        EXPECT_TRUE(copy.isBackendPrepared());
        const PlanImage before{copy};
        const std::array<NodeID, 1> missingOutput{9};
        const auto rejected = copy.prepareBackend(BackendConfig{.maxFrames = 1}, edges, missingOutput);
        ASSERT_FALSE(rejected);
        EXPECT_EQ(rejected.error(), (Error{ErrorCode::NodeNotFound, 9}));
        ASSERT_NO_FATAL_FAILURE(before.expectUnchanged(copy));
        ExecutionPlan assigned;
        assigned = copy;
        ASSERT_NO_FATAL_FAILURE(expectIndexed(assigned));
        ASSERT_NO_FATAL_FAILURE(expectRoles(assigned));
        EXPECT_EQ(taskIDs(assigned), taskIDs(copy));
        ExecutionPlan moved = std::move(copy);
        ASSERT_NO_FATAL_FAILURE(expectIndexed(moved));
        ASSERT_NO_FATAL_FAILURE(expectRoles(moved));
        EXPECT_TRUE(copy.tasks().empty());
        EXPECT_TRUE(copy.executionTasks().empty());
        EXPECT_TRUE(copy.sourceTasks().empty());
        EXPECT_TRUE(copy.outputTasks().empty());
        EXPECT_FALSE(copy.taskIndexOf(1));
        assigned = std::move(moved);
        ASSERT_NO_FATAL_FAILURE(expectIndexed(assigned));
        ASSERT_NO_FATAL_FAILURE(expectRoles(assigned));
        EXPECT_EQ(taskIDs(assigned), (std::vector<NodeID>{1, 2}));
        EXPECT_TRUE(moved.executionTasks().empty());
        EXPECT_FALSE(moved.taskIndexOf(2));
        EXPECT_TRUE(assigned.isBackendPrepared());
        EXPECT_EQ(one.calls + two.calls, 0U);
    }
}
