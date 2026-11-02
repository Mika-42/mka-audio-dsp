#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

// Include after importing mka.audio.dsp.dag. No device names in production.
namespace backend_test {
    using namespace mka::audio::dsp;
    using mka::audio::AudioProcessContext;
    inline constexpr float guard=-9999.F;

    template<FrameCount Maximum=128>
    struct FakeBackend final {
        static constexpr FrameCount maximum=Maximum;
        std::array<std::array<float,Maximum+2>,6> inputStorage{},outputStorage{};
        std::array<const float*,6> inputs{};
        std::array<float*,6> outputs{};

        explicit FakeBackend(float seed=0.F) noexcept {
            for (std::size_t c=0;c<6;++c) {
                inputStorage[c].fill(guard);
                for (FrameCount f=0;f<Maximum;++f)
                    inputStorage[c][f+1]=seed+static_cast<float>(c+1)*10.F+static_cast<float>(f)*0.25F;
                inputs[c]=inputStorage[c].data()+1;
                outputs[c]=outputStorage[c].data()+1;
            }
            reset();
        }
        FakeBackend(const FakeBackend&)=delete;
        FakeBackend& operator=(const FakeBackend&)=delete;
        FakeBackend(FakeBackend&&)=delete;
        FakeBackend& operator=(FakeBackend&&)=delete;
        void reset() noexcept { for (auto& c : outputStorage) c.fill(guard); }
        AudioProcessContext io(FrameCount frames,std::uint32_t in,std::uint32_t out) const noexcept {
            return {std::span{inputs}.first(in),std::span{outputs}.first(out),frames,Maximum,Maximum};
        }
    };

    /// External expectations live only during a synchronous test call.
    inline thread_local const AudioProcessContext* currentIO=nullptr;
    struct IOScope {
        const AudioProcessContext* previous=currentIO;
        explicit IOScope(const AudioProcessContext& io) noexcept { currentIO=&io; }
        ~IOScope() { currentIO=previous; }
    };

    /** Numeric observations only; never retains hardware/context pointers. */
    struct Observation {
        unsigned calls=0;
        std::size_t inputs=0,auxiliary=0,physicalOutputs=0;
        FrameCount frames=0;
        bool expectSource=true,expectOutput=false;
        bool directInputs=true,directOutputs=true,separateStorage=true,physicalIDsZero=true;
        bool checkConstantInput=false,inputSamplesIntact=true;
        float constantInput=0.F;
    };

    class ProbeNode final : public Node {
    public:
        ProbeNode(NodeID id,Observation& observation,Config cfg={}) : Node(id,cfg),observed_(observation) {}
        void process(const ProcessContext& ctx) noexcept override {
            auto& o=observed_; ++o.calls;
            o.inputs=ctx.inputs.size(); o.auxiliary=ctx.auxiliary.size();
            o.physicalOutputs=ctx.backendOutputs.size(); o.frames=ctx.frames;
            if (o.checkConstantInput && !ctx.inputs.empty())
                for (FrameCount f=0;f<ctx.frames;++f)
                    o.inputSamplesIntact=o.inputSamplesIntact && ctx.inputs[0].buffer[f]==o.constantInput;
            if (currentIO) {
                if (o.expectSource) {
                    o.directInputs=o.directInputs && ctx.inputs.size()==currentIO->inputs.size();
                    if (ctx.inputs.size()==currentIO->inputs.size()) for (std::size_t c=0;c<ctx.inputs.size();++c) {
                        o.directInputs=o.directInputs && ctx.inputs[c].buffer==currentIO->inputs[c];
                        o.physicalIDsZero=o.physicalIDsZero && ctx.inputs[c].channel==0;
                    }
                }
                o.directOutputs=o.directOutputs && ctx.backendOutputs.size()==
                    (o.expectOutput ? currentIO->outputs.size() : 0);
                for (std::size_t c=0;c<ctx.backendOutputs.size();++c) {
                    o.directOutputs=o.directOutputs && ctx.backendOutputs[c].buffer==currentIO->outputs[c];
                    o.physicalIDsZero=o.physicalIDsZero && ctx.backendOutputs[c].channel==0;
                    for (const auto& out : ctx.outputs)
                        if (ctx.frames) o.separateStorage=o.separateStorage && out.buffer!=ctx.backendOutputs[c].buffer;
                }
            }
            for (const auto& out : ctx.outputs) for (FrameCount f=0;f<ctx.frames;++f)
                out.buffer[f]=ctx.inputs.empty() ? 7.F : ctx.inputs[0].buffer[f];
        }
    private:
        Observation& observed_;
    };

    /** Control-set test policy. Logical and physical values intentionally differ. */
    struct WritePolicy {
        std::uint32_t lanes=0;
        FrameCount begin=0,end=UINT32_MAX,stride=1;
        float physicalValue=100.F,logicalValue=10.F;
    };

    class PhysicalWriter final : public Node {
    public:
        PhysicalWriter(NodeID id,Observation& seen,WritePolicy& policy,Config cfg={.layoutOutput={0}})
            : Node(id,cfg),seen_(seen),policy_(policy) {}
        void process(const ProcessContext& ctx) noexcept override {
            ++seen_.calls; seen_.inputs=ctx.inputs.size(); seen_.frames=ctx.frames;
            seen_.physicalOutputs=ctx.backendOutputs.size();
            if (currentIO) {
                seen_.directOutputs=seen_.directOutputs && ctx.backendOutputs.size()==
                    (seen_.expectOutput ? currentIO->outputs.size() : 0);
                for (std::size_t c=0;c<ctx.backendOutputs.size();++c) {
                    seen_.directOutputs=seen_.directOutputs && ctx.backendOutputs[c].buffer==currentIO->outputs[c];
                    seen_.physicalIDsZero=seen_.physicalIDsZero && ctx.backendOutputs[c].channel==0;
                    for (const auto& out : ctx.outputs) if (ctx.frames)
                        seen_.separateStorage=seen_.separateStorage && out.buffer!=ctx.backendOutputs[c].buffer;
                }
            }
            for (const auto& out : ctx.outputs) for (FrameCount f=0;f<ctx.frames;++f) out.buffer[f]=policy_.logicalValue;
            if (policy_.stride==0) return;
            for (std::size_t c=0;c<ctx.backendOutputs.size() && c<32;++c) if (policy_.lanes&(1U<<c))
                for (FrameCount f=0;f<ctx.frames;++f)
                    if (f>=policy_.begin && f<policy_.end && (f-policy_.begin)%policy_.stride==0)
                        ctx.backendOutputs[c].buffer[f]=policy_.physicalValue+static_cast<float>(c);
        }
    private:
        Observation& seen_;
        WritePolicy& policy_;
    };
}
