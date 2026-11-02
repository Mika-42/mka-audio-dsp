#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>
#include <gtest/gtest.h>
import mka.audio.dsp.dag;
#include "pdc_render_fixture.hpp"

namespace {
    using namespace mka::audio::dsp;
    using namespace pdc_test;
    class FreeRouter final : public Node {
    public:
        FreeRouter(NodeID id,std::uint32_t lanes,float bias=0.F,FrameCount begin=0,FrameCount end=UINT32_MAX)
            : Node(id,ports()),lanes_(lanes),bias_(bias),begin_(begin),end_(end) {}
        void process(const ProcessContext& ctx) noexcept override {
            for (FrameCount f=0;f<ctx.frames;++f) {
                float logical=0.F;
                for (const auto& input : ctx.inputs) logical+=input.buffer[f];
                for (const auto& input : ctx.auxiliary) logical+=input.buffer[f];
                for (const auto& output : ctx.outputs) output.buffer[f]=logical;
                if (f<begin_ || f>=end_) continue;
                for (std::size_t c=0;c<ctx.backendOutputs.size() && c<32;++c)
                    if (lanes_&(1U<<c)) ctx.backendOutputs[c].buffer[f]=logical+bias_;
            }
        }
    private:
        const std::uint32_t lanes_;
        const float bias_;
        const FrameCount begin_,end_;
    };

    TEST(PdcOutputs, CommonLogicalJunctionAlignsThreeAndEightBeforeFreeRoutingToLanesTwoAndFive) {
        DAG graph;
        const auto a=graph.add<UniformDelayNode>(3U),b=graph.add<UniformDelayNode>(8U);
        const auto router=graph.add<FreeRouter>((1U<<2)|(1U<<5));
        ASSERT_TRUE(a); ASSERT_TRUE(b); ASSERT_TRUE(router);
        ASSERT_TRUE(connect(graph,edge(*a,*router))); ASSERT_TRUE(connect(graph,edge(*b,*router,PortType::Aux)));
        ASSERT_TRUE(graph.markAsOutput(*router));
        auto plan=graph.compile(BackendConfig{.maxFrames=4,.inputChannels=1,.outputChannels=6}); ASSERT_TRUE(plan);
        EXPECT_EQ(plan->delayTaskCount(),1U);
        std::array<float,4> input{1,0,0,0}; std::array<std::array<float,4>,6> storage{};
        std::array<float*,6> outputs{}; for (unsigned c=0;c<6;++c) outputs[c]=storage[c].data();
        const std::array<const float*,1> inputs{input.data()};
        for (unsigned block=0;block<4;++block) {
            ASSERT_TRUE(plan->exec(BackendBlock{inputs,outputs,4}));
            for (unsigned c=0;c<6;++c) for (unsigned f=0;f<4;++f) {
                const float expected=(c==2 || c==5) && block*4+f==8 ? 2.F : 0.F;
                EXPECT_EQ(storage[c][f],expected)<<"physical lane="<<c<<" t="<<block*4+f;
            }
            input.fill(0);
        }
    }

    TEST(PdcOutputs, IndependentMarkedNodesKeepTheirOwnPhysicalTimingsWithoutInventedAlignment) {
        DAG graph;
        const auto a=graph.add<UniformDelayNode>(3U,ports(),5U),b=graph.add<UniformDelayNode>(8U,ports(),2U);
        ASSERT_TRUE(a); ASSERT_TRUE(b); ASSERT_TRUE(graph.markAsOutput(*a)); ASSERT_TRUE(graph.markAsOutput(*b));
        auto plan=graph.compile(BackendConfig{.maxFrames=4,.inputChannels=1,.outputChannels=6}); ASSERT_TRUE(plan);
        EXPECT_EQ(plan->delayTaskCount(),0U); EXPECT_TRUE(plan->pdcAnalysis()->edges.empty());
        EXPECT_EQ(plan->pdcAnalysis()->find(*a)->cumulative,3U);
        EXPECT_EQ(plan->pdcAnalysis()->find(*b)->cumulative,8U);
        std::array<float,4> input{1,0,0,0}; std::array<std::array<float,4>,6> storage{};
        std::array<float*,6> outputs{}; for (unsigned c=0;c<6;++c) outputs[c]=storage[c].data();
        const std::array<const float*,1> inputs{input.data()};
        for (unsigned block=0;block<4;++block) {
            ASSERT_TRUE(plan->exec(BackendBlock{inputs,outputs,4}));
            for (unsigned c=0;c<6;++c) for (unsigned f=0;f<4;++f) {
                const auto t=block*4+f;
                const float expected=(c==5 && t==3) || (c==2 && t==8) ? 1.F : 0.F;
                EXPECT_EQ(storage[c][f],expected)<<"physical lane="<<c<<" t="<<t;
            }
            input.fill(0);
        }
    }

    TEST(PdcOutputs, MarkedRouterWithSuccessorKeepsLogicalSamplesSeparateFromArbitraryHardwareWrites) {
        DAG graph;
        const auto a=graph.add<UniformDelayNode>(3U),b=graph.add<UniformDelayNode>(8U);
        const auto router=graph.add<FreeRouter>(1U,100.F),sink=graph.add<UniformDelayNode>(0U,ports(),1U);
        ASSERT_TRUE(a); ASSERT_TRUE(b); ASSERT_TRUE(router); ASSERT_TRUE(sink);
        for (const auto connection : {edge(*a,*router),edge(*b,*router,PortType::Aux),edge(*router,*sink)})
            ASSERT_TRUE(connect(graph,connection));
        ASSERT_TRUE(graph.markAsOutput(*router)); ASSERT_TRUE(graph.markAsOutput(*sink));
        auto plan=graph.compile(BackendConfig{.maxFrames=4,.inputChannels=1,.outputChannels=2}); ASSERT_TRUE(plan);
        EXPECT_EQ(plan->delayTaskCount(),1U);
        EXPECT_EQ(context(*plan,*router).outputs[0].buffer,context(*plan,*sink).inputs[0].buffer);
        std::array<float,4> input{1,0,0,0},lane0{},lane1{};
        const std::array<const float*,1> inputs{input.data()}; const std::array outputs{lane0.data(),lane1.data()};
        EXPECT_NE(context(*plan,*router).outputs[0].buffer,lane0.data());
        for (unsigned block=0;block<4;++block) {
            ASSERT_TRUE(plan->exec(BackendBlock{inputs,outputs,4}));
            for (unsigned f=0;f<4;++f) {
                const float logical=block*4+f==8 ? 2.F : 0.F;
                EXPECT_EQ(lane0[f],100.F+logical); EXPECT_EQ(lane1[f],logical);
            }
            input.fill(0);
        }
    }

    TEST(PdcOutputs, MultipleWritersKeepPerSampleLastWriterWinsAfterLogicalCompensation) {
        DAG graph;
        const auto a=graph.add<UniformDelayNode>(3U),b=graph.add<UniformDelayNode>(8U);
        const auto first=graph.add<FreeRouter>(1U,10.F),last=graph.add<FreeRouter>(1U,20.F,1U,3U);
        ASSERT_TRUE(a); ASSERT_TRUE(b); ASSERT_TRUE(first); ASSERT_TRUE(last);
        for (const auto connection : {edge(*a,*first),edge(*b,*first,PortType::Aux),edge(*first,*last)})
            ASSERT_TRUE(connect(graph,connection));
        ASSERT_TRUE(graph.markAsOutput(*first)); ASSERT_TRUE(graph.markAsOutput(*last));
        auto plan=graph.compile(BackendConfig{.maxFrames=4,.inputChannels=1,.outputChannels=1}); ASSERT_TRUE(plan);
        EXPECT_EQ(plan->delayTaskCount(),1U);
        EXPECT_EQ(plan->pdcAnalysis()->find(edge(*first,*last))->samples,0U);
        std::array<float,4> input{1,0,0,0},output{};
        const std::array<const float*,1> inputs{input.data()}; const std::array outputs{output.data()};
        for (unsigned block=0;block<4;++block) {
            ASSERT_TRUE(plan->exec(BackendBlock{inputs,outputs,4}));
            for (unsigned f=0;f<4;++f) {
                const float logical=block*4+f==8 ? 2.F : 0.F;
                EXPECT_EQ(output[f],logical+(f==1 || f==2 ? 20.F : 10.F));
            }
            input.fill(0);
        }
    }
}
