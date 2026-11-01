#pragma once

#include <array>
#include <cstddef>
#include <expected>
#include <span>

// Include after importing mka.audio.dsp.dag. Shared by functional and RT probes.
namespace offline_test {
    using namespace mka::audio::dsp;
    enum class Stage { Source,Gain,Output };

    inline Config stageConfig(Stage stage) {
        if (stage==Stage::Source) return Config{.layoutOutput={0,1}};
        if (stage==Stage::Gain) return Config{.layoutInput={0,1},.layoutOutput={0,1}};
        return Config{.layoutInput={0,1}};
    }

    /** No descriptor/backend pointer is stored in DSP state. */
    class PipelineNode final : public Node {
    public:
        PipelineNode(NodeID id,Stage stage) : Node(id,stageConfig(stage)),stage_(stage) {}
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
        unsigned calls=0;
        FrameCount lastFrames=0;
    private:
        Stage stage_;
    };

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
        static constexpr FrameCount maximum=256;
        std::array<std::array<float,maximum+2>,2> inputStorage{};
        std::array<std::array<float,maximum+2>,3> outputStorage{};
        std::array<const float*,2> inputs{};
        std::array<float*,3> outputs{};

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
        FakeBackend(const FakeBackend&)=delete;
        FakeBackend& operator=(const FakeBackend&)=delete;
        FakeBackend(FakeBackend&&)=delete;
        FakeBackend& operator=(FakeBackend&&)=delete;

        void resetOutputs(float value) noexcept { for (auto& lane : outputStorage) lane.fill(value); }

        /// Every supplied non-null buffer covers maximum samples; count <= 3.
        mka::audio::AudioProcessContext io(FrameCount frames,std::size_t count=3) noexcept {
            return {inputs,std::span{outputs}.first(count),frames,maximum,maximum};
        }
    };
}
