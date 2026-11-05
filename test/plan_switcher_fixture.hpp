/**
 * @file plan_switcher_fixture.hpp
 * @brief Test fixtures for plan switcher fixture.
 * @author Mika
 */
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <expected>
#include <span>

// Standard headers must be included BEFORE importing the modules in each .cpp.
namespace switcher_test {
    using namespace mka::audio::dsp;
    /// @brief Counts construction and destruction, including destruction inside audio processing.
    struct LifetimeTrace {
        /// @brief Number of processor instances destroyed on control or during the measured scope.
        std::atomic<unsigned> destroyed{0};
        /// @brief Number of destructor events recorded inside an audio callback.
        std::atomic<unsigned> destroyedOnAudio{0};
    };
    /// @brief Thread-local marker identifying destructor events inside audio processing.
    inline thread_local bool onAudio=false;

    /// @brief Produces or forwards a scalar signal while recording lifetime and binding observations.
    class SignalNode final : public Node {
    public:
        /// @brief Declare the mono output and optionally borrow the destruction trace.
        explicit SignalNode(NodeID id,LifetimeTrace* trace=nullptr)
            : Node(id,Config{.layoutOutput={0}}),trace_(trace) {}
        /// @brief Forward or produce fixture samples and record the borrowed callback views.
        void process(const ProcessContext& ctx) noexcept override {
            for (FrameCount f=0;f<ctx.frames;++f) {
                const auto sample=ctx.inputs.empty() ? 1.F : ctx.inputs[0].buffer[f];
                ctx.outputs[0].buffer[f]=sample;
                for (const auto& out : ctx.backendOutputs) out.buffer[f]=sample;
            }
        }
        /// @brief Record fixture destruction and release owned state on the calling thread.
        ~SignalNode() override {
            if (trace_) { ++trace_->destroyed; if (onAudio) ++trace_->destroyedOnAudio; }
        }
    private:
        /// @brief Borrowed lifetime or binding trace; it must outlive the fixture processor.
        LifetimeTrace* trace_;
    };
    /// @brief Copies an internal signal into its selected physical output.
    class SinkNode final : public Node {
    public:
        /// @brief Declare the logical input routed into physical output zero.
        explicit SinkNode(NodeID id) : Node(id,Config{.layoutInput={0}}) {}
        /// @brief Copy the internal mono input into physical output zero.
        void process(const ProcessContext& ctx) noexcept override {
            for (FrameCount f=0;f<ctx.frames;++f)
                for (const auto& out : ctx.backendOutputs) out.buffer[f]=ctx.inputs[0].buffer[f];
        }
    };
    /// @brief Records the audio generation observed from inside a processor callback.
    class GenerationNode final : public Node {
    public:
        /// @brief Declare the processor logical ports under the supplied stable Node identity.
        GenerationNode(NodeID id,PlanSwitcher& switcher) : Node(id,Config{.layoutOutput={0}}),switcher_(switcher) {}
        /// @brief Capture the active audio generation and render the signal for this callback.
        void process(const ProcessContext& ctx) noexcept override {
            const auto generation=switcher_.audioGeneration();
            noMidBlockSwap=noMidBlockSwap && !switcher_.trySwapAtBlockBoundary();
            for (FrameCount f=0;f<ctx.frames;++f) {
                ctx.outputs[0].buffer[f]=static_cast<float>(generation);
                for (const auto& output : ctx.backendOutputs) output.buffer[f]=static_cast<float>(generation);
            }
            coherent=coherent && generation==switcher_.audioGeneration();
        }
        bool noMidBlockSwap=true /**< No mid block swap retained by the fixture. */,
            coherent=true /**< Whether every processor observed the same adopted generation during its callback. */;
    private:
        /// @brief Borrowed switcher used to observe the current audio generation.
        PlanSwitcher& switcher_;
    };
    /// @brief Build a source/output pair and return its complete logical edge identity.
    inline Result makeDelayedGraph(DAG& graph,std::array<NodeID,2>& ids,Connection& edge,LifetimeTrace* trace=nullptr) {
        const auto a=graph.add<SignalNode>(trace),b=graph.add<SinkNode>();
        if (!a) return std::unexpected(a.error());
        if (!b) return std::unexpected(b.error());
        ids={*a,*b}; edge={encode(*a,PortType::Out,0),encode(*b,PortType::In,0)};
        if (auto result=graph.connect(edge.to,edge.from); !result) return result;
        return graph.markAsOutput(*b);
    }
    /// @brief Owns the small input/output block used by switcher examples in the tests.
    struct IO {
        std::array<float,4> input{1,2,3,4} /**< Owned input sample storage used by the fixture. */,
            output{} /**< Owned output sample storage or selected output-node identity. */;
        /// @brief Input channel descriptors or physical sample pointers used by this state.
        std::array<const float*,1> inputs{input.data()};
        /// @brief Output channel descriptors or physical sample pointers used by this state.
        std::array<float*,1> outputs{output.data()};
        /// @brief Borrow the fixture's input/output arrays for a callback with the supplied frame count.
        mka::audio::AudioProcessContext block(FrameCount frames=4) noexcept { return {inputs,outputs,frames,4,4}; }
        /// @brief Bind the owned four-sample arrays to stable physical callback pointers.
        IO()=default;
        /// @brief Disable copying to preserve stable ownership and borrowed addresses.
        IO(const IO&)=delete;
        /// @brief Disable copying to preserve stable ownership and borrowed addresses.
        IO& operator=(const IO&)=delete;
    };
}
