/**
 * @file pair_routing_fixture.hpp
 * @brief Test fixtures for pair routing fixture.
 * @author Mika
 */
#pragma once
#include <array>
#include <expected>
#include "backend_routing_fixture.hpp"

namespace backend_test {
    /** Routing is entirely a Node policy, with physical indices checked in process. */
    struct PairRouting {
        /// @brief Input channel descriptors or physical sample pointers used by this state.
        std::array<ChannelIndex,2> inputs{0,1};
        /// @brief Physical output pairs selected for each routing processor.
        std::array<std::array<ChannelIndex,2>,3> destinations{{{0,1},{2,3},{4,5}}};
        unsigned pairs=1 /**< Pairs retained by the fixture. */,
            writeMask=3 /**< Bit mask selecting the enabled physical destination pairs. */;
        /// @brief Scalar multiplication factor configured for this fixture path.
        float gain=1.F;
        FrameCount begin=0 /**< First sample position selected for the physical write. */,
            end=UINT32_MAX /**< Exclusive end of the physical write range. */,
            stride=1 /**< Spacing between selected physical writes; zero disables them. */;
    };

    /// @brief Routes a logical stereo pair to selected physical lanes and optional successors.
    class PairRouter final : public Node {
    public:
        /// @brief Borrow the pair-routing policy and binding observer for the supplied layouts.
        PairRouter(NodeID id,PairRouting& routing,Observation& seen,Config cfg={.layoutOutput={0,1}})
            : Node(id,cfg),routing_(routing),seen_(seen) {}
        /// @brief Copy the selected input pair to its configured physical lanes and logical successors.
        void process(const ProcessContext& ctx) noexcept override {
            ++seen_.calls; seen_.inputs=ctx.inputs.size(); seen_.physicalOutputs=ctx.backendOutputs.size();
            seen_.frames=ctx.frames;
            if (currentIO) {
                if (seen_.expectSource) {
                    seen_.directInputs=seen_.directInputs && ctx.inputs.size()==currentIO->inputs.size();
                    for (std::size_t c=0;c<ctx.inputs.size();++c)
                        seen_.directInputs=seen_.directInputs && ctx.inputs[c].buffer==currentIO->inputs[c] && ctx.inputs[c].channel==0;
                }
                seen_.directOutputs=seen_.directOutputs && ctx.backendOutputs.size()==
                    (seen_.expectOutput ? currentIO->outputs.size() : 0);
                for (std::size_t c=0;c<ctx.backendOutputs.size();++c) {
                    seen_.directOutputs=seen_.directOutputs && ctx.backendOutputs[c].buffer==currentIO->outputs[c];
                    seen_.physicalIDsZero=seen_.physicalIDsZero && ctx.backendOutputs[c].channel==0;
                    for (const auto& out : ctx.outputs) if (ctx.frames)
                        seen_.separateStorage=seen_.separateStorage && out.buffer!=ctx.backendOutputs[c].buffer;
                }
            }
            for (FrameCount f=0;f<ctx.frames;++f) for (unsigned lane=0;lane<2;++lane) {
                const auto input=routing_.inputs[lane];
                const float sample=input<ctx.inputs.size() ? ctx.inputs[input].buffer[f]*routing_.gain : 0.F;
                if (lane<ctx.outputs.size()) ctx.outputs[lane].buffer[f]=sample;
                if (routing_.stride==0 || f<routing_.begin || f>=routing_.end ||
                    (f-routing_.begin)%routing_.stride!=0 || !(routing_.writeMask&(1U<<lane))) continue;
                for (unsigned pair=0;pair<routing_.pairs && pair<routing_.destinations.size();++pair) {
                    const auto channel=routing_.destinations[pair][lane];
                    if (channel<ctx.backendOutputs.size()) ctx.backendOutputs[channel].buffer[f]=sample;
                }
            }
        }
    private:
        /// @brief Borrowed channel-pair routing policy selected by the control path.
        PairRouting& routing_;
        /// @brief Borrowed callback observations; no backend samples are owned here.
        Observation& seen_;
    };

    /// @brief Build processors that route stereo pairs to the configured physical output lanes.
    inline Result buildPairRouting(DAG& graph,std::array<NodeID,3>& ids,std::array<PairRouting,3>& routing,
                                  std::array<Observation,3>& seen,std::uint32_t outputs) {
        routing[0].inputs={1,5};
        routing[1].gain=0.5F;
        routing[1].destinations[0]=outputs==2 ? std::array<ChannelIndex,2>{0,1} : std::array<ChannelIndex,2>{2,3};
        routing[2].inputs={1,5}; routing[2].gain=2.F;
        routing[2].destinations[0]={1,4}; routing[2].pairs=outputs==2 ? 0 : 1;
        for (unsigned i=0;i<3;++i) {
            seen[i].expectOutput=true; seen[i].expectSource=i!=1;
            const auto cfg=i==1 ? Config{.layoutInput={0,1},.layoutOutput={0,1}} : Config{.layoutOutput={0,1}};
            const auto id=graph.add<PairRouter>(routing[i],seen[i],cfg);
            if (!id) return std::unexpected(id.error());
            ids[i]=*id;
            if (const auto result=graph.markAsOutput(*id); !result) return result;
        }
        for (ChannelIndex lane=0;lane<2;++lane)
            if (const auto result=graph.connect(encode(ids[1],PortType::In,lane),encode(ids[0],PortType::Out,lane)); !result)
                return result;
        return {};
    }

    /// @brief Compute the independent physical-lane sample for the selected routing configuration.
    inline float routedPairSample(std::uint32_t outputs,unsigned channel,FrameCount frame,const FakeBackend<>& backend) noexcept {
        const auto left=backend.inputs[1][frame],right=backend.inputs[5][frame];
        if (outputs==2) return channel==0 ? left*0.5F : right*0.5F;
        switch (channel) {
            case 0: return left;
            case 1: return left*2.F; // A later independent Node writes this shared lane.
            case 2: return left*0.5F;
            case 3: return right*0.5F;
            case 4: return right*2.F;
            default: return 0.F;
        }
    }
}
