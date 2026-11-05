/**
 * @file delay_audio_fixture.hpp
 * @brief Test fixtures for delay audio fixture.
 * @author Mika
 */
#pragma once
// Standard headers must precede the module imports in the including .cpp.
namespace audio_pdc_test {
    using namespace mka::audio::dsp;
    /// @brief Sentinel disabling physical output routing for a logical-only fixture processor.
    inline constexpr std::uint32_t noLane=UINT32_MAX;
    /// @brief Return the mono port layout used by this fixture.
    inline Config ports() { return {.layoutInput={0},.layoutAuxiliary={0},.layoutOutput={0}}; }
    /// @brief Build the complete logical edge identity used by the fixture.
    inline Connection edge(NodeID from,NodeID to,PortType port=PortType::In) {
        return {encode(from,PortType::Out,0),encode(to,port,0)};
    }

    // Test DSP implements intrinsic latency by a shift register, independently
    // of the production circular DelayLine. History allocation is control-only.
    /// @brief Mono processor with an actual declared delay and observed logical output.
    class MonoPlugin final : public Node {
    public:
        /// @brief Prepare zero DSP history and store the declared latency and optional physical lanes.
        MonoPlugin(NodeID id,LatencySamples latency,std::uint32_t sourceLane=noLane,std::uint32_t physicalLane=noLane)
            : Node(id,ports()),latency_(latency),sourceLane_(sourceLane),physicalLane_(physicalLane),history_(latency,0.F) {}
        /// @brief Return the fixed algorithmic delay declared by this fixture processor.
        LatencySamples latencySamples() const noexcept override { ++queries; return latency_; }
        /// @brief Render the configured gain, summation and actual delay while capturing logical samples.
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
        std::array<float,8192> mainLog{} /**< Main log retained by the fixture. */,
            auxLog{} /**< Borrowed destination for captured auxiliary-input samples. */,
            outputLog{} /**< Borrowed destination for captured logical output samples. */;
        /// @brief Sample-stream position or externally owned observations used by this fixture.
        std::uint64_t seen=0;
        /// @brief Number of control-side latency queries observed by the fixture.
        mutable unsigned queries=0;
    private:
        /// @brief Fixed intrinsic algorithmic latency declared by the processor.
        const LatencySamples latency_;
        const std::uint32_t sourceLane_ /**< Source lane retained by the fixture. */,
            physicalLane_ /**< Immutable physical output lane, or the no-lane sentinel. */;
        /// @brief Owned persistent delay samples, initialized to zero during preparation.
        std::vector<float> history_;
    };
    /// @brief Owns a graph, retained snapshots and observations for one generated workload.
    enum class Scenario {
        /// @brief Select a zero-latency or zero-valued reference case.
        Zero,
        /// @brief Select independently delayed branches joined by a Mix.
        Parallel,
        /// @brief Select a chained-latency graph before the Mix junction.
        Chain
    };
    /// @brief Expected signal timing or sample data computed independently of the plan.
    struct Reference {
        /// @brief Stable logical node identity.
        NodeID id=0;
        /// @brief Selected physical lane used by the fixture or sample oracle.
        std::uint32_t lane=0;
        /// @brief Algorithmic latency declared by the processor.
        LatencySamples intrinsic=0;
        /// @brief Expected absolute delay of the captured output signal.
        std::uint64_t outDelay=0;
        /// @brief Expected scalar gain of the captured output signal.
        float outGain=1.F;
        /// @brief Expected delay of the main input signal at the inspected task.
        std::uint64_t mainDelay=0;
        /// @brief Expected scalar gain of the main input signal.
        float mainGain=1.F;
        /// @brief Expected delay of the auxiliary input signal.
        std::uint64_t auxDelay=0;
        /// @brief Expected scalar gain of the auxiliary input signal.
        float auxGain=0.F;
    };
    /// @brief Builds the stereo graph variants used by the PDC audio oracle.
    struct Matrix {
        /// @brief Owned desired DAG or detached graph values used by this fixture.
        DAG graph;
        /// @brief Independent timing/gain descriptions for captured logical signals.
        std::vector<Reference> references;
        std::array<NodeID,2> a{} /**< A retained by the fixture. */,
            b{} /**< B retained by the fixture. */,
            fast{} /**< Fast retained by the fixture. */,
            mix{} /**< Identity of the desired main/auxiliary junction. */,
            secondary{} /**< Secondary retained by the fixture. */,
            monitor{} /**< Monitor retained by the fixture. */,
            router{} /**< Identity of the desired physical-output routing processor. */;
        LatencySamples pa=0 /**< Pa retained by the fixture. */,
            pc=0 /**< Pc retained by the fixture. */,
            pb=0 /**< Pb retained by the fixture. */;
        /// @brief Insert a fixture processor and retain its identity for later graph construction.
        std::expected<NodeID,Error> add(LatencySamples p,std::uint32_t sourceLane,std::uint32_t physicalLane,Reference ref) {
            const auto id=graph.add<MonoPlugin>(p,sourceLane,physicalLane);
            if (!id) return std::unexpected(id.error());
            ref.id=*id; ref.intrinsic=p; references.push_back(ref); return *id;
        }
        /// @brief Build the selected fixture topology and mark its physical-output processors.
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
        /// @brief Return the fast-branch edge entering the selected stereo Mix lane.
        Connection mainEdge(unsigned lane) const { return edge(fast[lane],mix[lane]); }
        /// @brief Return the secondary delayed-branch edge for the selected lane.
        Connection secondaryEdge(unsigned lane) const { return edge(a[lane],secondary[lane]); }
        /// @brief Return the reference compensation needed after the chained fast-path delays.
        std::uint64_t mainDelay() const { return std::uint64_t{pb}-pa-pc; }
        /// @brief Return the reference compensation at the secondary branch junction.
        std::uint64_t secondaryDelay() const { return std::uint64_t{pb}-pa; }
        /// @brief Borrow the known MonoPlugin fixture instance associated with the desired identity.
        MonoPlugin& processor(NodeID id) { return *static_cast<MonoPlugin*>(graph.node(id)); }
    };
    /// @brief Return the deterministic input waveform at the absolute sample position.
    inline float inputSample(unsigned lane,std::size_t n) {
        if (lane==0) return n==0 ? 1.F : n==129 ? -2.F : n==513 ? 0.5F : 0.F;
        return n==2 ? 4.F : n==257 ? -0.5F : n==1023 ? 2.F : 0.F;
    }
    /// @brief Return the independent delayed waveform value, including silence before the first input.
    inline float expectedSample(unsigned lane,std::size_t n,std::uint64_t delay,float gain) {
        return n<delay || gain==0.F ? 0.F : gain*inputSample(lane,n-static_cast<std::size_t>(delay));
    }
}
