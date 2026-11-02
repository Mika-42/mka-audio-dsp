#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>
#include <gtest/gtest.h>

import mka.audio.dsp.dag;
#include "pool_pipeline.hpp"

namespace {
    using namespace mka::audio::dsp;
    using mka::audio::AudioProcessContext;
    constexpr FrameCount maximum = 4096;
    constexpr float sentinel = -87654.F;
    using Samples = std::array<std::vector<float>,2>;

    Samples guardedSamples() {
        return {std::vector<float>(maximum+2, sentinel), std::vector<float>(maximum+2, sentinel)};
    }

    /**
     * Independent, intentionally slow graph interpreter: no BufferId, compiled
     * topology, lifetime table, slot map, pool or production DelayTask. Each
     * logical production has its own vector. Delays are shift registers, not
     * the engine's circular histories. Nodes in this corpus are inserted in a
     * known producer-first order; edges always go from a lower to a higher id.
     */
    struct ReferenceRenderer {
        const pool_test::Pipeline& pipeline;
        std::vector<Connection> edges;
        std::vector<NodeID> marked;
        std::vector<ConnectionDelay> delays;
        std::vector<std::vector<float>> history;
        std::vector<Samples> produced;
        Samples physical = guardedSamples();

        ReferenceRenderer(const pool_test::Pipeline& p, std::span<const ConnectionDelay> requested)
            : pipeline(p), edges(p.graph.connections().begin(),p.graph.connections().end()),
              marked(p.graph.outputNodes().begin(),p.graph.outputNodes().end()),
              delays(requested.begin(),requested.end()) {
            for (std::size_t i=0;i<p.ids.size();++i) produced.push_back(guardedSamples());
            for (const auto& delay : delays) history.emplace_back(delay.samples,0.F);
        }

        void render(const std::span<const float* const> inputs, FrameCount frames) {
            for (auto& channel : physical) {
                std::fill(channel.begin(),channel.end(),sentinel);
                std::fill_n(channel.data()+1,frames,0.F);
            }
            for (std::size_t task=0;task<pipeline.ids.size();++task) {
                const auto id=pipeline.ids[task];
                const bool source=std::ranges::none_of(edges,[&](auto e){return decodeNode(e.to)==id;});
                std::array<std::array<std::vector<float>,2>,2> ports;
                for (unsigned port=0;port<2;++port) for (ChannelIndex c=0;c<2;++c) {
                    auto& values=ports[port][c]; values.resize(frames,0.F);
                    const auto target=encode(id,port==0 ? PortType::In : PortType::Aux,c);
                    const auto edge=std::ranges::find(edges,target,&Connection::to);
                    if ((port==0 && source) || edge==edges.end()) continue;
                    const auto producer=std::ranges::find(pipeline.ids,decodeNode(edge->from));
                    const auto lane=decodeChannel(edge->from);
                    if (producer==pipeline.ids.end() || lane>=2) continue;
                    const auto index=static_cast<std::size_t>(producer-pipeline.ids.begin());
                    for (FrameCount f=0;f<frames;++f) values[f]=produced[index][lane][f+1];
                    const auto delay=std::ranges::find_if(delays,[&](auto d){return d.connection.to==target;});
                    if (delay==delays.end() || delay->samples==0) continue;
                    auto& shift=history[static_cast<std::size_t>(delay-delays.begin())];
                    for (FrameCount f=0;f<frames;++f) {
                        const auto input=values[f];
                        values[f]=shift.front();
                        std::move(shift.begin()+1,shift.end(),shift.begin());
                        shift.back()=input;
                    }
                }
                for (auto& channel : produced[task]) std::fill(channel.begin(),channel.end(),sentinel);
                for (FrameCount f=0;f<frames;++f) {
                    float value=static_cast<float>(id);
                    if (source) {
                        for (const auto* channel : inputs) value+=channel[f]*0.5F;
                    } else {
                        for (unsigned c=0;c<2;++c) value+=ports[0][c][f]*0.5F;
                    }
                    for (unsigned c=0;c<2;++c) value+=ports[1][c][f]*0.25F;
                    for (unsigned c=0;c<2;++c) {
                        produced[task][c][f+1]=value+static_cast<float>(c);
                        if (std::ranges::find(marked,id)!=marked.end()) physical[c][f+1]=value+static_cast<float>(c);
                    }
                }
            }
        }
    };

    void checkCorpus(pool_test::Pipeline& p, const std::span<const ConnectionDelay> delays={}) {
        BackendConfig config{.maxFrames=maximum,.inputChannels=2,.outputChannels=2,.connectionDelays=delays};
        auto optimized=p.graph.compile(config); ASSERT_TRUE(optimized);
        config.slotStrategy=SlotStrategy::Unique;
        auto unique=p.graph.compile(config); ASSERT_TRUE(unique);
        ReferenceRenderer oracle(p,delays);
        ASSERT_EQ(optimized->signalLifetimes().size(),unique->signalLifetimes().size());
        EXPECT_EQ(unique->bufferSlotCount(),unique->signalLifetimes().size());
        EXPECT_LE(optimized->bufferSlotCount(),unique->bufferSlotCount());
        std::cout << "peakSlots=" << optimized->bufferSlotCount() << " unique=" << unique->bufferSlotCount() << '\n';
        const auto life=optimized->signalLifetimes();
        const auto slots=optimized->signalSlots();
        for (SignalID a=0;a<life.size();++a) for (SignalID b=a+1;b<life.size();++b)
            if (life[a].producedAt<=life[b].lastUse && life[b].producedAt<=life[a].lastUse)
                ASSERT_NE(slots[a],slots[b]) << "overlap signal=" << a << ',' << b;
        std::vector<Samples> captured, uniqueCaptured;
        for (std::size_t t=0;t<p.ids.size();++t) {
            captured.push_back(guardedSamples()); uniqueCaptured.push_back(guardedSamples());
        }
        Samples inputA=guardedSamples(), inputB=guardedSamples();
        Samples output=guardedSamples(), referenceOutput=guardedSamples();
        std::vector<std::array<const float*,2>> addresses;
        for (const auto& task : optimized->executionTasks())
            addresses.push_back({task.context.outputs[0].buffer,task.context.outputs[1].buffer});

        // Changing callback sizes crosses history wrap points and empty callbacks.
        const std::array<FrameCount,12> sizes{0,1,128,4096,1,128,0,4096,128,1,4096,1};
        for (std::size_t block=0;block<sizes.size();++block) {
            SCOPED_TRACE(block);
            const auto frames=sizes[block];
            auto& input=block%2 ? inputA : inputB;
            for (unsigned c=0;c<2;++c) for (FrameCount f=0;f<maximum;++f)
                input[c][f+1]=static_cast<float>(static_cast<int>((f+block*31+c*7)%127)-63)*0.125F;
            const auto original=input;
            const std::array<const float*,2> inputs{input[0].data()+1,input[1].data()+1};
            oracle.render(inputs,frames);
            const auto execute=[&](ExecutionPlan& plan,std::vector<Samples>& trace,Samples& out) {
                for (auto& channel : out) std::fill(channel.begin(),channel.end(),sentinel);
                for (std::size_t t=0;t<p.ids.size();++t) {
                    auto* node=static_cast<pool_test::SumNode*>(p.graph.node(p.ids[t]));
                    for (unsigned c=0;c<2;++c) {
                        std::fill(trace[t][c].begin(),trace[t][c].end(),sentinel);
                        node->capture[c]=trace[t][c].data()+1;
                    }
                }
                const std::array outputs{out[0].data()+1,out[1].data()+1};
                plan.exec(AudioProcessContext{inputs,outputs,frames});
            };
            execute(*optimized,captured,output);
            execute(*unique,uniqueCaptured,referenceOutput);
            EXPECT_EQ(input,original);
            for (std::size_t task=0;task<p.ids.size();++task) for (unsigned c=0;c<2;++c) {
                EXPECT_EQ(optimized->executionTasks()[task].context.outputs[c].buffer,addresses[task][c]);
                const auto signal=task*2+c;
                for (FrameCount f=0;f<maximum+2;++f) {
                    ASSERT_EQ(captured[task][c][f],oracle.produced[task][c][f])
                        << "task=" << task << " SignalID=" << signal << " slot=" << slots[signal] << " offset=" << f;
                    ASSERT_EQ(uniqueCaptured[task][c][f],oracle.produced[task][c][f])
                        << "unique task=" << task << " SignalID=" << signal << " offset=" << f;
                }
            }
            for (unsigned c=0;c<2;++c) {
                EXPECT_EQ(output[c],oracle.physical[c]) << "physical channel=" << c;
                EXPECT_EQ(referenceOutput[c],oracle.physical[c]) << "unique physical channel=" << c;
            }
        }
        for (const auto id : p.ids) static_cast<pool_test::SumNode*>(p.graph.node(id))->capture={};
    }

    TEST(PoolRenderOracle, LongChainAndDiamondKeepEveryIntermediateSample) {
        {
            pool_test::Pipeline p;
            auto last=p.add();
            for (unsigned n=0;n<24;++n) { const auto next=p.add(); p.edge(last,next); last=next; }
            ASSERT_TRUE(p.graph.markAsOutput(last));
            checkCorpus(p);
        }
        {
            pool_test::Pipeline p;
            const auto s=p.add(), a=p.add(), b=p.add(), join=p.add(), after=p.add();
            p.edge(s,a); p.edge(s,b,PortType::Aux);
            p.edge(a,join); p.edge(b,join,PortType::In,1);
            p.edge(join,after); p.edge(s,after,PortType::Aux,1);
            ASSERT_TRUE(p.graph.markAsOutput(join)); ASSERT_TRUE(p.graph.markAsOutput(after));
            checkCorpus(p);
        }
    }

    TEST(PoolRenderOracle, TenWayFanoutLateReadersAuxAndMultipleBackendWriters) {
        pool_test::Pipeline p;
        const auto source=p.add();
        std::vector<NodeID> branches;
        for (unsigned n=0;n<10;++n) {
            const auto branch=p.add(); branches.push_back(branch);
            p.edge(source,branch,n%2 ? PortType::Aux : PortType::In);
            ASSERT_TRUE(p.graph.markAsOutput(branch));
        }
        const auto last=p.add();
        p.edge(branches.front(),last); p.edge(branches.back(),last,PortType::In,1);
        p.edge(source,last,PortType::Aux,1);
        ASSERT_TRUE(p.graph.markAsOutput(last));
        checkCorpus(p);
    }

    TEST(PoolRenderOracle, DelaysZeroThreeAnd256HaveIndependentPersistentHistories) {
        for (const std::uint64_t delay : {0U,3U,256U}) {
            SCOPED_TRACE(delay);
            pool_test::Pipeline p;
            const auto a=p.add(), b=p.add(), first=p.add(), late=p.add(), output=p.add();
            const auto e1=p.edge(a,first);
            p.edge(a,late,PortType::Aux); p.edge(b,late);
            const auto e2=p.edge(first,output); p.edge(late,output,PortType::Aux);
            const std::array delays{ConnectionDelay{e1,delay},ConnectionDelay{e2,delay}};
            ASSERT_TRUE(p.graph.markAsOutput(first)); ASSERT_TRUE(p.graph.markAsOutput(output));
            checkCorpus(p,delays);
        }
    }

    TEST(PoolRenderOracle, UndeclaredEdgesSilenceMultipleSourcesAndMissingPortsRemainSafe) {
        pool_test::Pipeline p;
        const auto a=p.add(), b=p.add(), silence=p.add(), output=p.add();
        p.edge(a,silence,PortType::Aux,65535,65535);
        p.edge(b,output); p.edge(silence,output,PortType::Aux);
        ASSERT_TRUE(p.graph.markAsOutput(output));
        checkCorpus(p);
    }

    TEST(PoolRenderOracle, DeterministicGeneratedGraphsCoverDifferentLivenessAndDelayShapes) {
        std::uint32_t state=0x51A7E5U;
        const auto next=[&]() { state=state*1664525U+1013904223U; return state; };
        for (unsigned scenario=0;scenario<8;++scenario) {
            SCOPED_TRACE(scenario);
            pool_test::Pipeline p;
            std::vector<ConnectionDelay> delays;
            for (unsigned n=0;n<9;++n) {
                const auto node=p.add();
                if (n!=0) for (unsigned port=0;port<2;++port) for (ChannelIndex lane=0;lane<2;++lane) {
                    if (next()%4==0) continue;
                    const auto from=p.ids[next()%n];
                    const auto edge=p.edge(from,node,port ? PortType::Aux : PortType::In,lane,next()%2);
                    if (delays.size()<3 && next()%3==0) delays.push_back({edge,next()%4});
                }
                if (n==8 || next()%3==0) ASSERT_TRUE(p.graph.markAsOutput(node));
            }
            checkCorpus(p,delays);
        }
    }
}
