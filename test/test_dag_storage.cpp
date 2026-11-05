/**
 * @file test_dag_storage.cpp
 * @brief Tests for dag storage.
 * @author Mika
 */
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

    /// @brief Minimal processor with no sample-processing side effects.
    class EmptyNode : public Node {
    public:
        /// @brief Construct a processor with the supplied identity and no declared logical ports.
        explicit EmptyNode(const NodeID id) : Node(id, Config{}) {}
        /// @brief Count processing calls without accessing samples or changing ownership.
        void process(const ProcessContext&) noexcept override { ++calls; }
        /// @brief Number of processor invocations observed by the fixture.
        unsigned calls = 0;
    };

    /// @brief Owns an additional payload so node-address stability can be exercised.
    class PayloadNode final : public Node {
    public:
        /// @brief Allocate an owned payload and retain its stable configuration address.
        PayloadNode(const NodeID id, std::unique_ptr<int> payload, int& setting)
            : Node(id, Config{}), payload(std::move(payload)), setting(&setting) {}
        /// @brief Leave sample buffers unchanged; this processor supplies structural metadata only.
        void process(const ProcessContext&) noexcept override {}
        /// @brief Extra owned allocation used to expose node lifetime and address stability.
        std::unique_ptr<int> payload;
        /// @brief Borrowed configuration payload whose address must survive graph insertions.
        int* setting; // Test control data, owned by the caller outside the DAG.
    };

    /// @brief Reports its destruction through an external counter.
    class CountedNode final : public EmptyNode {
    public:
        /// @brief Borrow the destruction counter used to observe ownership release.
        CountedNode(const NodeID id, unsigned& constructions) : EmptyNode(id) { ++constructions; }
    };

    /// @brief Exception tag used to exercise failed processor construction.
    struct ConstructionFailure {};
    /// @brief Throws during construction to exercise transactional DAG insertion.
    class ThrowingNode final : public EmptyNode {
    public:
        /// @brief Throw the fixture exception to exercise transactional insertion failure.
        explicit ThrowingNode(const NodeID id) : EmptyNode(id) { throw ConstructionFailure{}; }
    };
    /// @brief Throws an allocation error during construction to exercise error translation.
    class BadAllocNode final : public EmptyNode {
    public:
        /// @brief Throw bad_alloc to exercise insertion failure before the DAG takes ownership.
        explicit BadAllocNode(const NodeID id) : EmptyNode(id) { throw std::bad_alloc{}; }
    };
    /// @brief Intentionally exposes an identity inconsistent with the DAG assignment.
    class WrongIdentityNode final : public EmptyNode {
    public:
        /// @brief Construct a processor with a deliberately mismatched base identity.
        explicit WrongIdentityNode(NodeID) : EmptyNode(invalidNodeID) {}
    };
    /// @brief Hides the identity accessor to exercise insertion-time identity validation.
    class HiddenIdentityNode final : public EmptyNode {
    public:
        using EmptyNode::EmptyNode;
        /// @brief Return a deliberately hidden identity to exercise the base Node identity contract.
        NodeID id() const noexcept { return 99; }
    };
    /// @brief Abstract processor used to exercise the add<T> construction constraints.
    class AbstractNode : public Node { public: using Node::Node; };
    /// @brief Nonpublic processor constructor used to exercise add<T> constraints.
    class PrivateNode : private EmptyNode { public: using EmptyNode::EmptyNode; };
    /// @brief Processor without the required NodeID constructor argument.
    class NoIdentityConstructor final : public EmptyNode {
    public:
        /// @brief Construct the negative fixture without the NodeID argument required by DAG::add.
        NoIdentityConstructor() : EmptyNode(1) {}
    };

    /// @brief Whether DAG::add accepts the processor type and constructor arguments.
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

    /// @brief Require the expected insertion error without dereferencing a failed NodeID result.
    void expectAddError(const std::expected<NodeID, GraphError>& result, const GraphError error) {
        ASSERT_FALSE(result);
        EXPECT_EQ(result.error().code, error.code);
    }

    /// @brief Test case: mixed types forward arguments and expose borrowed mutable and const lookups.
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

    /// @brief Test case: absent and invalid identities return null from both overloads.
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

    /// @brief Test case: growing and reordering owner storage preserves every live address and old tasks.
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

    /// @brief Test case: removed identities are never reused even after collection.
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

    /// @brief Test case: quota rejects before construction and does not consume an identity.
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

    /// @brief Test case: retired nodes count against quota until every plan releases its borrow and control collects.
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

    /// @brief Test case: constructor exception preserves the old graph plan and next identity.
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

    /// @brief Test case: constructor bad alloc returns allocation failure and preserves the candidate.
    TEST(DAGStorage, ConstructorBadAllocReturnsAllocationFailureAndPreservesTheCandidate) {
        DAG graph;
        expectAddError(graph.add<BadAllocNode>(), GraphError::AllocationFailed);
        EXPECT_EQ(graph.node(1), nullptr);
        const auto next = graph.add<EmptyNode>();
        ASSERT_TRUE(next);
        EXPECT_EQ(*next, 1U);
    }

    /// @brief Test case: rejects constructors that ignore the assigned base identity.
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

    /// @brief Test case: checks the actual uint32 boundary before narrowing and never wraps.
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

    /// @brief Test case: invalid or already exhausted cursors cannot yield truncated identities.
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
