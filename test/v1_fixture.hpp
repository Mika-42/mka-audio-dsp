#pragma once
namespace v1_test {
    using namespace mka::audio::dsp;
    inline thread_local bool inAudio=false;
    struct Trace { std::atomic<unsigned> created{0},destroyed{0},destroyedInAudio{0}; };
    enum class Role { Source,Gain,Delay,Mix,Router,Writer,Extra };
    class Processor final : public Node {
    public:
        Processor(NodeID id,Role role,Trace& trace,LatencySamples delay=0)
            : Node(id,shape(role)),role_(role),trace_(trace),delay_(delay) { ++trace_.created; }
        ~Processor() override { ++trace_.destroyed; if (inAudio) ++trace_.destroyedInAudio; }
        LatencySamples latencySamples() const noexcept override { ++queries; return delay_; }
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
        mutable unsigned queries=0;
        std::uint64_t seen=0;
    private:
        static Config shape(Role role) {
            if (role==Role::Extra) return {};
            if (role==Role::Source) return {.layoutOutput={0,1}};
            if (role==Role::Mix) return {.layoutInput={0,1},.layoutAuxiliary={0,1},.layoutOutput={0,1}};
            return {.layoutInput={0,1},.layoutOutput={0,1}};
        }
        const Role role_; Trace& trace_; const LatencySamples delay_;
        std::array<std::array<float,512>,2> history_{};
        std::size_t position_=0;
    };
    struct Graph {
        Trace trace; DAG dag;
        NodeID source=0,gain=0,slow=0,mix=0,router=0,writer=0;
        Result link(NodeID from,NodeID to,PortType port=PortType::In) {
            for (ChannelIndex c=0;c<2;++c)
                if (auto r=dag.connect(encode(to,port,c),encode(from,PortType::Out,c)); !r) return r;
            return {};
        }
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
        Result replaceSlow(LatencySamples delay) {
            const auto next=dag.add<Processor>(Role::Delay,trace,delay);
            if (!next) return std::unexpected(next.error());
            if (auto r=dag.remove(slow); !r) return r;
            slow=*next;
            if (auto r=link(gain,slow); !r) return r;
            return link(slow,mix,PortType::Aux);
        }
    };
    struct Bank {
        std::array<std::array<float,130>,2> input{};
        std::array<std::array<float,130>,6> output{};
        std::array<const float*,2> inputs{input[0].data()+1,input[1].data()+1};
        std::array<float*,6> outputs{};
        Bank() { for (unsigned c=0;c<6;++c) outputs[c]=output[c].data()+1; }
        Bank(const Bank&)=delete;
        Bank& operator=(const Bank&)=delete;
        void fill(FrameCount frames,float left=1.F,float right=-2.F) {
            for (unsigned c=0;c<2;++c) { input[c].fill(-999.F); for (FrameCount f=0;f<frames;++f) input[c][f+1]=c ? right:left; }
            for (auto& c : output) c.fill(-999.F);
        }
        mka::audio::AudioProcessContext io(FrameCount frames,unsigned out=6) { return {inputs,std::span{outputs}.first(out),frames,128,128}; }
    };
    inline void callback(PlanSwitcher& switcher,const mka::audio::AudioProcessContext& io) {
        inAudio=true; switcher.exec(io); inAudio=false;
    }
}
