#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <limits>
#include <map>
#include <span>
#include <thread>
#include <tuple>
#include <vector>
#include <gtest/gtest.h>
import mka.audio.dsp.plan_switcher;
#include "pdc_render_fixture.hpp"

namespace {
    using namespace mka::audio::dsp;

    // Analysis-only test double: rendering is covered by the sample oracles in
    // PdcEdges/Fanout/Outputs/Budget with real, independently owned DSP delays.
    class OracleNode final : public Node {
    public:
        OracleNode(NodeID id,LatencySamples latency,Config config) : Node(id,config),latency_(latency) {}
        LatencySamples latencySamples() const noexcept override { ++queries; return latency_; }
        void process(const ProcessContext&) noexcept override {}
        mutable unsigned queries=0;
    private:
        const LatencySamples latency_;
    };
    struct ModelNode { NodeID id; LatencySamples intrinsic; Config config; };
    struct ModelEdge {
        NodeID from,to;
        PortType port;
        ChannelIndex sourceIndex,destinationIndex;
        Connection identity() const {
            return {encode(from,PortType::Out,sourceIndex),encode(to,port,destinationIndex)};
        }
    };
    struct Model {
        DAG graph;
        std::vector<ModelNode> nodes;
        std::vector<ModelEdge> edges;
        NodeID add(LatencySamples p,Config cfg={.layoutInput={0,1,2,3,4,5,6,7},
                                               .layoutAuxiliary={0,1,2,3,4,5,6,7},.layoutOutput={0}}) {
            const auto id=graph.add<OracleNode>(p,cfg);
            if (!id) return invalidNodeID;
            nodes.push_back({*id,p,cfg}); return *id;
        }
        Result link(NodeID from,NodeID to,PortType port=PortType::In,
                    ChannelIndex destinationIndex=0,ChannelIndex sourceIndex=0) {
            const ModelEdge edge{from,to,port,sourceIndex,destinationIndex};
            const auto connection=edge.identity();
            const auto result=graph.connect(connection.to,connection.from);
            if (result) edges.push_back(edge);
            return result;
        }
        const ModelNode& node(NodeID id) const {
            return *std::find_if(nodes.begin(),nodes.end(),[id](const auto& value) { return value.id==id; });
        }
        bool audio(const ModelEdge& edge) const {
            // Independent test-model rule. No compiled signal/slot/PDC table
            // or production filter/topological helper participates in this oracle.
            const auto& producer=node(edge.from).config.layoutOutput;
            const auto& consumer=edge.port==PortType::In ? node(edge.to).config.layoutInput :
                node(edge.to).config.layoutAuxiliary;
            return std::count(producer.begin(),producer.end(),edge.sourceIndex)!=0 &&
                std::count(consumer.begin(),consumer.end(),edge.destinationIndex)!=0;
        }
    };
    struct Reference { LatencySamples p; std::uint64_t a,l; };
    std::map<NodeID,Reference> reference(const Model& model) {
        // Recursive longest-path evaluation differs from the production's
        // iterative topological pass and never asks a Node for its declaration.
        std::map<NodeID,Reference> result;
        std::function<std::uint64_t(NodeID)> finish=[&](NodeID id) {
            if (const auto it=result.find(id); it!=result.end()) return it->second.l;
            std::uint64_t arrival=0;
            for (const auto& edge : model.edges)
                if (edge.to==id && model.audio(edge)) {
                    const auto predecessor=finish(edge.from);
                    if (predecessor>arrival) arrival=predecessor;
                }
            const auto intrinsic=model.node(id).intrinsic;
            // Small test graphs are representable; boundary additions have a
            // separate table below, without calling a production arithmetic helper.
            const auto output=arrival+std::uint64_t{intrinsic};
            result.emplace(id,Reference{intrinsic,arrival,output}); return output;
        };
        for (const auto& node : model.nodes) finish(node.id);
        return result;
    }

    void compare(const Model& model,const ExecutionPlan& plan) {
        const auto expected=reference(model);
        const auto* snapshot=plan.latencySnapshot(); ASSERT_NE(snapshot,nullptr);
        const auto* actual=plan.pdcAnalysis(); ASSERT_NE(actual,nullptr);
        ASSERT_EQ(actual->nodes.size(),expected.size());
        ASSERT_EQ(snapshot->nodes.size(),expected.size());
        for (const auto& [id,value] : expected) {
            SCOPED_TRACE(::testing::Message()<<"NodeID="<<id<<" P="<<value.p<<" A="<<value.a<<" L="<<value.l);
            const auto* node=actual->find(id); ASSERT_NE(node,nullptr);
            const auto* declared=snapshot->find(id); ASSERT_NE(declared,nullptr);
            EXPECT_EQ(declared->samples,value.p); EXPECT_EQ(node->intrinsic,value.p);
            EXPECT_EQ(node->arrival,value.a); EXPECT_EQ(node->cumulative,value.l);
        }
        std::size_t audioCount=0,positiveCount=0;
        std::uint64_t historySamples=0;
        for (const auto& edge : model.edges) {
            const auto connection=edge.identity();
            SCOPED_TRACE(::testing::Message()<<"NodeID="<<edge.to<<" from="<<connection.from<<" to="<<connection.to
                <<" P="<<expected.at(edge.to).p<<" A="<<expected.at(edge.to).a<<" L="<<expected.at(edge.to).l);
            ASSERT_TRUE(plan.taskIndexOf(edge.from)); ASSERT_TRUE(plan.taskIndexOf(edge.to));
            EXPECT_LT(*plan.taskIndexOf(edge.from),*plan.taskIndexOf(edge.to)); // Even ignored structural edges.
            const auto* delay=actual->find(connection);
            if (!model.audio(edge)) { EXPECT_EQ(delay,nullptr); continue; }
            ++audioCount; ASSERT_NE(delay,nullptr);
            const auto d=expected.at(edge.to).a-expected.at(edge.from).l;
            EXPECT_EQ(delay->samples,d)<<"D="<<d<<" source L="<<expected.at(edge.from).l;
            if (d) ++positiveCount;
            historySamples+=d;
        }
        EXPECT_EQ(actual->edges.size(),audioCount); EXPECT_EQ(plan.delayTaskCount(),positiveCount);
        EXPECT_EQ(plan.preparedMemoryUsage().historyBytes,historySamples*sizeof(float));
    }

    TEST(PdcOracle, EveryFourNodeDagInAllTwentyFourIdentityOrdersMatchesEveryNodeAndExactEdge) {
        std::array<unsigned,4> permutation{0,1,2,3};
        unsigned cases=0;
        do {
            for (unsigned mask=0;mask<64;++mask) {
                SCOPED_TRACE(::testing::Message()<<"case="<<cases<<" edge mask="<<mask);
                Model model; std::array<NodeID,4> ids{};
                constexpr std::array<LatencySamples,4> intrinsic{0,64,256,512};
                for (unsigned i=0;i<4;++i) { ids[i]=model.add(intrinsic[i]); ASSERT_NE(ids[i],invalidNodeID); }
                unsigned bit=0;
                for (unsigned to=1;to<4;++to) for (unsigned from=0;from<to;++from,++bit)
                    if (mask&(1U<<bit))
                        ASSERT_TRUE(model.link(ids[permutation[from]],ids[permutation[to]],
                            (from+to)%2 ? PortType::Aux : PortType::In,static_cast<ChannelIndex>(from)));
                ASSERT_TRUE(model.graph.markAsOutput(ids[1])); ASSERT_TRUE(model.graph.markAsOutput(ids[3]));
                const BackendConfig cfg{.maxFrames=4,.sampleRate=48000};
                auto plan=model.graph.compile(cfg); ASSERT_TRUE(plan);
                ASSERT_NO_FATAL_FAILURE(compare(model,*plan));
                ASSERT_TRUE(plan->exec(BackendBlock{.frames=4}));
                ASSERT_TRUE(plan->exec(BackendBlock{.frames=0}));
                for (const auto& node : model.nodes)
                    EXPECT_EQ(static_cast<const OracleNode*>(model.graph.node(node.id))->queries,1U);
                ++cases;
            }
        } while (std::next_permutation(permutation.begin(),permutation.end()));
        EXPECT_EQ(cases,1536U);
    }

    TEST(PdcOracle, FourWayJoinFanoutTwoLogicalOutputsIgnoredPortsAndIdentityGapsMatchReference) {
        Model model;
        const auto gap=model.graph.add<OracleNode>(0U,Config{}); ASSERT_TRUE(gap); ASSERT_TRUE(model.graph.remove(*gap));
        std::array<NodeID,4> sources{}; constexpr std::array<LatencySamples,4> intrinsic{0,64,256,512};
        for (unsigned i=0;i<4;++i) { sources[i]=model.add(intrinsic[i]); ASSERT_NE(sources[i],invalidNodeID); }
        const auto join=model.add(8U),other=model.add(3U),silent=model.add(64U);
        ASSERT_NE(join,invalidNodeID); ASSERT_NE(other,invalidNodeID); ASSERT_NE(silent,invalidNodeID);
        for (ChannelIndex i=0;i<4;++i)
            ASSERT_TRUE(model.link(sources[i],join,i%2 ? PortType::Aux : PortType::In,i));
        ASSERT_TRUE(model.link(sources[0],other)); ASSERT_TRUE(model.link(sources[1],other,PortType::Aux));
        ASSERT_TRUE(model.link(sources[3],other,PortType::In,99)); // Unprocessed destination.
        ASSERT_TRUE(model.link(sources[3],silent,PortType::Aux,0,99)); // Unproduced source.
        ASSERT_TRUE(model.graph.markAsOutput(join)); ASSERT_TRUE(model.graph.markAsOutput(other));
        auto plan=model.graph.compile(BackendConfig{.maxFrames=4}); ASSERT_TRUE(plan);
        ASSERT_NO_FATAL_FAILURE(compare(model,*plan));
        EXPECT_EQ(plan->pdcAnalysis()->find(join)->cumulative,520U);
        EXPECT_EQ(plan->pdcAnalysis()->find(other)->cumulative,67U);
        EXPECT_EQ(plan->pdcAnalysis()->find(silent)->arrival,0U);
        EXPECT_EQ(plan->pdcAnalysis()->find(pdc_test::edge(sources[0],join))->samples,512U);
        EXPECT_EQ(plan->pdcAnalysis()->find(pdc_test::edge(sources[0],other))->samples,64U);
    }

    TEST(PdcOracle, ZeroLatencyDiamondAndNonzeroLatencyChainAreIndependentOfContainerOrder) {
        for (const auto intrinsic : {0U,3U,8U}) {
            Model model;
            const auto sink=model.add(intrinsic),left=model.add(intrinsic),right=model.add(intrinsic),source=model.add(intrinsic);
            ASSERT_NE(sink,invalidNodeID); ASSERT_NE(left,invalidNodeID); ASSERT_NE(right,invalidNodeID); ASSERT_NE(source,invalidNodeID);
            for (const auto connection : {pdc_test::edge(source,left),pdc_test::edge(source,right),
                                         pdc_test::edge(left,sink),pdc_test::edge(right,sink,PortType::Aux)}) {
                ASSERT_TRUE(model.link(decodeNode(connection.from),decodeNode(connection.to),decodePort(connection.to)));
            }
            auto plan=model.graph.compile(BackendConfig{.maxFrames=4}); ASSERT_TRUE(plan);
            ASSERT_NO_FATAL_FAILURE(compare(model,*plan));
            EXPECT_EQ(plan->pdcAnalysis()->find(sink)->cumulative,3U*intrinsic);
        }
        Model chain;
        const auto sink=chain.add(512U),middle=chain.add(8U),source=chain.add(3U);
        ASSERT_NE(sink,invalidNodeID); ASSERT_NE(middle,invalidNodeID); ASSERT_NE(source,invalidNodeID);
        ASSERT_TRUE(chain.link(source,middle,PortType::Aux)); ASSERT_TRUE(chain.link(middle,sink));
        auto plan=chain.graph.compile(BackendConfig{.maxFrames=4}); ASSERT_TRUE(plan);
        ASSERT_NO_FATAL_FAILURE(compare(chain,*plan));
        EXPECT_EQ(plan->pdcAnalysis()->find(sink)->cumulative,523U);
    }

    TEST(PdcOracle, WideCumulativeTableAndRejectedHugeHistoryPreserveThePreviousGeneration) {
        Model model;
        const auto fast=model.add(0),a=model.add(UINT32_MAX),b=model.add(UINT32_MAX),sink=model.add(0);
        ASSERT_NE(fast,invalidNodeID); ASSERT_NE(a,invalidNodeID); ASSERT_NE(b,invalidNodeID); ASSERT_NE(sink,invalidNodeID);
        ASSERT_TRUE(model.link(a,b)); ASSERT_TRUE(model.link(b,sink));
        const BackendConfig cfg{.maxFrames=4,.memoryBudgetBytes=1024U*1024U};
        auto plan=model.graph.compile(cfg); ASSERT_TRUE(plan);
        ASSERT_NO_FATAL_FAILURE(compare(model,*plan));
        const auto table=plan->pdcAnalysis()->nodes; const auto* snapshot=plan->latencySnapshot()->nodes.data();
        EXPECT_EQ(plan->pdcAnalysis()->find(sink)->cumulative,std::uint64_t{UINT32_MAX}*2);
        ASSERT_TRUE(model.link(fast,sink,PortType::Aux));
        const auto rejected=model.graph.compile(cfg); ASSERT_FALSE(rejected);
        if constexpr (sizeof(std::size_t)>=8) {
            EXPECT_EQ(rejected.error().code,ErrorCode::MemoryBudgetExceeded);
        } else {
            EXPECT_EQ(rejected.error().code,ErrorCode::ArithmeticOverflow);
        }
        EXPECT_EQ(plan->pdcAnalysis()->nodes,table); EXPECT_EQ(plan->latencySnapshot()->nodes.data(),snapshot);
        EXPECT_EQ(plan->delayTaskCount(),0U); ASSERT_TRUE(plan->exec(BackendBlock{.frames=4}));
    }

    TEST(PdcOracle, Uint64BoundaryResultsAndErrorsMatchIndependentKnownSums) {
        constexpr auto m=std::numeric_limits<std::uint64_t>::max();
        using Case=std::tuple<std::uint64_t,std::uint64_t,bool,std::uint64_t>;
        const std::array<Case,6> cases{Case{m,0,true,m},Case{m-512,512,true,m},
            Case{m-512,513,false,0},Case{m,1,false,0},Case{0,m,true,m},Case{1,m,false,0}};
        for (const auto [a,p,fits,l] : cases) {
            SCOPED_TRACE(::testing::Message()<<"A="<<a<<" P="<<p<<" expected L="<<l);
            const auto actual=checkedLatencyAdd(a,p);
            ASSERT_EQ(actual.has_value(),fits);
            if (fits) EXPECT_EQ(*actual,l);
            else EXPECT_EQ(actual.error().code,ErrorCode::LatencyOverflow);
        }
    }

    TEST(PdcOracle, ConcurrentAutomaticCandidatesAndSwapsKeepTablesCoherentAndAudioBounded) {
        DAG graph; PlanSwitcher switcher;
        const auto a=graph.add<pdc_test::UniformDelayNode>(3U),b=graph.add<pdc_test::UniformDelayNode>(8U);
        const auto sink=graph.add<pdc_test::UniformDelayNode>(0U,pdc_test::ports(),0U);
        ASSERT_TRUE(a); ASSERT_TRUE(b); ASSERT_TRUE(sink);
        ASSERT_TRUE(pdc_test::connect(graph,pdc_test::edge(*a,*sink)));
        ASSERT_TRUE(pdc_test::connect(graph,pdc_test::edge(*b,*sink,PortType::Aux)));
        ASSERT_TRUE(graph.markAsOutput(*sink));
        const BackendConfig cfg{.maxFrames=4,.inputChannels=1,.outputChannels=1,.sampleRate=48000};
        ASSERT_TRUE(switcher.initialize(graph,cfg));
        std::atomic<bool> running{true},audioOK{true}; std::atomic<unsigned> callbacks{0};
        std::thread audio([&] {
            std::array<float,6> input{-99,1,1,1,1,-99},output{}; output.fill(-99);
            const std::array<const float*,1> inputs{input.data()+1}; const std::array outputs{output.data()+1};
            GenerationID last=0;
            while (running.load(std::memory_order_acquire)) {
                switcher.exec(mka::audio::AudioProcessContext{inputs,outputs,4,4,4});
                const auto generation=switcher.audioGeneration();
                bool valid=generation>=last && generation<=1001;
                last=generation;
                for (unsigned f=1;f<=4;++f) valid=valid && output[f]>=0.F && output[f]<=2.F;
                valid=valid && input.front()==-99.F && input.back()==-99.F && output.front()==-99.F && output.back()==-99.F;
                if (!valid) audioOK.store(false,std::memory_order_relaxed);
                callbacks.fetch_add(1,std::memory_order_relaxed);
            }
        });
        bool controlOK=true;
        for (unsigned generation=2;generation<=1001 && controlOK;++generation) {
            if (!switcher.prepareInactivePlan(graph,cfg)) { controlOK=false; break; }
            const auto* ready=switcher.readyPlan();
            controlOK=ready && ready->pdcAnalysis()->find(*sink)->arrival==8 &&
                ready->pdcAnalysis()->find(pdc_test::edge(*a,*sink))->samples==5 && ready->delayTaskCount()==1;
            if (!controlOK || !switcher.publishReadyPlan()) { controlOK=false; break; }
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
            while (switcher.handshakeState()!=HandshakeState::Acknowledged) {
                if (std::chrono::steady_clock::now()>deadline) { controlOK=false; break; }
                std::this_thread::yield();
            }
            if (controlOK && !switcher.retireAcknowledgedPlan()) controlOK=false;
        }
        running.store(false,std::memory_order_release); audio.join();
        EXPECT_TRUE(controlOK); EXPECT_TRUE(audioOK.load()); EXPECT_GE(callbacks.load(),1000U);
        EXPECT_EQ(switcher.controlGeneration(),1001U);
        ASSERT_TRUE(switcher.shutdownAfterBackendStopped());
    }
}
