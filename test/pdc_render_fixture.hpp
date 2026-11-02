#pragma once
// Include standard headers before importing dag/plan_switcher, then this file.
namespace pdc_test {
    using namespace mka::audio::dsp;
    inline Config ports() { return {.layoutInput={0},.layoutAuxiliary={0},.layoutOutput={0}}; }
    inline Connection edge(NodeID from,NodeID to,PortType port=PortType::In,ChannelIndex index=0) {
        return {encode(from,PortType::Out,0),encode(to,port,index)};
    }
    inline Result connect(DAG& graph,Connection value) { return graph.connect(value.to,value.from); }
    inline const ProcessContext& context(const ExecutionPlan& plan,NodeID id) {
        return plan.executionTasks()[*plan.taskIndexOf(id)].context;
    }

    // An actual scalar DSP delay, with independent Node-owned history prepared
    // before any plan borrow. Logs are fixed storage, never grown in process.
    class UniformDelayNode final : public Node {
    public:
        UniformDelayNode(NodeID id,LatencySamples delay,Config cfg=ports(),
                         std::uint32_t lane=UINT32_MAX,float gain=1.F)
            : Node(id,cfg),delay_(delay),history_(delay,0.F),lane_(lane),gain_(gain) {}
        LatencySamples latencySamples() const noexcept override { return delay_; }
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
        std::array<float,4096> mainLog{},auxLog{},producedLog{};
        std::size_t seen=0;
    private:
        const LatencySamples delay_;
        std::vector<float> history_;
        std::size_t position_=0;
        const std::uint32_t lane_;
        const float gain_;
    };
    inline UniformDelayNode& node(DAG& graph,NodeID id) {
        return *static_cast<UniformDelayNode*>(graph.node(id));
    }
}
