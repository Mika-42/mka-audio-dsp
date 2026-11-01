#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <limits>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "mutation_oracle.hpp"

import mka.audio.dsp.dag;

namespace {
    using namespace mka::audio::dsp;
    namespace reference = mutation_oracle;
    constexpr std::size_t operationCount = 10000;

    struct MutationTrace {
        std::array<unsigned, operationCount + 1> destructions{};
        unsigned total = 0;
        unsigned processCalls = 0;
        bool invalidIdentity = false;
        bool duplicateDestruction = false;
        bool destroyedOnControl = true;
        std::thread::id control = std::this_thread::get_id();
    };

    /** All observations outlive the DAG and its control-owned plan snapshots. */
    class MutationNode final : public Node {
    public:
        MutationNode(NodeID id, MutationTrace& trace)
            : Node(id, id % 2 == 0 ? Config{} :
                   Config{.layoutInput={2,6},.layoutAuxiliary={2,6},.layoutOutput={2,6}}), trace_(trace) {}
        ~MutationNode() override {
            ++trace_.total;
            trace_.destroyedOnControl = trace_.destroyedOnControl &&
                std::this_thread::get_id() == trace_.control;
            if (id() >= trace_.destructions.size()) {
                trace_.invalidIdentity = true;
                return;
            }
            if (++trace_.destructions[id()] != 1) trace_.duplicateDestruction = true;
        }
        void process(const ProcessContext&) noexcept override { ++trace_.processCalls; }
    private:
        MutationTrace& trace_;
    };

    struct XorShift32 {
        std::uint32_t state;
        std::uint32_t next() noexcept {
            state ^= state << 13U;
            state ^= state >> 17U;
            state ^= state << 5U;
            return state;
        }
    };

    [[nodiscard]] auto expectedError(reference::Decision decision) {
        switch (decision) {
            case reference::Decision::InvalidPort: return GraphError::InvalidPort;
            case reference::Decision::InvalidDirection: return GraphError::InvalidDirection;
            case reference::Decision::NodeNotFound: return GraphError::NodeNotFound;
            case reference::Decision::ConnectionExists: return GraphError::ConnectionExists;
            case reference::Decision::InputOccupied: return GraphError::InputOccupied;
            case reference::Decision::CycleDetected: return GraphError::CycleDetected;
            case reference::Decision::Accepted: break;
        }
        ADD_FAILURE() << "Accepted has no error";
        return GraphError::InvalidDirection;
    }

    struct Coverage {
        std::array<unsigned, 6> commands{};
        std::array<unsigned, 7> connectionDecisions{};
        unsigned aux = 0, undeclared = 0, fanOut = 0;
        unsigned selfCycle = 0, indirectCycle = 0, doubleDisconnect = 0;
        unsigned borrowedRemoval = 0;
    };

    /**
     * Control owns every plan; copies of expected IDs/edges come from the model.
     * Member order retires plans before their DAG. No snapshot is ever executed.
     */
    struct Scenario {
        MutationTrace trace;
        reference::Model model;
        DAG graph;
        std::array<Node*, operationCount + 1> addresses{};
        std::array<std::optional<ExecutionPlan>, 2> plans;
        std::array<std::vector<reference::NodeId>, 2> planIDs;
        std::array<std::vector<reference::Edge>, 2> planEdges;
        std::vector<reference::NodeId> deferred;
        unsigned expectedDestructions = 0;
        Coverage coverage;
        std::optional<reference::Operation> previous;
        reference::Outcome previousOutcome;

        [[nodiscard]] bool held(reference::NodeId id) const {
            for (const auto& ids : planIDs)
                if (std::find(ids.begin(), ids.end(), id) != ids.end()) return true;
            return false;
        }
    };

    /** Every deferred lifetime transition is checked when it becomes collectable. */
    void settleRetirements(Scenario& state) {
        for (const auto id:state.model.nodes) ASSERT_EQ(state.trace.destructions[id],0U);
        for (const auto& ids:state.planIDs)
            for (const auto id:ids) ASSERT_EQ(state.trace.destructions[id],0U);
        for (const auto id:state.deferred) ASSERT_EQ(state.trace.destructions[id],0U);
        const auto collected = state.graph.collectRetiredNodes();
        std::size_t expectedCollected = 0;
        auto cursor = state.deferred.begin();
        while (cursor != state.deferred.end()) {
            if (state.held(*cursor)) {
                EXPECT_EQ(state.trace.destructions[*cursor], 0U);
                ++cursor;
            } else {
                EXPECT_EQ(state.trace.destructions[*cursor], 1U);
                ++state.expectedDestructions;
                ++expectedCollected;
                cursor = state.deferred.erase(cursor);
            }
        }
        EXPECT_EQ(collected, expectedCollected);
        EXPECT_EQ(state.trace.total, state.expectedDestructions);
        EXPECT_FALSE(state.trace.invalidIdentity);
        EXPECT_FALSE(state.trace.duplicateDestruction);
        EXPECT_TRUE(state.trace.destroyedOnControl);
        EXPECT_EQ(state.trace.processCalls, 0U);
    }

    /**
     * @brief Observe exact membership, edges, occupancy, outputs and acyclicity.
     * @details Production topology is observed, never used to predict validity.
     * Expected owners/values come only from the independent model. No borrowed
     * collection view survives a mutation. Work is bounded by twelve live IDs
     * plus two retained snapshots, not by all identities ever generated.
     */
    void expectState(const Scenario& state) {
        ASSERT_LE(state.model.nodes.size(), reference::maxLive);
        ASSERT_TRUE(state.model.acyclic());
        // Diagnose a premature destruction before any lookup/traversal can
        // dereference the borrowed Node addresses retained by this scenario.
        for (const auto id:state.model.nodes) ASSERT_EQ(state.trace.destructions[id],0U);
        for (const auto& ids:state.planIDs)
            for (const auto id:ids) ASSERT_EQ(state.trace.destructions[id],0U);
        const auto order = topologicalOrder(state.graph);
        ASSERT_TRUE(order);
        auto actualIDs = *order;
        std::sort(actualIDs.begin(), actualIDs.end());
        EXPECT_EQ(actualIDs, state.model.nodes);
        for (const auto id : state.model.nodes) {
            ASSERT_EQ(state.graph.node(id), state.addresses[id]);
            EXPECT_EQ(state.trace.destructions[id], 0U);
        }
        ASSERT_EQ(state.graph.connections().size(), state.model.edges.size());
        std::vector<ChannelID> actualOccupancy, expectedOccupancy;
        for (std::size_t i = 0; i < state.model.edges.size(); ++i) {
            const auto& expected = state.model.edges[i];
            EXPECT_EQ(state.graph.connections()[i], (Connection{expected.from,expected.to}));
            actualOccupancy.push_back(state.graph.connections()[i].to);
            expectedOccupancy.push_back(expected.to);
            const auto source = std::find(order->begin(),order->end(),reference::nodeOf(expected.from));
            const auto destination = std::find(order->begin(),order->end(),reference::nodeOf(expected.to));
            ASSERT_NE(source,order->end()); ASSERT_NE(destination,order->end());
            EXPECT_LT(source,destination);
        }
        std::sort(actualOccupancy.begin(),actualOccupancy.end());
        std::sort(expectedOccupancy.begin(),expectedOccupancy.end());
        EXPECT_EQ(actualOccupancy,expectedOccupancy);
        EXPECT_EQ(std::adjacent_find(actualOccupancy.begin(),actualOccupancy.end()),actualOccupancy.end());
        ASSERT_EQ(state.graph.outputNodes().size(),state.model.outputs.size());
        for (std::size_t i=0;i<state.model.outputs.size();++i)
            EXPECT_EQ(state.graph.outputNodes()[i],state.model.outputs[i]);
        EXPECT_EQ(state.graph.node(state.model.nextID),nullptr);
        EXPECT_EQ(state.graph.node(0),nullptr);
        for (std::size_t generation=0;generation<state.plans.size();++generation) {
            if (!state.plans[generation]) {
                EXPECT_TRUE(state.planIDs[generation].empty());
                continue;
            }
            const auto& plan=*state.plans[generation];
            std::vector<NodeID> borrowedIDs;
            for (const auto* node:plan.tasks()) {
                borrowedIDs.push_back(node->id());
                EXPECT_EQ(node,state.addresses[node->id()]);
                EXPECT_EQ(state.trace.destructions[node->id()],0U);
            }
            auto sorted=borrowedIDs;
            std::sort(sorted.begin(),sorted.end());
            EXPECT_EQ(sorted,state.planIDs[generation]);
            for (const auto& edge:state.planEdges[generation]) {
                const auto source=std::find(borrowedIDs.begin(),borrowedIDs.end(),reference::nodeOf(edge.from));
                const auto destination=std::find(borrowedIDs.begin(),borrowedIDs.end(),reference::nodeOf(edge.to));
                ASSERT_NE(source,borrowedIDs.end()); ASSERT_NE(destination,borrowedIDs.end());
                EXPECT_LT(source,destination);
            }
        }
    }

    void refreshSnapshots(Scenario& state) {
        state.plans[1]=std::move(state.plans[0]);
        state.planIDs[1]=std::move(state.planIDs[0]);
        state.planEdges[1]=std::move(state.planEdges[0]);
        state.plans[0].reset();
        state.planIDs[0].clear();
        state.planEdges[0].clear();
        auto candidate=state.graph.compile();
        ASSERT_TRUE(candidate);
        state.plans[0].emplace(std::move(*candidate));
        state.planIDs[0]=state.model.nodes;
        state.planEdges[0]=state.model.edges;
    }

    void runOperation(Scenario& state,const reference::Operation& operation) {
        const auto prediction=state.model.predict(operation);
        ++state.coverage.commands[static_cast<std::size_t>(operation.kind)];
        const bool wasBorrowed=operation.kind==reference::Kind::Remove &&
                               prediction.decision==reference::Decision::Accepted && state.held(operation.id);
        switch (operation.kind) {
            case reference::Kind::Add: {
                const auto added=state.graph.add<MutationNode>(state.trace);
                ASSERT_TRUE(added);
                ASSERT_EQ(*added,prediction.id);
                ASSERT_LT(*added,state.addresses.size());
                state.addresses[*added]=state.graph.node(*added);
                break;
            }
            case reference::Kind::Disconnect: {
                const auto removed=state.graph.disconnect(operation.destination,operation.source);
                if (prediction.decision==reference::Decision::Accepted) {
                    ASSERT_TRUE(removed);
                    EXPECT_EQ(*removed,prediction.value);
                } else {
                    ASSERT_FALSE(removed);
                    EXPECT_EQ(removed.error(),expectedError(prediction.decision));
                }
                if (prediction.decision==reference::Decision::Accepted && !prediction.value && state.previous &&
                    state.previous->kind==reference::Kind::Disconnect &&
                    state.previous->source==operation.source && state.previous->destination==operation.destination &&
                    state.previousOutcome.decision==reference::Decision::Accepted && state.previousOutcome.value)
                    ++state.coverage.doubleDisconnect;
                break;
            }
            default: {
                Result result;
                if (operation.kind==reference::Kind::Connect) {
                    ++state.coverage.connectionDecisions[static_cast<std::size_t>(prediction.decision)];
                    result=state.graph.connect(operation.destination,operation.source);
                } else if (operation.kind==reference::Kind::Remove) result=state.graph.remove(operation.id);
                else if (operation.kind==reference::Kind::Mark) result=state.graph.markAsOutput(operation.id);
                else result=state.graph.unmarkAsOutput(operation.id);
                if (prediction.decision==reference::Decision::Accepted) ASSERT_TRUE(result);
                else {
                    ASSERT_FALSE(result);
                    EXPECT_EQ(result.error(),expectedError(prediction.decision));
                }
                break;
            }
        }
        state.model.apply(operation);
        if (operation.kind==reference::Kind::Remove && prediction.decision==reference::Decision::Accepted) {
            EXPECT_EQ(state.graph.node(operation.id),nullptr);
            if (wasBorrowed) {
                ++state.coverage.borrowedRemoval;
                state.deferred.push_back(operation.id);
                EXPECT_EQ(state.trace.destructions[operation.id],0U);
            } else {
                ++state.expectedDestructions;
                EXPECT_EQ(state.trace.destructions[operation.id],1U);
            }
        }
        if (operation.kind==reference::Kind::Connect) {
            if (prediction.decision==reference::Decision::CycleDetected) {
                if (reference::nodeOf(operation.source)==reference::nodeOf(operation.destination))
                    ++state.coverage.selfCycle;
                else ++state.coverage.indirectCycle;
            }
            if (prediction.decision==reference::Decision::Accepted) {
                if (reference::portOf(operation.destination)==reference::aux) ++state.coverage.aux;
                if (reference::indexOf(operation.source)==65535 || reference::indexOf(operation.destination)==65535)
                    ++state.coverage.undeclared;
                for (const auto& edge:state.model.edges)
                    if (edge.from==operation.source && edge.to!=operation.destination) {
                        ++state.coverage.fanOut;
                        break;
                    }
            }
        }
        state.previous=operation;
        state.previousOutcome=prediction;
        ASSERT_NO_FATAL_FAILURE(settleRetirements(state));
        ASSERT_NO_FATAL_FAILURE(expectState(state));
    }

    reference::NodeId pickNode(const reference::Model& model,XorShift32& rng) {
        const auto value=rng.next();
        if (model.nodes.empty() || value%6==0) {
            const auto missing=rng.next()%3;
            if (missing==0) return 0;
            if (missing==1) return std::numeric_limits<reference::NodeId>::max();
            return model.nextID; // Never allocated, even after many removals.
        }
        return model.nodes[value%model.nodes.size()];
    }

    reference::Operation generatedOperation(const reference::Model& model,XorShift32& rng) {
        auto kind=static_cast<reference::Kind>(rng.next()%6);
        if (kind==reference::Kind::Add && model.nodes.size()==reference::maxLive)
            return {reference::Kind::Remove,model.nodes[rng.next()%model.nodes.size()]};
        if (kind==reference::Kind::Add) return {kind};
        if (kind==reference::Kind::Remove || kind==reference::Kind::Mark || kind==reference::Kind::Unmark)
            return {kind,pickNode(model,rng)};
        if (!model.edges.empty() && rng.next()%4==0) {
            const auto edge=model.edges[rng.next()%model.edges.size()];
            return {kind,0,edge.from,edge.to}; // Reproducible duplicates/exact removals.
        }
        constexpr std::array<std::uint16_t,5> indices{0,1,2,6,65535};
        const auto source=pickNode(model,rng), destination=pickNode(model,rng);
        std::uint16_t sourcePort=reference::out;
        std::uint16_t destinationPort=rng.next()%2==0 ? reference::in : reference::aux;
        if (rng.next()%11==0) sourcePort=rng.next()%2==0 ? reference::in : std::uint16_t{0x1001};
        if (rng.next()%13==0) destinationPort=rng.next()%2==0 ? reference::out : std::uint16_t{0x8001};
        return {kind,0,reference::channel(source,sourcePort,indices[rng.next()%indices.size()]),
                       reference::channel(destination,destinationPort,indices[rng.next()%indices.size()])};
    }

    /** A deterministic prefix guarantees all required corner cases for each seed. */
    std::vector<reference::Operation> prefix() {
        using reference::Kind;
        const auto c=reference::channel;
        return {
            {Kind::Add},{Kind::Add},{Kind::Add},{Kind::Add},{Kind::Mark,2},
            {Kind::Connect,0,c(1,1,0),c(2,0,0)},
            {Kind::Connect,0,c(2,1,0),c(3,0,0)},
            {Kind::Connect,0,c(1,1,65535),c(3,2,65535)},
            {Kind::Connect,0,c(1,1,0),c(4,0,1)},
            {Kind::Connect,0,c(1,1,0),c(2,0,0)},
            {Kind::Connect,0,c(3,1,0),c(2,0,0)},
            {Kind::Connect,0,c(3,1,0),c(1,2,0)},
            {Kind::Connect,0,c(4,1,1),c(4,0,0)},
            {Kind::Connect,0,c(1,0x1001,0),c(4,0,0)},
            {Kind::Connect,0,c(1,1,0),c(4,1,0)},
            {Kind::Connect,0,c(1,1,0),c(0,0,0)},
            {Kind::Disconnect,0,c(1,1,65535),c(3,2,65535)},
            {Kind::Disconnect,0,c(1,1,65535),c(3,2,65535)},
            {Kind::Unmark,2},{Kind::Mark,2},{Kind::Remove,2},
            {Kind::Mark,2},{Kind::Unmark,2},{Kind::Remove,2},
            {Kind::Disconnect,0,c(2,1,0),c(3,0,0)},
            {Kind::Connect,0,c(1,1,65535),c(3,2,65535)},
            {Kind::Disconnect,0,c(1,1,0),c(4,1,0)},
            {Kind::Disconnect,0,c(1,0x1001,0),c(4,0,0)},
            {Kind::Mark,0},{Kind::Unmark,4},
            {Kind::Connect,0,c(1,1,0),c(3,0,65535)},
            {Kind::Connect,0,c(1,0,0),c(4,2,0)},
            {Kind::Connect,0,c(0x80000001U,1,0),c(4,0,0)},
            {Kind::Remove,4},{Kind::Add}};
    }

    class ReproducibleDAGMutations : public ::testing::TestWithParam<std::uint32_t> {};

    TEST_P(ReproducibleDAGMutations, TenThousandOperationsMatchIndependentStateAndRetirementModel) {
        Scenario state;
        const auto seed=GetParam();
        SCOPED_TRACE(::testing::Message()<<"seed=0x"<<std::hex<<seed);
        XorShift32 rng{seed};
        const auto scripted=prefix();
        ASSERT_NO_FATAL_FAILURE(expectState(state));
        for (std::size_t step=0;step<operationCount;++step) {
            // Retained snapshots deliberately span removals. All retire on
            // control; no callback ever starts, so quiescence is immediate.
            if (step==20 || step%173==0) ASSERT_NO_FATAL_FAILURE(refreshSnapshots(state));
            if (step%257==0) {
                state.plans[1].reset(); state.planIDs[1].clear(); state.planEdges[1].clear();
            }
            const auto operation=step<scripted.size() ? scripted[step] : generatedOperation(state.model,rng);
            SCOPED_TRACE(::testing::Message()<<"seed=0x"<<std::hex<<seed<<std::dec<<" step="<<step
                <<" operation="<<static_cast<unsigned>(operation.kind)<<" id="<<operation.id
                <<" source=0x"<<std::hex<<operation.source<<" destination=0x"<<operation.destination);
            ASSERT_NO_FATAL_FAILURE(runOperation(state,operation));
        }
        for (const auto hits:state.coverage.commands) EXPECT_GT(hits,0U);
        for (const auto hits:state.coverage.connectionDecisions) EXPECT_GT(hits,0U);
        EXPECT_GT(state.coverage.aux,0U); EXPECT_GT(state.coverage.undeclared,0U);
        EXPECT_GT(state.coverage.fanOut,0U); EXPECT_GT(state.coverage.selfCycle,0U);
        EXPECT_GT(state.coverage.indirectCycle,0U); EXPECT_GT(state.coverage.doubleDisconnect,0U);
        EXPECT_GT(state.coverage.borrowedRemoval,0U);
        for (std::size_t generation=0;generation<state.plans.size();++generation) {
            state.plans[generation].reset(); state.planIDs[generation].clear(); state.planEdges[generation].clear();
        }
        ASSERT_NO_FATAL_FAILURE(settleRetirements(state));
        std::size_t cleanupStep=operationCount;
        while (!state.model.nodes.empty()) {
            const reference::Operation remove{reference::Kind::Remove,state.model.nodes.back()};
            SCOPED_TRACE(::testing::Message()<<"step="<<cleanupStep++<<" operation=cleanup-remove id="<<remove.id);
            ASSERT_NO_FATAL_FAILURE(runOperation(state,remove));
        }
        EXPECT_EQ(state.trace.total,state.model.nextID-1);
        for (reference::NodeId id=1;id<state.model.nextID;++id) EXPECT_EQ(state.trace.destructions[id],1U);
        EXPECT_EQ(state.trace.processCalls,0U);
        EXPECT_TRUE(state.trace.destroyedOnControl);
    }

    INSTANTIATE_TEST_SUITE_P(FixedSeeds,ReproducibleDAGMutations,
        ::testing::Values(std::uint32_t{0xC0FFEE42U},std::uint32_t{0x5EED1234U}));
}
