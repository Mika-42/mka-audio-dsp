#pragma once
// Standard headers must precede the module imports in the including .cpp.
namespace audio_pdc_test {
    using namespace mka::audio::dsp;
    inline constexpr std::uint32_t noLane=UINT32_MAX;
    inline Config ports() { return {.layoutInput={0},.layoutAuxiliary={0},.layoutOutput={0}}; }
    inline Connection edge(NodeID from,NodeID to,PortType port=PortType::In) {
        return {encode(from,PortType::Out,0),encode(to,port,0)};
    }

    // Test DSP implements intrinsic latency by a shift register, independently
    // of the production circular DelayLine. History allocation is control-only.
    class MonoPlugin final : public Node {
    public:
        MonoPlugin(NodeID id,LatencySamples latency,std::uint32_t sourceLane=noLane,std::uint32_t physicalLane=noLane)
            : Node(id,ports()),latency_(latency),sourceLane_(sourceLane),physicalLane_(physicalLane),history_(latency,0.F) {}
        LatencySamples latencySamples() const noexcept override { ++queries; return latency_; }
        void process(const ProcessContext& ctx) noexcept override {
            for (FrameCount f=0;f<ctx.frames;++f) {
                const auto index=sourceLane_==noLane ? 0 : sourceLane_;
                const auto main=index<ctx.inputs.size() ? ctx.inputs[index].buffer[f] : 0.F;
                const auto aux=ctx.auxiliary.empty() ? 0.F : ctx.auxiliary[0].buffer[f];
                float value=main+aux;
                if (!history_.empty()) {
                    const auto delayed=history_[0];
                    for (std::size_t i=1;i<history_.size();++i) history_[i-1]=history_[i];
                    history_.back()=value; value=delayed;
                }
                for (const auto& output : ctx.outputs) output.buffer[f]=value;
                if (physicalLane_<ctx.backendOutputs.size()) ctx.backendOutputs[physicalLane_].buffer[f]=value;
                if (seen<outputLog.size()) {
                    mainLog[seen]=main; auxLog[seen]=aux; outputLog[seen]=value;
                }
                ++seen;
            }
        }
        std::array<float,8192> mainLog{},auxLog{},outputLog{};
        std::uint64_t seen=0;
        mutable unsigned queries=0;
    private:
        const LatencySamples latency_;
        const std::uint32_t sourceLane_,physicalLane_;
        std::vector<float> history_;
    };
    enum class Scenario { Zero,Parallel,Chain };
    struct Reference {
        NodeID id=0;
        std::uint32_t lane=0;
        LatencySamples intrinsic=0;
        std::uint64_t outDelay=0;
        float outGain=1.F;
        std::uint64_t mainDelay=0;
        float mainGain=1.F;
        std::uint64_t auxDelay=0;
        float auxGain=0.F;
    };
    struct Matrix {
        DAG graph;
        std::vector<Reference> references;
        std::array<NodeID,2> a{},b{},fast{},mix{},secondary{},monitor{},router{};
        LatencySamples pa=0,pc=0,pb=0;
        std::expected<NodeID,Error> add(LatencySamples p,std::uint32_t sourceLane,std::uint32_t physicalLane,Reference ref) {
            const auto id=graph.add<MonoPlugin>(p,sourceLane,physicalLane);
            if (!id) return std::unexpected(id.error());
            ref.id=*id; ref.intrinsic=p; references.push_back(ref); return *id;
        }
        Result make(Scenario scenario) {
            if (!references.empty()) return std::unexpected(Error{ErrorCode::InvalidParameter});
            if (scenario==Scenario::Parallel) { pa=64; pb=256; }
            if (scenario==Scenario::Chain) { pa=128; pc=64; pb=512; }
            for (std::uint32_t lane=0;lane<2;++lane) {
                const auto first=add(pa,lane,noLane,{.lane=lane,.outDelay=pa});
                const auto slow=add(pb,lane,noLane,{.lane=lane,.outDelay=pb});
                if (!first || !slow) return std::unexpected(Error{ErrorCode::PrepareFailed});
                a[lane]=*first; b[lane]=*slow; fast[lane]=*first;
                if (pc) {
                    const auto c=add(pc,noLane,noLane,{.lane=lane,.outDelay=std::uint64_t{pa}+pc,.mainDelay=pa});
                    if (!c) return std::unexpected(c.error());
                    fast[lane]=*c; const auto e=edge(*first,*c);
                    if (const auto result=graph.connect(e.to,e.from); !result) return result;
                }
                const Reference join{.lane=lane,.outDelay=pb,.outGain=2,.mainDelay=pb,.auxDelay=pb,.auxGain=1};
                const auto m=add(0,noLane,noLane,join),s=add(0,noLane,noLane,join);
                const auto observe=add(0,noLane,noLane,{.lane=lane,.outDelay=pa,.mainDelay=pa});
                const auto out=add(0,noLane,lane,{.lane=lane,.outDelay=pb,.outGain=2,.mainDelay=pb,.mainGain=2});
                if (!m || !s || !observe || !out) return std::unexpected(Error{ErrorCode::PrepareFailed});
                mix[lane]=*m; secondary[lane]=*s; monitor[lane]=*observe; router[lane]=*out;
                for (const auto e : {edge(fast[lane],*m),edge(*slow,*m,PortType::Aux),edge(*first,*s),
                                     edge(*slow,*s,PortType::Aux),edge(*first,*observe),edge(*m,*out)})
                    if (const auto result=graph.connect(e.to,e.from); !result) return result;
                if (const auto result=graph.markAsOutput(*out); !result) return result;
            }
            return {};
        }
        Connection mainEdge(unsigned lane) const { return edge(fast[lane],mix[lane]); }
        Connection secondaryEdge(unsigned lane) const { return edge(a[lane],secondary[lane]); }
        std::uint64_t mainDelay() const { return std::uint64_t{pb}-pa-pc; }
        std::uint64_t secondaryDelay() const { return std::uint64_t{pb}-pa; }
        MonoPlugin& processor(NodeID id) { return *static_cast<MonoPlugin*>(graph.node(id)); }
    };
    inline float inputSample(unsigned lane,std::size_t n) {
        if (lane==0) return n==0 ? 1.F : n==129 ? -2.F : n==513 ? 0.5F : 0.F;
        return n==2 ? 4.F : n==257 ? -0.5F : n==1023 ? 2.F : 0.F;
    }
    inline float expectedSample(unsigned lane,std::size_t n,std::uint64_t delay,float gain) {
        return n<delay || gain==0.F ? 0.F : gain*inputSample(lane,n-static_cast<std::size_t>(delay));
    }
}
