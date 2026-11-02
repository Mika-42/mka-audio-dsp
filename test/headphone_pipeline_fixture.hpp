#pragma once
#include <array>
#include <expected>
#include "pair_routing_fixture.hpp"

namespace backend_test {
    /** Test policies: physical inputs -> source -> gain -> two pairs -> partial writer. */
    inline Result buildHeadphonePipeline(DAG& graph,std::array<NodeID,4>& ids,
        std::array<PairRouting,4>& routing,std::array<Observation,4>& seen) {
        routing[0].inputs={1,5}; routing[0].pairs=0;
        routing[1].gain=0.5F; routing[1].pairs=0;
        routing[2].pairs=2;
        routing[3].gain=2.F; routing[3].writeMask=1; routing[3].stride=2;
        for (unsigned i=0;i<4;++i) {
            seen[i].expectSource=i==0; seen[i].expectOutput=i!=1;
            const auto cfg=i==0 ? Config{.layoutOutput={0,1}} : Config{.layoutInput={0,1},.layoutOutput={0,1}};
            const auto id=graph.add<PairRouter>(routing[i],seen[i],cfg);
            if (!id) return std::unexpected(id.error());
            ids[i]=*id;
            if (i!=1) if (const auto result=graph.markAsOutput(*id); !result) return result;
        }
        for (unsigned stage=1;stage<4;++stage) for (ChannelIndex lane=0;lane<2;++lane)
            if (const auto result=graph.connect(encode(ids[stage],PortType::In,lane),encode(ids[stage-1],PortType::Out,lane)); !result)
                return result;
        return {};
    }

    inline float headphoneSample(unsigned channel,FrameCount frame,const FakeBackend<>& backend) noexcept {
        if (channel==0) return backend.inputs[1][frame]*(frame%2==0 ? 1.F : 0.5F);
        if (channel==1 || channel==3) return backend.inputs[5][frame]*0.5F;
        if (channel==2) return backend.inputs[1][frame]*0.5F;
        return 0.F;
    }
}
