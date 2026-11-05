/**
 * @file backend_routing_fixture.hpp
 * @brief Test fixtures for backend routing fixture.
 * @author Mika
 */
#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

// Include after importing mka.audio.dsp.dag. No device names in production.
namespace backend_test {
    using namespace mka::audio::dsp;
    using mka::audio::AudioProcessContext;
    /// @brief Sentinel identifying sample positions outside the writable callback range.
    inline constexpr float guard=-9999.F;

    /// @brief Owns guarded physical sample buffers and exposes bounded callback views.
    template<FrameCount Maximum=128>
    struct FakeBackend final {
        /// @brief Compile-time sample capacity of each fake-backend channel.
        static constexpr FrameCount maximum=Maximum;
        std::array<std::array<float,Maximum+2>,6> inputStorage{} /**< Guarded physical input samples owned by the fake backend. */,
            outputStorage{} /**< Guarded writable physical samples owned by the fake backend. */;
        /// @brief Input channel descriptors or physical sample pointers used by this state.
        std::array<const float*,6> inputs{};
        /// @brief Output channel descriptors or physical sample pointers used by this state.
        std::array<float*,6> outputs{};

        /// @brief Initialize guarded deterministic input samples and bind the physical buffer pointers.
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
        /// @brief Disable copying to preserve stable ownership and borrowed addresses.
        FakeBackend(const FakeBackend&)=delete;
        /// @brief Disable copying to preserve stable ownership and borrowed addresses.
        FakeBackend& operator=(const FakeBackend&)=delete;
        /// @brief Disable moving to preserve stable ownership and borrowed addresses.
        FakeBackend(FakeBackend&&)=delete;
        /// @brief Disable moving to preserve stable ownership and borrowed addresses.
        FakeBackend& operator=(FakeBackend&&)=delete;
        /// @brief Restore sample guards or observation counters before the next fixture invocation.
        void reset() noexcept { for (auto& c : outputStorage) c.fill(guard); }
        /// @brief Borrow the selected physical buffers with explicit frame and channel capacities.
        AudioProcessContext io(FrameCount frames,std::uint32_t in,std::uint32_t out) const noexcept {
            return {std::span{inputs}.first(in),std::span{outputs}.first(out),frames,Maximum,Maximum};
        }
    };

    /// External expectations live only during a synchronous test call.
    inline thread_local const AudioProcessContext* currentIO=nullptr;
    /// @brief Temporarily installs the physical-buffer oracle for a synchronous callback.
    struct IOScope {
        /// @brief Observer restored when the scoped oracle leaves scope.
        const AudioProcessContext* previous=currentIO;
        /// @brief Install this callback's physical-buffer observer and save its predecessor.
        explicit IOScope(const AudioProcessContext& io) noexcept { currentIO=&io; }
        /// @brief Restore the previously selected synchronous observation scope.
        ~IOScope() { currentIO=previous; }
    };

    /** Numeric observations only; never retains hardware/context pointers. */
    struct Observation {
        /// @brief Number of processor invocations observed by the fixture.
        unsigned calls=0;
        std::size_t inputs=0 /**< Observed number of main or physical input descriptors. */,
            auxiliary=0 /**< Observed number of auxiliary input descriptors. */,
            physicalOutputs=0 /**< Number of physical output descriptors observed by the fixture. */;
        /// @brief Effective samples per channel for this processing block.
        FrameCount frames=0;
        bool expectSource=true /**< Whether the observer expects direct physical input descriptors. */,
            expectOutput=false /**< Whether the observer expects writable physical output descriptors. */;
        bool directInputs=true /**< Whether source input descriptors preserve the backend's original pointers. */,
            directOutputs=true /**< Whether physical output descriptors preserve the backend's original pointers. */,
            separateStorage=true /**< Whether logical output storage remains separate from physical buffers. */,
            physicalIDsZero=true /**< Whether every physical descriptor uses the required zero logical identity. */;
        bool checkConstantInput=false /**< Whether the observer also checks the supplied constant input samples. */,
            inputSamplesIntact=true /**< Whether all checked read-only input samples retained their original values. */;
        /// @brief Input sample value expected by the pointer/binding observer.
        float constantInput=0.F;
    };

    /// @brief Records context bindings and renders values for backend-routing tests.
    class ProbeNode final : public Node {
    public:
        /// @brief Bind the supplied logical layout and borrow the numeric context observer.
        ProbeNode(NodeID id,Observation& observation,Config cfg={}) : Node(id,cfg),observed_(observation) {}
        /// @brief Observe physical pointer forwarding and copy input values into logical output storage.
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
        /// @brief Borrowed numeric observations updated by this processor.
        Observation& observed_;
    };

    /** Control-set test policy. Logical and physical values intentionally differ. */
    struct WritePolicy {
        /// @brief Bit mask selecting the physical output lanes written by the fixture.
        std::uint32_t lanes=0;
        FrameCount begin=0 /**< First sample position selected for the physical write. */,
            end=UINT32_MAX /**< Exclusive end of the physical write range. */,
            stride=1 /**< Spacing between selected physical writes; zero disables them. */;
        float physicalValue=100.F /**< Base value written to physical output samples. */,
            logicalValue=10.F /**< Value written to the separate logical output signal. */;
    };

    /// @brief Writes selected physical samples independently of its logical output signal.
    class PhysicalWriter final : public Node {
    public:
        /// @brief Borrow the write policy and observer while installing the logical output layout.
        PhysicalWriter(NodeID id,Observation& seen,WritePolicy& policy,Config cfg={.layoutOutput={0}})
            : Node(id,cfg),seen_(seen),policy_(policy) {}
        /// @brief Write the selected physical sample ranges while preserving a separate logical signal.
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
        /// @brief Borrowed callback observations; no backend samples are owned here.
        Observation& seen_;
        /// @brief Borrowed physical/logical write policy configured by the test control path.
        WritePolicy& policy_;
    };
}
