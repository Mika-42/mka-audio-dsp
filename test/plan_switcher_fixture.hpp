#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <expected>
#include <span>

// Standard headers must be included BEFORE importing the modules in each .cpp.
namespace switcher_test {
    using namespace mka::audio::dsp;
    struct LifetimeTrace {
        std::atomic<unsigned> destroyed{0};
        std::atomic<unsigned> destroyedOnAudio{0};
    };
    inline thread_local bool onAudio=false;

    class SignalNode final : public Node {
    public:
        explicit SignalNode(NodeID id,LifetimeTrace* trace=nullptr)
            : Node(id,Config{.layoutOutput={0}}),trace_(trace) {}
        void process(const ProcessContext& ctx) noexcept override {
            for (FrameCount f=0;f<ctx.frames;++f) {
                const auto sample=ctx.inputs.empty() ? 1.F : ctx.inputs[0].buffer[f];
                ctx.outputs[0].buffer[f]=sample;
                for (const auto& out : ctx.backendOutputs) out.buffer[f]=sample;
            }
        }
        ~SignalNode() override {
            if (trace_) { ++trace_->destroyed; if (onAudio) ++trace_->destroyedOnAudio; }
        }
    private:
        LifetimeTrace* trace_;
    };
    class SinkNode final : public Node {
    public:
        explicit SinkNode(NodeID id) : Node(id,Config{.layoutInput={0}}) {}
        void process(const ProcessContext& ctx) noexcept override {
            for (FrameCount f=0;f<ctx.frames;++f)
                for (const auto& out : ctx.backendOutputs) out.buffer[f]=ctx.inputs[0].buffer[f];
        }
    };
    class GenerationNode final : public Node {
    public:
        GenerationNode(NodeID id,PlanSwitcher& switcher) : Node(id,Config{.layoutOutput={0}}),switcher_(switcher) {}
        void process(const ProcessContext& ctx) noexcept override {
            const auto generation=switcher_.audioGeneration();
            noMidBlockSwap=noMidBlockSwap && !switcher_.trySwapAtBlockBoundary();
            for (FrameCount f=0;f<ctx.frames;++f) {
                ctx.outputs[0].buffer[f]=static_cast<float>(generation);
                for (const auto& output : ctx.backendOutputs) output.buffer[f]=static_cast<float>(generation);
            }
            coherent=coherent && generation==switcher_.audioGeneration();
        }
        bool noMidBlockSwap=true,coherent=true;
    private:
        PlanSwitcher& switcher_;
    };
    inline Result makeDelayedGraph(DAG& graph,std::array<NodeID,2>& ids,Connection& edge,LifetimeTrace* trace=nullptr) {
        const auto a=graph.add<SignalNode>(trace),b=graph.add<SinkNode>();
        if (!a) return std::unexpected(a.error());
        if (!b) return std::unexpected(b.error());
        ids={*a,*b}; edge={encode(*a,PortType::Out,0),encode(*b,PortType::In,0)};
        if (auto result=graph.connect(edge.to,edge.from); !result) return result;
        return graph.markAsOutput(*b);
    }
    struct IO {
        std::array<float,4> input{1,2,3,4},output{};
        std::array<const float*,1> inputs{input.data()};
        std::array<float*,1> outputs{output.data()};
        mka::audio::AudioProcessContext block(FrameCount frames=4) noexcept { return {inputs,outputs,frames,4,4}; }
        IO()=default;
        IO(const IO&)=delete;
        IO& operator=(const IO&)=delete;
    };
}
