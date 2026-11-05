/**
 * @file offline_pipeline.hpp
 * @brief Test fixtures for offline pipeline.
 * @author Mika
 */
#pragma once

#include <array>
#include <cstddef>
#include <expected>
#include <span>

// Include after importing mka.audio.dsp.dag. Shared by functional and RT probes.
namespace offline_test {
    using namespace mka::audio::dsp;
    /// @brief Selects the arithmetic operation performed by an offline processor.
    enum class Stage {
        /// @brief Produce the fixture's source signal.
        Source,
        /// @brief Apply the fixture's scalar gain.
        Gain,
        /// @brief Select the output fixture case.
        Output
    };

    /// @brief Return the logical port layout required by the selected offline processing stage.
    inline Config stageConfig(Stage stage) {
        if (stage==Stage::Source) return Config{.layoutOutput={0,1}};
        if (stage==Stage::Gain) return Config{.layoutInput={0,1},.layoutOutput={0,1}};
        return Config{.layoutInput={0,1}};
    }

    /** No descriptor/backend pointer is stored in DSP state. */
    class PipelineNode final : public Node {
    public:
        /// @brief Select the offline stage and install its required stereo port layouts.
        PipelineNode(NodeID id,Stage stage) : Node(id,stageConfig(stage)),stage_(stage) {}
        /// @brief Run the source, scalar-gain or physical-output stage and record the processed frame count.
        void process(const ProcessContext& ctx) noexcept override {
            ++calls; lastFrames=ctx.frames;
            for (std::size_t lane=0;lane<2;++lane)
                for (FrameCount i=0;i<ctx.frames;++i) {
                    const auto sample=ctx.inputs[lane].buffer[i]*(stage_==Stage::Gain ? 0.5F : 1.F);
                    if (stage_==Stage::Output) {
                        if (lane<ctx.backendOutputs.size()) ctx.backendOutputs[lane].buffer[i]=sample;
                    } else ctx.outputs[lane].buffer[i]=sample;
                }
        }
        /// @brief Number of processor invocations observed by the fixture.
        unsigned calls=0;
        /// @brief Frame count observed during the most recent processing invocation.
        FrameCount lastFrames=0;
    private:
        /// @brief Immutable role selecting this offline processor's arithmetic operation.
        Stage stage_;
    };

    /// @brief Build the source, gain and physical-output chain used for offline rendering.
    inline Result buildPipeline(DAG& graph,std::array<NodeID,3>& ids) {
        const std::array stages{Stage::Source,Stage::Gain,Stage::Output};
        for (std::size_t i=0;i<stages.size();++i) {
            const auto added=graph.add<PipelineNode>(stages[i]);
            if (!added) return std::unexpected(added.error());
            ids[i]=*added;
        }
        for (std::size_t edge=0;edge<2;++edge)
            for (ChannelIndex lane=0;lane<2;++lane)
                if (const auto result=graph.connect(encode(ids[edge+1],PortType::In,lane),
                    encode(ids[edge],PortType::Out,lane)); !result) return result;
        return graph.markAsOutput(ids[2]);
    }

    /** Fixed fake backend; nonmovable because its spans borrow its own pointer arrays. */
    struct FakeBackend {
        /// @brief Compile-time sample capacity of each fake-backend channel.
        static constexpr FrameCount maximum=256;
        /// @brief Guarded physical input samples owned by the fake backend.
        std::array<std::array<float,maximum+2>,2> inputStorage{};
        /// @brief Guarded writable physical samples owned by the fake backend.
        std::array<std::array<float,maximum+2>,3> outputStorage{};
        /// @brief Input channel descriptors or physical sample pointers used by this state.
        std::array<const float*,2> inputs{};
        /// @brief Output channel descriptors or physical sample pointers used by this state.
        std::array<float*,3> outputs{};

        /// @brief Initialize guarded deterministic input samples and bind the physical buffer pointers.
        explicit FakeBackend(float seed) noexcept {
            for (std::size_t lane=0;lane<2;++lane) {
                inputStorage[lane].fill(-333.F);
                for (FrameCount i=0;i<maximum;++i)
                    inputStorage[lane][i+1]=lane==0 ? seed+static_cast<float>(i)*0.25F :
                        -seed-static_cast<float>(i)*0.125F;
                inputs[lane]=inputStorage[lane].data()+1;
            }
            for (std::size_t lane=0;lane<3;++lane) outputs[lane]=outputStorage[lane].data()+1;
            resetOutputs(999.F);
        }
        /// @brief Disable copying to preserve stable ownership and borrowed addresses.
        FakeBackend(const FakeBackend&)=delete;
        /// @brief Disable copying to preserve stable ownership and borrowed addresses.
        FakeBackend& operator=(const FakeBackend&)=delete;
        /// @brief Disable moving to preserve stable ownership and borrowed addresses.
        FakeBackend(FakeBackend&&)=delete;
        /// @brief Disable moving to preserve stable ownership and borrowed addresses.
        FakeBackend& operator=(FakeBackend&&)=delete;

        /// @brief Fill every guarded physical output lane with the supplied sentinel value.
        void resetOutputs(float value) noexcept { for (auto& lane : outputStorage) lane.fill(value); }

        /// Every supplied non-null buffer covers maximum samples; count <= 3.
        mka::audio::AudioProcessContext io(FrameCount frames,std::size_t count=3) noexcept {
            return {inputs,std::span{outputs}.first(count),frames,maximum,maximum};
        }
    };
}
