/**
 * @file test_declared_latency.cpp
 * @brief Tests for declared latency.
 * @author Mika
 */
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
    /// @brief Delays its output while declaring zero latency to demonstrate the plugin contract.
    class WrongDeclaration final : public Node {
    public:
        /// @brief Declare the processor logical ports under the supplied stable Node identity.
        explicit WrongDeclaration(NodeID id):Node(id,Config{.layoutInput={0},.layoutOutput={0}}) {}
        /// @brief Apply an actual 64-sample delay while the declaration intentionally remains zero.
        void process(const ProcessContext& ctx) noexcept override {
            for(FrameCount f=0;f<ctx.frames;++f) {
                ctx.outputs[0].buffer[f]=history_[position_]; history_[position_]=ctx.inputs[0].buffer[f];
                if(++position_==64) position_=0;
            }
        }
    private: /** Owned persistent delay samples, initialized to zero during preparation. */ std::array<float,64> history_{}; /** Cursor into persistent DSP history; empty blocks do not advance it. */ std::size_t position_=0;
    };
    /// @brief Produces a fixed signal independently of physical input samples.
    class Oscillator final : public Node {
    public:
        /// @brief Declare the mono signal produced independently of physical inputs.
        explicit Oscillator(NodeID id):Node(id,Config{.layoutOutput={0}}) {}
        /// @brief Produce a constant signal while recording how many physical inputs were offered.
        void process(const ProcessContext& ctx) noexcept override {
            offered=ctx.inputs.size();
            for(FrameCount f=0;f<ctx.frames;++f) { ctx.outputs[0].buffer[f]=0.25F; ctx.backendOutputs[0].buffer[f]=0.25F; }
        }
        /// @brief Number of physical inputs offered to a source that may ignore them.
        std::size_t offered=0;
    };
    /// @brief Test case: incorrect plugin declaration is not automatically measured or repaired.
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
    /// @brief Test case: A source may ignore all six physical inputs.
    TEST(BackendOscillator, ASourceMayIgnoreAllSixPhysicalInputs) {
        DAG g;auto id=g.add<Oscillator>();ASSERT_TRUE(id);ASSERT_TRUE(g.markAsOutput(*id));
        auto plan=g.compile(BackendConfig{.maxFrames=4,.inputChannels=6,.outputChannels=1});ASSERT_TRUE(plan);
        std::array<float,4> in{-999,-999,-999,-999},out{};std::array<const float*,6> inputs{};inputs.fill(in.data());const std::array outputs{out.data()};
        ASSERT_TRUE(plan->exec(BackendBlock{inputs,outputs,4}));
        EXPECT_EQ(static_cast<Oscillator*>(g.node(*id))->offered,6U);EXPECT_EQ(out,(std::array<float,4>{.25F,.25F,.25F,.25F}));
        EXPECT_EQ(in,(std::array<float,4>{-999,-999,-999,-999}));
    }
}
