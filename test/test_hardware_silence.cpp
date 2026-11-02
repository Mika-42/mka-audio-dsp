#include <array>
#include <gtest/gtest.h>
import mka.audio.dsp.dag;
#include "backend_routing_fixture.hpp"

namespace {
    using namespace mka::audio::dsp;
    using namespace backend_test;

    class ZeroObserver final : public Node {
    public:
        explicit ZeroObserver(NodeID id) : Node(id,Config{}) {}
        void process(const ProcessContext& ctx) noexcept override {
            ++calls;
            for (const auto& output : ctx.backendOutputs) for (FrameCount f=0;f<ctx.frames;++f)
                allZero=allZero && output.buffer[f]==0.F;
        }
        unsigned calls=0;
        bool allZero=true;
    };

    TEST(HardwareSilence, EmptyGraphZerosEveryAnnouncedChannelAndOnlyTheCurrentFrameRange) {
        for (std::uint32_t channels : {0U,1U,2U,6U}) {
            DAG graph;
            auto plan=graph.compile(BackendConfig{.maxFrames=128,.outputChannels=channels}); ASSERT_TRUE(plan);
            FakeBackend first,second;
            for (FrameCount frames : {128U,1U,0U,17U,128U}) for (auto* backend : {&first,&second}) {
                const auto before=backend->outputStorage;
                plan->exec(backend->io(frames,0,channels));
                for (std::size_t c=0;c<6;++c) for (std::size_t f=0;f<130;++f)
                    EXPECT_EQ(backend->outputStorage[c][f],c<channels && f>0 && f<=frames ? 0.F : before[c][f]);
            }
            EXPECT_EQ(plan->callbackDiagnostics(),0U);
        }
    }

    TEST(HardwareSilence, AllOutputsAreZeroBeforeFirstProcessAndPartialWritersLeaveSilentHoles) {
        Observation seen; seen.expectOutput=true;
        WritePolicy policy{.lanes=(1U<<0)|(1U<<3),.begin=2,.end=7,.stride=2,.physicalValue=40.F};
        DAG graph;
        const auto first=graph.add<ZeroObserver>();
        const auto writer=graph.add<PhysicalWriter>(seen,policy);
        ASSERT_TRUE(first); ASSERT_TRUE(writer);
        ASSERT_TRUE(graph.markAsOutput(*first)); ASSERT_TRUE(graph.markAsOutput(*writer));
        auto plan=graph.compile(BackendConfig{.maxFrames=128,.outputChannels=6}); ASSERT_TRUE(plan);
        FakeBackend a,b;
        for (FrameCount frames : {128U,0U,1U,8U,128U}) for (auto* backend : {&a,&b}) {
            const auto before=backend->outputStorage;
            const auto io=backend->io(frames,0,6); IOScope scope(io); plan->exec(io);
            for (std::size_t c=0;c<6;++c) for (std::size_t index=0;index<130;++index) {
                float expected=before[c][index];
                if (index>0 && index<=frames) {
                    const auto f=index-1;
                    const bool written=(c==0 || c==3) && (f==2 || f==4 || f==6);
                    expected=written ? 40.F+static_cast<float>(c) : 0.F;
                }
                EXPECT_EQ(backend->outputStorage[c][index],expected) << "channel=" << c << " index=" << index;
            }
        }
        EXPECT_TRUE(static_cast<ZeroObserver*>(graph.node(*first))->allZero);
        EXPECT_EQ(static_cast<ZeroObserver*>(graph.node(*first))->calls,10U);
        EXPECT_TRUE(seen.separateStorage); EXPECT_TRUE(seen.directOutputs);
    }
}
