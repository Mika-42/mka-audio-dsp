/**
 * @file test_physical_input_views.cpp
 * @brief Tests for physical input views.
 * @author Mika
 */
#include <array>
#include <concepts>
#include <span>
#include <vector>
#include <gtest/gtest.h>
import mka.audio.dsp.dag;
#include "backend_routing_fixture.hpp"

namespace {
    using namespace mka::audio::dsp;
    using namespace backend_test;
    static_assert(std::same_as<decltype(InputChannel::buffer),const float*>);
    static_assert(std::same_as<decltype(ProcessContext::inputs),std::span<const InputChannel>>);

    /// @brief Records borrowed physical inputs and preserves their read-only samples.
    class PhysicalReader final : public Node {
    public:
        /// @brief Declare the processor logical ports under the supplied stable Node identity.
        PhysicalReader(NodeID id,Config cfg) : Node(id,cfg) {}
        /// @brief Inspect direct read-only physical inputs without changing their sample storage.
        void process(const ProcessContext& ctx) noexcept override {
            ++calls; count=ctx.inputs.size();
            if (currentIO) {
                direct=direct && count==currentIO->inputs.size();
                for (std::size_t c=0;c<count;++c)
                    direct=direct && ctx.inputs[c].buffer==currentIO->inputs[c] && ctx.inputs[c].channel==0;
            }
            for (FrameCount f=0;f<ctx.frames;++f) {
                const float sample=ctx.inputs.empty() ? 0.F : ctx.inputs.front().buffer[f]+2.F*ctx.inputs.back().buffer[f];
                ctx.outputs[0].buffer[f]=sample;
                for (const auto& output : ctx.backendOutputs) output.buffer[f]=sample;
            }
        }
        /// @brief Number of processor invocations observed by the fixture.
        unsigned calls=0;
        /// @brief Requested number of retained array elements.
        std::size_t count=0;
        /// @brief Whether physical input pointers were forwarded without copying or remapping.
        bool direct=true;
    };

    /// @brief Test case: multiple sources borrow every physical channel regardless of their input layouts.
    TEST(PhysicalInputViews, MultipleSourcesBorrowEveryPhysicalChannelRegardlessOfTheirInputLayouts) {
        for (const std::uint32_t count : {0U,1U,2U,6U}) {
            SCOPED_TRACE(count);
            DAG graph;
            const auto a=graph.add<PhysicalReader>(Config{.layoutOutput={9}});
            const auto b=graph.add<PhysicalReader>(Config{.layoutInput={0,65535},.layoutOutput={9}});
            ASSERT_TRUE(a); ASSERT_TRUE(b); ASSERT_TRUE(graph.markAsOutput(*b));
            auto plan=graph.compile(BackendConfig{.maxFrames=128,.inputChannels=count,.outputChannels=2}); ASSERT_TRUE(plan);
            EXPECT_TRUE(graph.connections().empty()); EXPECT_EQ(plan->sourceTasks().size(),2U);
            auto* first=plan->executionTasks()[0].context.outputs[0].buffer;
            auto* second=plan->executionTasks()[1].context.outputs[0].buffer;
            FakeBackend left(1.F),right(1000.F);
            const auto originalLeft=left.inputStorage,originalRight=right.inputStorage;
            for (FrameCount frames : {0U,1U,128U}) for (auto* backend : {&left,&right}) {
                backend->reset();
                const auto io=backend->io(frames,count,2); IOScope expected(io); plan->exec(io);
                EXPECT_EQ(static_cast<PhysicalReader*>(graph.node(*a))->count,count);
                EXPECT_EQ(static_cast<PhysicalReader*>(graph.node(*b))->count,count);
                for (FrameCount f=0;f<frames;++f) {
                    const auto sample=count==0 ? 0.F : backend->inputs[0][f]+2.F*backend->inputs[count-1][f];
                    EXPECT_EQ(first[f],sample); EXPECT_EQ(second[f],sample);
                    EXPECT_EQ(backend->outputs[0][f],sample); EXPECT_EQ(backend->outputs[1][f],sample);
                }
                for (const auto& task : plan->executionTasks()) {
                    EXPECT_TRUE(task.context.inputs.empty()); EXPECT_TRUE(task.context.backendOutputs.empty());
                }
                EXPECT_EQ(plan->executionTasks()[0].context.outputs[0].buffer,first);
                EXPECT_EQ(plan->executionTasks()[1].context.outputs[0].buffer,second);
            }
            EXPECT_TRUE(static_cast<PhysicalReader*>(graph.node(*a))->direct);
            EXPECT_TRUE(static_cast<PhysicalReader*>(graph.node(*b))->direct);
            EXPECT_EQ(left.inputStorage,originalLeft); EXPECT_EQ(right.inputStorage,originalRight);
        }
    }

    /// @brief Test case: exact zero frame shape accepts null pointers and does not retain callback views.
    TEST(PhysicalInputViews, ExactZeroFrameShapeAcceptsNullPointersAndDoesNotRetainCallbackViews) {
        DAG graph;
        const auto a=graph.add<PhysicalReader>(Config{.layoutOutput={9}}); ASSERT_TRUE(a);
        auto plan=graph.compile(BackendConfig{.inputChannels=6}); ASSERT_TRUE(plan);
        const std::array<const float*,6> inputs{};
        const mka::audio::AudioProcessContext io{.inputs=inputs};
        const IOScope expected(io); plan->exec(io);
        EXPECT_EQ(static_cast<PhysicalReader*>(graph.node(*a))->calls,1U);
        EXPECT_EQ(static_cast<PhysicalReader*>(graph.node(*a))->count,6U);
        EXPECT_TRUE(plan->executionTasks()[0].context.inputs.empty());
        EXPECT_EQ(plan->callbackDiagnostics(),0U);
    }
}
