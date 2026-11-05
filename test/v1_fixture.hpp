/**
 * @file v1_fixture.hpp
 * @brief Test fixtures for v1 fixture.
 * @author Mika
 */
#pragma once
namespace v1_test {
    using namespace mka::audio::dsp;
    /// @brief Thread-local marker delimiting an executing audio callback.
    inline thread_local bool inAudio=false;
    /// @brief Collects numeric callback, binding or lifetime observations for the fixture.
    struct Trace { std::atomic<unsigned> created{0} /**< Number of processor instances constructed on control. */,
                       destroyed{0} /**< Number of processor instances destroyed on control or during the measured scope. */,
                       destroyedInAudio{0} /**< Number of processor destructors observed inside an audio callback. */; };
    /// @brief Selects a processor's role in the fixture signal path.
    enum class Role {
        /// @brief Produce the fixture's source signal.
        Source,
        /// @brief Apply the fixture's scalar gain.
        Gain,
        /// @brief Process a persistent logical-edge compensation history.
        Delay,
        /// @brief Sum the fixture's main and auxiliary signals.
        Mix,
        /// @brief Route a logical signal into selected physical lanes.
        Router,
        /// @brief Perform the fixture's final physical sample writes.
        Writer,
        /// @brief Provide an additional structural processor for edit/lifetime checks.
        Extra
    };
    /// @brief Configurable fixture DSP used to observe routing, latency and lifetime behavior.
    class Processor final : public Node {
    public:
        /// @brief Select the processor role and initialize its logical layouts and observation state.
        Processor(NodeID id,Role role,Trace& trace,LatencySamples delay=0)
            : Node(id,shape(role)),role_(role),trace_(trace),delay_(delay) { ++trace_.created; }
        /// @brief Record fixture destruction and release owned state on the calling thread.
        ~Processor() override { ++trace_.destroyed; if (inAudio) ++trace_.destroyedInAudio; }
        /// @brief Return the fixed algorithmic delay declared by this fixture processor.
        LatencySamples latencySamples() const noexcept override { ++queries; return delay_; }
        /// @brief Render the selected stereo source, gain, delay, Mix or physical-routing role.
        void process(const ProcessContext& ctx) noexcept override {
            if (role_==Role::Extra) return;
            for (FrameCount f=0;f<ctx.frames;++f) {
                for (unsigned c=0;c<2;++c) {
                    float value=c<ctx.inputs.size() ? ctx.inputs[c].buffer[f] : 0.F;
                    if (role_==Role::Gain) value*=0.5F;
                    if (role_==Role::Delay && delay_) {
                        const auto old=history_[c][position_]; history_[c][position_]=value; value=old;
                    }
                    if (role_==Role::Mix) value+=ctx.auxiliary[c].buffer[f];
                    ctx.outputs[c].buffer[f]=value;
                    if (role_==Role::Router) {
                        if (c<ctx.backendOutputs.size()) ctx.backendOutputs[c].buffer[f]=value;
                        if (c+2<ctx.backendOutputs.size()) ctx.backendOutputs[c+2].buffer[f]=value;
                    }
                    if (role_==Role::Writer && c==0 && seen%2==0 && !ctx.backendOutputs.empty())
                        ctx.backendOutputs[0].buffer[f]=2.F*value;
                }
                if (role_==Role::Delay && delay_ && ++position_==delay_) position_=0;
                ++seen;
            }
        }
        /// @brief Number of control-side latency queries observed by the fixture.
        mutable unsigned queries=0;
        /// @brief Sample-stream position or externally owned observations used by this fixture.
        std::uint64_t seen=0;
    private:
        /// @brief Return the logical port layout required by the selected processor role.
        static Config shape(Role role) {
            if (role==Role::Extra) return {};
            if (role==Role::Source) return {.layoutOutput={0,1}};
            if (role==Role::Mix) return {.layoutInput={0,1},.layoutAuxiliary={0,1},.layoutOutput={0,1}};
            return {.layoutInput={0,1},.layoutOutput={0,1}};
        }
        /// @brief Immutable processor role selecting this fixture's signal behavior.
        const Role role_; /** Borrowed lifetime or binding trace; it must outlive the fixture processor. */ Trace& trace_; /** Immutable whole-sample latency used by the processor's delay state. */ const LatencySamples delay_;
        /// @brief Owned persistent delay samples, initialized to zero during preparation.
        std::array<std::array<float,512>,2> history_{};
        /// @brief Cursor into persistent DSP history; empty blocks do not advance it.
        std::size_t position_=0;
    };
    /// @brief Owns a fixture DAG and the node identities used to build its signal paths.
    struct Graph {
        /// @brief Externally observed lifetime, order or context data used by this fixture.
        Trace trace; /** Owned desired graph; every retained plan must be released before this owner. */ DAG dag;
        NodeID source=0 /**< Borrowed producer sample pointer or identity selected for this operation. */,
            gain=0 /**< Scalar multiplication factor configured for this fixture path. */,
            slow=0 /**< Identity of the desired delayed-branch processor. */,
            mix=0 /**< Identity of the desired main/auxiliary junction. */,
            router=0 /**< Identity of the desired physical-output routing processor. */,
            writer=0 /**< Identity of the final physical-output writer. */;
        /// @brief Connect the selected logical channels and propagate any graph-edit error.
        Result link(NodeID from,NodeID to,PortType port=PortType::In) {
            for (ChannelIndex c=0;c<2;++c)
                if (auto r=dag.connect(encode(to,port,c),encode(from,PortType::Out,c)); !r) return r;
            return {};
        }
        /// @brief Build the selected fixture topology and mark its physical-output processors.
        Result make(LatencySamples delay) {
            auto a=dag.add<Processor>(Role::Source,trace),b=dag.add<Processor>(Role::Gain,trace);
            auto c=dag.add<Processor>(Role::Delay,trace,delay),d=dag.add<Processor>(Role::Mix,trace);
            auto e=dag.add<Processor>(Role::Router,trace),f=dag.add<Processor>(Role::Writer,trace);
            if (!a||!b||!c||!d||!e||!f) return std::unexpected(Error{ErrorCode::PrepareFailed});
            source=*a; gain=*b; slow=*c; mix=*d; router=*e; writer=*f;
            for (auto pair : {std::pair{source,gain},std::pair{gain,slow},std::pair{gain,mix},std::pair{mix,router},std::pair{router,writer}})
                if (auto r=link(pair.first,pair.second); !r) return r;
            if (auto r=link(slow,mix,PortType::Aux); !r) return r;
            if (auto r=dag.markAsOutput(router); !r) return r;
            return dag.markAsOutput(writer);
        }
        /// @brief Insert a distinct latency processor and update the desired slow-branch edges.
        Result replaceSlow(LatencySamples delay) {
            const auto next=dag.add<Processor>(Role::Delay,trace,delay);
            if (!next) return std::unexpected(next.error());
            if (auto r=dag.remove(slow); !r) return r;
            slow=*next;
            if (auto r=link(gain,slow); !r) return r;
            return link(slow,mix,PortType::Aux);
        }
    };
    /// @brief Owns sample buffers and callback views that remain stable for one render call.
    struct Bank {
        /// @brief Owned input sample storage used by the fixture.
        std::array<std::array<float,130>,2> input{};
        /// @brief Owned output sample storage or selected output-node identity.
        std::array<std::array<float,130>,6> output{};
        /// @brief Input channel descriptors or physical sample pointers used by this state.
        std::array<const float*,2> inputs{input[0].data()+1,input[1].data()+1};
        /// @brief Output channel descriptors or physical sample pointers used by this state.
        std::array<float*,6> outputs{};
        /// @brief Bind stable callback pointers to the owned sample arrays and their guarded ranges.
        Bank() { for (unsigned c=0;c<6;++c) outputs[c]=output[c].data()+1; }
        /// @brief Disable copying to preserve stable ownership and borrowed addresses.
        Bank(const Bank&)=delete;
        /// @brief Disable copying to preserve stable ownership and borrowed addresses.
        Bank& operator=(const Bank&)=delete;
        /// @brief Initialize the selected input samples and reset output guards before rendering.
        void fill(FrameCount frames,float left=1.F,float right=-2.F) {
            for (unsigned c=0;c<2;++c) { input[c].fill(-999.F); for (FrameCount f=0;f<frames;++f) input[c][f+1]=c ? right:left; }
            for (auto& c : output) c.fill(-999.F);
        }
        /// @brief Borrow the selected physical buffers with explicit frame and channel capacities.
        mka::audio::AudioProcessContext io(FrameCount frames,unsigned out=6) { return {inputs,std::span{outputs}.first(out),frames,128,128}; }
    };
    /// @brief Run one synchronous audio callback while marking the measured audio scope.
    inline void callback(PlanSwitcher& switcher,const mka::audio::AudioProcessContext& io) {
        inAudio=true; switcher.exec(io); inAudio=false;
    }
}
