/**
 * @file pdc_render_fixture.hpp
 * @brief Test fixtures for pdc render fixture.
 * @author Mika
 */
#pragma once
// Include standard headers before importing dag/plan_switcher, then this file.
namespace pdc_test {
    using namespace mka::audio::dsp;
    /// @brief Return the mono port layout used by this fixture.
    inline Config ports() { return {.layoutInput={0},.layoutAuxiliary={0},.layoutOutput={0}}; }
    /// @brief Build the complete logical edge identity used by the fixture.
    inline Connection edge(NodeID from,NodeID to,PortType port=PortType::In,ChannelIndex index=0) {
        return {encode(from,PortType::Out,0),encode(to,port,index)};
    }
    /// @brief Apply a logical-edge insertion to this fixture or its independent model.
    inline Result connect(DAG& graph,Connection value) { return graph.connect(value.to,value.from); }
    /// @brief Borrow a known prepared task's context; the view ends when the plan is retired.
    inline const ProcessContext& context(const ExecutionPlan& plan,NodeID id) {
        return plan.executionTasks()[*plan.taskIndexOf(id)].context;
    }

    // An actual scalar DSP delay, with independent Node-owned history prepared
    // before any plan borrow. Logs are fixed storage, never grown in process.
    /// @brief Declares a uniform latency for every logical path in the processor.
    class UniformDelayNode final : public Node {
    public:
        /// @brief Declare the processor logical ports under the supplied stable Node identity.
        UniformDelayNode(NodeID id,LatencySamples delay,Config cfg=ports(),
                         std::uint32_t lane=UINT32_MAX,float gain=1.F)
            : Node(id,cfg),delay_(delay),history_(delay,0.F),lane_(lane),gain_(gain) {}
        /// @brief Return the fixed algorithmic delay declared by this fixture processor.
        LatencySamples latencySamples() const noexcept override { return delay_; }
        /// @brief Render the fixture waveform with its configured persistent intrinsic delay.
        void process(const ProcessContext& ctx) noexcept override {
            for (FrameCount f=0;f<ctx.frames;++f) {
                float sample=0.F;
                for (const auto& input : ctx.inputs) sample+=input.buffer[f];
                for (const auto& input : ctx.auxiliary) sample+=input.buffer[f];
                sample*=gain_;
                if (delay_) {
                    const auto delayed=history_[position_]; history_[position_]=sample;
                    if (++position_==history_.size()) position_=0;
                    sample=delayed;
                }
                for (const auto& output : ctx.outputs) output.buffer[f]=sample;
                if (lane_<ctx.backendOutputs.size()) ctx.backendOutputs[lane_].buffer[f]=sample;
                if (seen<mainLog.size()) {
                    mainLog[seen]=ctx.inputs.empty() ? 0.F : ctx.inputs[0].buffer[f];
                    auxLog[seen]=ctx.auxiliary.empty() ? 0.F : ctx.auxiliary[0].buffer[f];
                    producedLog[seen]=sample;
                }
                ++seen;
            }
        }
        std::array<float,4096> mainLog{} /**< Main log retained by the fixture. */,
            auxLog{} /**< Borrowed destination for captured auxiliary-input samples. */,
            producedLog{} /**< Borrowed destination for captured logical production samples. */;
        /// @brief Sample-stream position or externally owned observations used by this fixture.
        std::size_t seen=0;
    private:
        /// @brief Immutable whole-sample latency used by the processor's delay state.
        const LatencySamples delay_;
        /// @brief Owned persistent delay samples, initialized to zero during preparation.
        std::vector<float> history_;
        /// @brief Cursor into persistent DSP history; empty blocks do not advance it.
        std::size_t position_=0;
        /// @brief Logical or physical lane selected by this processor.
        const std::uint32_t lane_;
        /// @brief Fixed multiplication factor applied by the processor.
        const float gain_;
    };
    /// @brief Look up the independent model's metadata for the requested node identity.
    inline UniformDelayNode& node(DAG& graph,NodeID id) {
        return *static_cast<UniformDelayNode*>(graph.node(id));
    }
}
