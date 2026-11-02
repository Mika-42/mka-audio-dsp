#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>
#include <gtest/gtest.h>
import mka.audio.dsp.dag;
#include "delay_audio_fixture.hpp"
namespace {
    using namespace mka::audio::dsp;
    class WrongDeclaration final : public Node {
    public:
        explicit WrongDeclaration(NodeID id):Node(id,Config{.layoutInput={0},.layoutOutput={0}}) {}
        void process(const ProcessContext& ctx) noexcept override {
            for(FrameCount f=0;f<ctx.frames;++f) {
                ctx.outputs[0].buffer[f]=history_[position_]; history_[position_]=ctx.inputs[0].buffer[f];
                if(++position_==64) position_=0;
            }
        }
    private: std::array<float,64> history_{}; std::size_t position_=0;
    };
    class Oscillator final : public Node {
    public:
        explicit Oscillator(NodeID id):Node(id,Config{.layoutOutput={0}}) {}
        void process(const ProcessContext& ctx) noexcept override {
            offered=ctx.inputs.size();
            for(FrameCount f=0;f<ctx.frames;++f) { ctx.outputs[0].buffer[f]=0.25F; ctx.backendOutputs[0].buffer[f]=0.25F; }
        }
        std::size_t offered=0;
    };
    TEST(DeclaredLatencyContract, IncorrectPluginDeclarationIsNotAutomaticallyMeasuredOrRepaired) {
        DAG g; auto a=g.add<audio_pdc_test::MonoPlugin>(0U,0U),b=g.add<WrongDeclaration>();
        auto mix=g.add<audio_pdc_test::MonoPlugin>(0U,audio_pdc_test::noLane,0U); ASSERT_TRUE(a);ASSERT_TRUE(b);ASSERT_TRUE(mix);
        for(auto edge : {audio_pdc_test::edge(*a,*b),audio_pdc_test::edge(*a,*mix),audio_pdc_test::edge(*b,*mix,PortType::Aux)})
            ASSERT_TRUE(g.connect(edge.to,edge.from));
        ASSERT_TRUE(g.markAsOutput(*mix));auto plan=g.compile(BackendConfig{.maxFrames=128,.inputChannels=1,.outputChannels=1});ASSERT_TRUE(plan);
        EXPECT_EQ(plan->delayTaskCount(),0U); EXPECT_EQ(plan->pdcAnalysis()->find(*mix)->cumulative,0U);
        std::array<float,128> in{},out{};in[0]=1;const std::array<const float*,1> inputs{in.data()};const std::array outputs{out.data()};
        ASSERT_TRUE(plan->exec(BackendBlock{inputs,outputs,128}));
        for(unsigned n=0;n<128;++n) EXPECT_EQ(out[n],n==0||n==64 ? 1.F:0.F)<<"n="<<n;
    }
    TEST(BackendOscillator, ASourceMayIgnoreAllSixPhysicalInputs) {
        DAG g;auto id=g.add<Oscillator>();ASSERT_TRUE(id);ASSERT_TRUE(g.markAsOutput(*id));
        auto plan=g.compile(BackendConfig{.maxFrames=4,.inputChannels=6,.outputChannels=1});ASSERT_TRUE(plan);
        std::array<float,4> in{-999,-999,-999,-999},out{};std::array<const float*,6> inputs{};inputs.fill(in.data());const std::array outputs{out.data()};
        ASSERT_TRUE(plan->exec(BackendBlock{inputs,outputs,4}));
        EXPECT_EQ(static_cast<Oscillator*>(g.node(*id))->offered,6U);EXPECT_EQ(out,(std::array<float,4>{.25F,.25F,.25F,.25F}));
        EXPECT_EQ(in,(std::array<float,4>{-999,-999,-999,-999}));
    }
}
