#pragma once
#include <array>
#include <expected>
#include "backend_routing_fixture.hpp"

namespace backend_test {
    inline Result buildLastWriters(DAG& graph,std::array<NodeID,5>& ids,
        std::array<Observation,5>& seen,std::array<WritePolicy,4>& policies,bool reversed=false) {
        policies[0]={.lanes=1U,.begin=2,.end=6,.stride=2,.physicalValue=20.F,.logicalValue=22.F};
        policies[1]={.lanes=3U,.physicalValue=10.F,.logicalValue=111.F};
        policies[2]={.lanes=6U,.begin=1,.end=4,.physicalValue=30.F,.logicalValue=33.F};
        policies[3]={.lanes=0x3FU,.physicalValue=-100.F,.logicalValue=99.F};
        for (std::size_t i=0;i<4;++i) {
            seen[i].expectOutput=i<3; seen[i].expectSource=i!=0;
            const auto cfg=i==0 ? Config{.layoutInput={0},.layoutOutput={0}} : Config{.layoutOutput={0}};
            const auto id=graph.add<PhysicalWriter>(seen[i],policies[i],cfg);
            if (!id) return std::unexpected(id.error());
            ids[i]=*id;
        }
        seen[4].expectSource=false; seen[4].checkConstantInput=true; seen[4].constantInput=22.F;
        const auto reader=graph.add<ProbeNode>(seen[4],Config{.layoutInput={0}});
        if (!reader) return std::unexpected(reader.error());
        ids[4]=*reader;
        const std::array edges{Connection{encode(ids[1],PortType::Out,0),encode(ids[0],PortType::In,0)},
                               Connection{encode(ids[0],PortType::Out,0),encode(ids[4],PortType::In,0)}};
        for (unsigned i=0;i<2;++i) {
            const auto& edge=edges[reversed ? 1-i : i];
            if (const auto result=graph.connect(edge.to,edge.from); !result) return result;
        }
        for (unsigned i=0;i<3;++i) if (const auto result=graph.markAsOutput(ids[i]); !result) return result;
        return {};
    }

    /// Independent sample equation, in the proven order producer -> consumer -> late.
    inline float lastWriterSample(unsigned channel,FrameCount frame,const std::array<WritePolicy,4>& policies) {
        float result=0.F;
        if (channel<2) result=policies[1].physicalValue+static_cast<float>(channel);
        if (channel==0 && (frame==2 || frame==4)) result=policies[0].physicalValue;
        if ((channel==1 || channel==2) && frame>=1 && frame<4)
            result=policies[2].physicalValue+static_cast<float>(channel);
        return result;
    }
}
