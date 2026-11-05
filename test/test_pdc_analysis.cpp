/**
 * @file test_pdc_analysis.cpp
 * @brief Tests for pdc analysis.
 * @author Mika
 */
#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>
#include <type_traits>
#include <vector>
#include <gtest/gtest.h>
import mka.audio.dsp.dag;

namespace {
    using namespace mka::audio::dsp;
    static_assert(std::same_as<decltype(NodeLatency{}.intrinsic),std::uint32_t>);
    static_assert(std::same_as<decltype(NodeLatency{}.arrival),std::uint64_t>);
    static_assert(std::same_as<decltype(NodeLatency{}.cumulative),std::uint64_t>);
    static_assert(noexcept(checkedLatencyAdd(0,0)));
    /// @brief Build the complete logical edge identity used by the fixture.
    constexpr Connection edge(NodeID from,NodeID to,PortType port=PortType::In,ChannelIndex channel=0) {
        return {encode(from,PortType::Out,0),encode(to,port,channel)};
    }
    /// @brief Exposes intrinsic latency while recording preparation-time metadata queries.
    class MetadataNode final : public Node {
    public:
        /// @brief Copy logical layouts and store the declared intrinsic latency for analysis.
        MetadataNode(NodeID id,LatencySamples latency,Config cfg={.layoutInput={0},.layoutAuxiliary={0},.layoutOutput={0}})
            : Node(id,cfg),latency_(latency) {}
        /// @brief Return the fixed algorithmic delay declared by this fixture processor.
        LatencySamples latencySamples() const noexcept override { return latency_; }
        /// @brief Leave sample buffers unchanged; this processor supplies structural metadata only.
        void process(const ProcessContext&) noexcept override {}
    private:
        /// @brief Fixed intrinsic algorithmic latency declared by the processor.
        const LatencySamples latency_;
    };
    /// @brief Check a node's intrinsic, arrival and cumulative latency entries.
    void expectNode(const PdcAnalysis& analysis,NodeID id,LatencySamples p,std::uint64_t a,std::uint64_t l) {
        SCOPED_TRACE(::testing::Message()<<"NodeID="<<id<<" P="<<p<<" A="<<a<<" L="<<l);
        const auto* value=analysis.find(id); ASSERT_NE(value,nullptr);
        EXPECT_EQ(value->intrinsic,p); EXPECT_EQ(value->arrival,a); EXPECT_EQ(value->cumulative,l);
    }

    /// @brief Test case: empty and independent sources have zero arrival.
    TEST(PdcAnalysis, EmptyAndIndependentSourcesHaveZeroArrival) {
        const auto empty=analyzeLatencies(LatencySnapshot{}, {}, {}); ASSERT_TRUE(empty);
        EXPECT_TRUE(empty->nodes.empty()); EXPECT_EQ(empty->find(1),nullptr);
        const LatencySnapshot snapshot{48000,{{1,0},{2,3},{3,8},{4,512}}};
        const std::array<NodeID,4> order{4,2,1,3};
        const auto analysis=analyzeLatencies(snapshot,order,{}); ASSERT_TRUE(analysis);
        for (const auto& value : snapshot.nodes) expectNode(*analysis,value.node,value.samples,0,value.samples);
    }

    /// @brief Test case: chain accumulates intrinsic latency exactly once.
    TEST(PdcAnalysis, ChainAccumulatesIntrinsicLatencyExactlyOnce) {
        const LatencySnapshot snapshot{48000,{{1,3},{2,8},{3,512}}};
        const std::array<NodeID,3> order{1,2,3}; const std::array edges{edge(1,2),edge(2,3)};
        const auto analysis=analyzeLatencies(snapshot,order,edges); ASSERT_TRUE(analysis);
        expectNode(*analysis,1,3,0,3); expectNode(*analysis,2,8,3,11); expectNode(*analysis,3,512,11,523);
    }

    /// @brief Test case: diamond and auxiliary take maximum of all audio predecessors.
    TEST(PdcAnalysis, DiamondAndAuxiliaryTakeMaximumOfAllAudioPredecessors) {
        const LatencySnapshot snapshot{48000,{{1,3},{2,8},{3,512},{4,64}}};
        const std::array<NodeID,4> order{1,3,2,4};
        const std::array edges{edge(3,4,PortType::Aux),edge(1,2),edge(2,4),edge(1,3)};
        const auto analysis=analyzeLatencies(snapshot,order,edges); ASSERT_TRUE(analysis);
        expectNode(*analysis,1,3,0,3); expectNode(*analysis,2,8,3,11);
        expectNode(*analysis,3,512,3,515); expectNode(*analysis,4,64,515,579);
    }

    /// @brief Test case: intrinsic uint32 limits accumulate in uint64.
    TEST(PdcAnalysis, IntrinsicUint32LimitsAccumulateInUint64) {
        const auto maximum=std::numeric_limits<LatencySamples>::max();
        const LatencySnapshot snapshot{48000,{{1,maximum},{2,maximum}}};
        const std::array<NodeID,2> order{1,2}; const std::array edges{edge(1,2)};
        const auto analysis=analyzeLatencies(snapshot,order,edges); ASSERT_TRUE(analysis);
        expectNode(*analysis,2,maximum,maximum,std::uint64_t{maximum}*2);
    }

    /// @brief Test case: checked addition rejects overflow before adding.
    TEST(PdcAnalysis, CheckedAdditionRejectsOverflowBeforeAdding) {
        constexpr auto maximum=std::numeric_limits<std::uint64_t>::max();
        EXPECT_EQ(checkedLatencyAdd(maximum,0).value(),maximum);
        EXPECT_EQ(checkedLatencyAdd(maximum-512,512).value(),maximum);
        EXPECT_EQ(checkedLatencyAdd(maximum-512,513).error().code,ErrorCode::LatencyOverflow);
        EXPECT_EQ(checkedLatencyAdd(maximum,1).error().code,ErrorCode::LatencyOverflow);
        EXPECT_EQ(checkedLatencyAdd(1,maximum).error().code,ErrorCode::LatencyOverflow);
    }

    /// @brief Test case: invalid snapshot order and audio endpoints return control errors.
    TEST(PdcAnalysis, InvalidSnapshotOrderAndAudioEndpointsReturnControlErrors) {
        const LatencySnapshot snapshot{48000,{{1,3},{2,8}}};
        const std::array<NodeID,2> order{1,2},reversed{2,1},duplicate{1,1};
        const std::array edges{edge(1,2)};
        EXPECT_EQ(analyzeLatencies(snapshot,reversed,edges).error().code,ErrorCode::InvalidParameter);
        EXPECT_EQ(analyzeLatencies(snapshot,duplicate,{}).error().code,ErrorCode::InvalidParameter);
        EXPECT_EQ(analyzeLatencies(LatencySnapshot{},order,{}).error().code,ErrorCode::InvalidParameter);
        EXPECT_EQ(analyzeLatencies(LatencySnapshot{0,{{2,8},{1,3}}},order,{}).error().code,ErrorCode::InvalidParameter);
        EXPECT_EQ(analyzeLatencies(LatencySnapshot{0,{{0,0},{2,8}}},order,{}).error().code,ErrorCode::InvalidParameter);
        EXPECT_EQ(analyzeLatencies(LatencySnapshot{0,{{1,0},{1,0}}},order,{}).error().code,ErrorCode::InvalidParameter);
        const std::array absentFrom{edge(9,2)},absentTo{edge(1,9)};
        EXPECT_EQ(analyzeLatencies(snapshot,order,absentFrom).error().code,ErrorCode::NodeNotFound);
        EXPECT_EQ(analyzeLatencies(snapshot,order,absentTo).error().code,ErrorCode::NodeNotFound);
        const std::array wrongDirection{Connection{encode(1,PortType::In,0),encode(2,PortType::In,0)}};
        EXPECT_EQ(analyzeLatencies(snapshot,order,wrongDirection).error().code,ErrorCode::InvalidDirection);
        const std::array wrongPort{Connection{(ChannelID{1}<<32)|(ChannelID{7}<<16),encode(2,PortType::In,0)}};
        EXPECT_EQ(analyzeLatencies(snapshot,order,wrongPort).error().code,ErrorCode::InvalidPort);
    }

    /// @brief Test case: compilation uses only materialized audio ports and keeps structural edges.
    TEST(PdcAnalysis, CompilationUsesOnlyMaterializedAudioPortsAndKeepsStructuralEdges) {
        DAG graph;
        const auto fast=graph.add<MetadataNode>(3U),slow=graph.add<MetadataNode>(8U);
        const auto ignored=graph.add<MetadataNode>(512U,Config{}),join=graph.add<MetadataNode>(64U);
        ASSERT_TRUE(fast); ASSERT_TRUE(slow); ASSERT_TRUE(ignored); ASSERT_TRUE(join);
        for (const auto connection : {edge(*fast,*join),edge(*slow,*join,PortType::Aux),
                                     edge(*ignored,*join,PortType::In,9)})
            ASSERT_TRUE(graph.connect(connection.to,connection.from));
        // A declared input with an undeclared producer output reads silence.
        const auto sink=graph.add<MetadataNode>(1U); ASSERT_TRUE(sink);
        ASSERT_TRUE(graph.connect(encode(*sink,PortType::In,0),encode(*ignored,PortType::Out,0)));
        const auto count=graph.connections().size();
        auto plan=graph.compile(BackendConfig{.maxFrames=4,.sampleRate=48000}); ASSERT_TRUE(plan);
        ASSERT_NE(plan->pdcAnalysis(),nullptr);
        expectNode(*plan->pdcAnalysis(),*join,64,8,72);
        expectNode(*plan->pdcAnalysis(),*sink,1,0,1);
        EXPECT_EQ(graph.connections().size(),count);
        EXPECT_LT(*plan->taskIndexOf(*ignored),*plan->taskIndexOf(*sink));
    }

    /// @brief Test case: node storage order does not replace topological order.
    TEST(PdcAnalysis, NodeStorageOrderDoesNotReplaceTopologicalOrder) {
        DAG graph;
        const auto sink=graph.add<MetadataNode>(8U),middle=graph.add<MetadataNode>(3U),source=graph.add<MetadataNode>(512U);
        ASSERT_TRUE(sink); ASSERT_TRUE(middle); ASSERT_TRUE(source);
        ASSERT_TRUE(graph.connect(encode(*sink,PortType::In,0),encode(*middle,PortType::Out,0)));
        ASSERT_TRUE(graph.connect(encode(*middle,PortType::Aux,0),encode(*source,PortType::Out,0)));
        auto plan=graph.compile(BackendConfig{.maxFrames=4}); ASSERT_TRUE(plan);
        EXPECT_EQ(plan->pdcAnalysis()->nodes.front().node,*source);
        expectNode(*plan->pdcAnalysis(),*source,512,0,512);
        expectNode(*plan->pdcAnalysis(),*middle,3,512,515);
        expectNode(*plan->pdcAnalysis(),*sink,8,515,523);
    }
}
