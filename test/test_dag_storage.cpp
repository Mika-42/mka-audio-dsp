#include <concepts>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

import mka.audio.dsp.dag;

namespace {
    using namespace mka::audio::dsp;

    class EmptyNode : public Node {
    public:
        explicit EmptyNode(const NodeID id) : Node(id, Config{}) {}
        void process(const ProcessContext&) noexcept override { ++calls; }
        unsigned calls = 0;
    };

    class PayloadNode final : public Node {
    public:
        PayloadNode(const NodeID id, std::unique_ptr<int> payload, int& setting)
            : Node(id, Config{}), payload(std::move(payload)), setting(&setting) {}
        void process(const ProcessContext&) noexcept override {}
        std::unique_ptr<int> payload;
        int* setting; // Test control data, owned by the caller outside the DAG.
    };

    class CountedNode final : public EmptyNode {
    public:
        CountedNode(const NodeID id, unsigned& constructions) : EmptyNode(id) { ++constructions; }
    };

    struct ConstructionFailure {};
    class ThrowingNode final : public EmptyNode {
    public:
        explicit ThrowingNode(const NodeID id) : EmptyNode(id) { throw ConstructionFailure{}; }
    };
    class BadAllocNode final : public EmptyNode {
    public:
        explicit BadAllocNode(const NodeID id) : EmptyNode(id) { throw std::bad_alloc{}; }
    };
    class WrongIdentityNode final : public EmptyNode {
    public:
        explicit WrongIdentityNode(NodeID) : EmptyNode(invalidNodeID) {}
    };
    class HiddenIdentityNode final : public EmptyNode {
    public:
        using EmptyNode::EmptyNode;
        NodeID id() const noexcept { return 99; }
    };
    class AbstractNode : public Node { public: using Node::Node; };
    class PrivateNode : private EmptyNode { public: using EmptyNode::EmptyNode; };
    class NoIdentityConstructor final : public EmptyNode {
    public:
        NoIdentityConstructor() : EmptyNode(1) {}
    };

    template<class T, class... Args>
    concept Addable = requires(DAG& graph, Args&&... args) {
        graph.template add<T>(std::forward<Args>(args)...);
    };

    static_assert(std::is_final_v<DAG> && !std::is_polymorphic_v<DAG>);
    static_assert(!std::is_copy_constructible_v<DAG> && !std::is_move_constructible_v<DAG>);
    static_assert(Addable<EmptyNode> && Addable<PayloadNode, std::unique_ptr<int>, int&>);
    static_assert(!Addable<int> && !Addable<Node> && !Addable<AbstractNode>);
    static_assert(!Addable<PrivateNode> && !Addable<NoIdentityConstructor>);
    static_assert(!Addable<PayloadNode> && !Addable<EmptyNode, int>);
    static_assert(std::same_as<decltype(std::declval<DAG&>().add<EmptyNode>()),
                               std::expected<NodeID, GraphError>>);
    static_assert(noexcept(std::declval<DAG&>().add<EmptyNode>()));
    static_assert(std::same_as<decltype(std::declval<DAG&>().node(NodeID{})), Node*>);
    static_assert(std::same_as<decltype(std::declval<const DAG&>().node(NodeID{})), const Node*>);
    static_assert(noexcept(std::declval<DAG&>().node(NodeID{})));
    static_assert(noexcept(std::declval<const DAG&>().node(NodeID{})));
    static_assert(noexcept(std::declval<DAG&>().collectRetiredNodes()));

    void expectAddError(const std::expected<NodeID, GraphError>& result, const GraphError error) {
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error(), error);
    }

    TEST(DAGStorage, MixedTypesForwardArgumentsAndExposeBorrowedMutableAndConstLookups) {
        int setting = 42;
        auto payload = std::make_unique<int>(7);
        const auto* payloadAddress = payload.get();
        DAG graph;
        const auto first = graph.add<EmptyNode>();
        const auto second = graph.add<PayloadNode>(std::move(payload), setting);
        ASSERT_TRUE(first);
        ASSERT_TRUE(second);
        EXPECT_EQ(*first, 1U);
        EXPECT_EQ(*second, 2U);
        EXPECT_EQ(payload, nullptr);
        auto* empty = dynamic_cast<EmptyNode*>(graph.node(*first));
        auto* effect = dynamic_cast<PayloadNode*>(graph.node(*second));
        ASSERT_NE(empty, nullptr);
        ASSERT_NE(effect, nullptr);
        EXPECT_EQ(effect->payload.get(), payloadAddress);
        EXPECT_EQ(*effect->payload, 7);
        EXPECT_EQ(effect->setting, &setting);
        empty->calls = 3; // Mutable control lookup is usable on a quiescent Node.
        const DAG& readOnly = graph;
        EXPECT_EQ(readOnly.node(*first), empty);
        EXPECT_EQ(readOnly.node(*second), effect);
        EXPECT_EQ(empty->calls, 3U);
    }

    TEST(DAGStorage, AbsentAndInvalidIdentitiesReturnNullFromBothOverloads) {
        DAG graph;
        const DAG& readOnly = graph;
        for (const NodeID id : {invalidNodeID, NodeID{1}, maxNodeID}) {
            EXPECT_EQ(graph.node(id), nullptr);
            EXPECT_EQ(readOnly.node(id), nullptr);
        }
        const auto id = graph.add<EmptyNode>();
        ASSERT_TRUE(id);
        ASSERT_TRUE(graph.remove(*id));
        EXPECT_EQ(graph.node(*id), nullptr);
        EXPECT_EQ(readOnly.node(*id), nullptr);
    }

    TEST(DAGStorage, GrowingAndReorderingOwnerStoragePreservesEveryLiveAddressAndOldTasks) {
        DAG graph;
        const auto root = graph.add<EmptyNode>();
        ASSERT_TRUE(root);
        Node* const rootAddress = graph.node(*root);
        auto oldPlan = graph.compile();
        ASSERT_TRUE(oldPlan);
        std::vector<std::pair<NodeID, Node*>> addresses;
        for (unsigned i = 0; i < 512; ++i) {
            const auto added = graph.add<EmptyNode>();
            ASSERT_TRUE(added);
            addresses.emplace_back(*added, graph.node(*added));
            EXPECT_EQ(graph.node(*root), rootAddress);
            EXPECT_EQ(oldPlan->tasks().front(), rootAddress);
        }
        const auto removed = addresses[100].first;
        ASSERT_TRUE(graph.remove(removed)); // Shifts unique_ptr handles, not Nodes.
        for (const auto& [id, address] : addresses)
            EXPECT_EQ(graph.node(id), id == removed ? nullptr : address);
        ASSERT_EQ(oldPlan->tasks().size(), 1U);
        EXPECT_EQ(oldPlan->tasks().front(), rootAddress);
        oldPlan->tasks().front()->process(ProcessContext{});
        EXPECT_EQ(static_cast<EmptyNode*>(rootAddress)->calls, 1U);
    }

    TEST(DAGStorage, RemovedIdentitiesAreNeverReusedEvenAfterCollection) {
        DAG graph;
        const auto first = graph.add<EmptyNode>();
        ASSERT_TRUE(first);
        auto oldPlan = graph.compile();
        ASSERT_TRUE(oldPlan);
        const auto second = graph.add<EmptyNode>();
        ASSERT_TRUE(second);
        ASSERT_TRUE(graph.remove(*second)); // Not borrowed by oldPlan.
        ASSERT_TRUE(graph.remove(*first));  // Retained for oldPlan.
        EXPECT_EQ(graph.collectRetiredNodes(), 0U);
        const auto third = graph.add<EmptyNode>();
        ASSERT_TRUE(third);
        EXPECT_EQ(*third, 3U);
        oldPlan = std::unexpected(GraphError::NodeNotFound);
        EXPECT_EQ(graph.collectRetiredNodes(), 1U);
        const auto fourth = graph.add<EmptyNode>();
        ASSERT_TRUE(fourth);
        EXPECT_EQ(*fourth, 4U);
        EXPECT_EQ(graph.node(*first), nullptr);
        EXPECT_EQ(graph.node(*second), nullptr);
    }

    TEST(DAGStorage, QuotaRejectsBeforeConstructionAndDoesNotConsumeAnIdentity) {
        unsigned constructions = 0;
        DAG disabled{0};
        expectAddError(disabled.add<CountedNode>(constructions), GraphError::NodeCapacityExceeded);
        EXPECT_EQ(constructions, 0U);
        DAG graph{1};
        const auto first = graph.add<CountedNode>(constructions);
        ASSERT_TRUE(first);
        expectAddError(graph.add<CountedNode>(constructions), GraphError::NodeCapacityExceeded);
        EXPECT_EQ(constructions, 1U);
        ASSERT_TRUE(graph.remove(*first));
        const auto next = graph.add<CountedNode>(constructions);
        ASSERT_TRUE(next);
        EXPECT_EQ(*next, 2U);
        EXPECT_EQ(constructions, 2U);
    }

    TEST(DAGStorage, RetiredNodesCountAgainstQuotaUntilEveryPlanReleasesItsBorrowAndControlCollects) {
        DAG graph{1};
        const auto first = graph.add<EmptyNode>();
        ASSERT_TRUE(first);
        auto original = graph.compile();
        ASSERT_TRUE(original);
        std::optional<ExecutionPlan> copy{*original};
        ASSERT_TRUE(graph.remove(*first));
        EXPECT_EQ(graph.node(*first), nullptr);
        expectAddError(graph.add<EmptyNode>(), GraphError::NodeCapacityExceeded);
        original = std::unexpected(GraphError::NodeNotFound);
        EXPECT_EQ(graph.collectRetiredNodes(), 0U);
        copy->tasks().front()->process(ProcessContext{});
        copy.reset();
        expectAddError(graph.add<EmptyNode>(), GraphError::NodeCapacityExceeded);
        EXPECT_EQ(graph.collectRetiredNodes(), 1U);
        const auto next = graph.add<EmptyNode>();
        ASSERT_TRUE(next);
        EXPECT_EQ(*next, 2U);
    }

    TEST(DAGStorage, ConstructorExceptionPreservesTheOldGraphPlanAndNextIdentity) {
        DAG graph;
        const auto first = graph.add<EmptyNode>();
        ASSERT_TRUE(first);
        auto* const address = graph.node(*first);
        auto active = graph.compile(BackendConfig{.maxFrames = 1});
        ASSERT_TRUE(active);
        expectAddError(graph.add<ThrowingNode>(), GraphError::NodeConstructionFailed);
        EXPECT_EQ(graph.node(*first), address);
        EXPECT_EQ(graph.node(2), nullptr);
        ASSERT_EQ(active->tasks().size(), 1U);
        EXPECT_EQ(active->tasks().front(), address);
        ASSERT_TRUE(active->exec(BackendBlock{.frames = 1}));
        EXPECT_EQ(static_cast<EmptyNode*>(address)->calls, 1U);
        const auto next = graph.add<EmptyNode>();
        ASSERT_TRUE(next);
        EXPECT_EQ(*next, 2U);
    }

    TEST(DAGStorage, ConstructorBadAllocReturnsAllocationFailureAndPreservesTheCandidate) {
        DAG graph;
        expectAddError(graph.add<BadAllocNode>(), GraphError::AllocationFailed);
        EXPECT_EQ(graph.node(1), nullptr);
        const auto next = graph.add<EmptyNode>();
        ASSERT_TRUE(next);
        EXPECT_EQ(*next, 1U);
    }

    TEST(DAGStorage, RejectsConstructorsThatIgnoreTheAssignedBaseIdentity) {
        DAG graph;
        expectAddError(graph.add<WrongIdentityNode>(), GraphError::InvalidNodeIdentity);
        EXPECT_EQ(graph.node(invalidNodeID), nullptr);
        EXPECT_EQ(graph.node(1), nullptr);
        const auto next = graph.add<HiddenIdentityNode>();
        ASSERT_TRUE(next);
        EXPECT_EQ(*next, 1U);
        // A derived name hiding Node::id cannot spoof the base identity check.
        EXPECT_EQ(graph.node(*next)->id(), 1U);
        EXPECT_EQ(static_cast<HiddenIdentityNode*>(graph.node(*next))->id(), 99U);
    }

    TEST(DAGIdentitySequence, ChecksTheActualUint32BoundaryBeforeNarrowingAndNeverWraps) {
        detail::NodeIDSequence ids{std::uint64_t{maxNodeID} - 1};
        ASSERT_TRUE(ids.candidate());
        EXPECT_EQ(*ids.candidate(), maxNodeID - 1);
        EXPECT_EQ(*ids.candidate(), maxNodeID - 1); // Failed work would not commit.
        ids.commit();
        ASSERT_TRUE(ids.candidate());
        EXPECT_EQ(*ids.candidate(), maxNodeID);
        ids.commit();
        expectAddError(ids.candidate(), GraphError::NodeIDExhausted);
        ids.commit();
        expectAddError(ids.candidate(), GraphError::NodeIDExhausted);
    }

    TEST(DAGIdentitySequence, InvalidOrAlreadyExhaustedCursorsCannotYieldTruncatedIdentities) {
        for (const auto initial : {std::uint64_t{0}, std::uint64_t{maxNodeID} + 1,
                                   std::numeric_limits<std::uint64_t>::max()}) {
            detail::NodeIDSequence ids{initial};
            expectAddError(ids.candidate(), GraphError::NodeIDExhausted);
            ids.commit();
            expectAddError(ids.candidate(), GraphError::NodeIDExhausted);
        }
    }
}
