#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <expected>
#include <fcntl.h>
#include <new>
#include <numeric>
#include <span>
#include <string>
#include <utility>
#include <vector>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
import mka.audio.dsp.plan_switcher;
#include "../test/v1_fixture.hpp"
#include "rt_probe.hpp"
using namespace mka::audio::dsp;
void require(Result r) { if(!r) { std::fprintf(stderr,"control error %u\n",unsigned(r.error().code)); std::exit(2); } }
struct Scenario {
    v1_test::Trace trace; DAG graph;
    NodeID add(v1_test::Role role,unsigned delay=0) { auto id=graph.add<v1_test::Processor>(role,trace,delay); if(!id) std::exit(2); return *id; }
    void link(NodeID from,NodeID to,PortType port=PortType::In) {
        for(ChannelIndex c=0;c<2;++c) require(graph.connect(encode(to,port,c),encode(from,PortType::Out,c)));
    }
    void make(std::string shape,unsigned branches,bool pdc) {
        auto source=add(v1_test::Role::Source),fast=add(v1_test::Role::Gain); link(source,fast);
        auto other=(branches>=8) ? add(v1_test::Role::Source) : source;
        auto slow=add(v1_test::Role::Delay,pdc?64:0); link(other,slow);
        if(shape=="chain") for(unsigned i=0;i<8;++i) { auto next=add(v1_test::Role::Gain); link(fast,next); fast=next; }
        for(unsigned i=0;i<branches;++i) {
            auto mix=add(v1_test::Role::Mix),out=add(v1_test::Role::Router);
            link(fast,mix); link(slow,mix,PortType::Aux); link(mix,out); require(graph.markAsOutput(out));
        }
    }
};
int main(int argc,char** argv) {
    if(argc!=2) { std::fprintf(stderr,"usage: mka-v1-benchmark output-directory\n"); return 2; }
    const std::string root=argv[1];
    // Positive controls prove that the linked allocation/mutex/wait/log probes run.
    const int fd=open("/dev/null",O_WRONLY); pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
    probe::reset(); probe::active=true;
    void* p=::operator new(17); ::operator delete(p);
    pthread_mutex_trylock(&mutex); pthread_mutex_unlock(&mutex);
    timespec zero{}; nanosleep(&zero,nullptr); write(fd,"x",1);
    probe::active=false; close(fd); pthread_mutex_destroy(&mutex);
    if(!probe::allocations||!probe::frees||!probe::mutexes||!probe::waits||!probe::logs) return 3;
    auto* raw=std::fopen((root+"/raw.csv").c_str(),"w"); auto* summary=std::fopen((root+"/summary.csv").c_str(),"w");
    if(!raw||!summary) return 2;
    std::fprintf(raw,"scenario,branches,pdc,outputs,frames,iteration,duration_us\n");
    std::fprintf(summary,"scenario,branches,pdc,outputs,frames,sample_rate,nodes,tasks,peak_slots,pool_bytes,silence_bytes,history_bytes,total_plan_bytes,max_latency,mean_us,median_us,p95_us,p99_us,max_us,deadline_us,overruns,allocations,frees,mutexes,waits,logs,capacity_diagnostics,publications,refused_candidates,checksum\n");
    constexpr unsigned repetitions=100,warmup=20;
    for(const auto& shape : {std::string("chain"),std::string("diamond"),std::string("fan2"),std::string("fan8"),std::string("fan64"),std::string("publish")})
    for(unsigned pdc : {0U,1U}) for(unsigned outputs : {2U,6U}) for(FrameCount frames : {1U,64U,128U,512U,4096U}) {
        unsigned branches=shape=="fan64"?64:shape=="fan8"?8:shape=="fan2"?2:1;
        Scenario scenario; scenario.make(shape,branches,pdc); PlanSwitcher sw;
        BackendConfig cfg{.maxFrames=frames,.inputChannels=2,.outputChannels=outputs,.sampleRate=48000,.memoryBudgetBytes=256U*1024U*1024U};
        require(sw.initialize(scenario.graph,cfg)); const auto* plan=sw.slotAddress(0);
        const auto usage=plan->preparedMemoryUsage(); const auto slots=plan->bufferSlotCount(); const auto nodes=plan->tasks().size(),tasks=plan->operationSchedule().size();
        std::uint64_t latency=0; for(const auto& n : plan->pdcAnalysis()->nodes) latency=std::max(latency,n.cumulative);
        auto rejected=cfg; rejected.memoryBudgetBytes=0; const auto refusal=sw.prepareInactivePlan(scenario.graph,rejected); if(refusal || refusal.error().code!=ErrorCode::MemoryBudgetExceeded) return 4;
        std::array<std::vector<float>,2> input; std::array<std::vector<float>,6> output;
        for(unsigned c=0;c<2;++c) input[c].assign(frames,c? -2.F:1.F);
        for(auto& c:output) c.resize(frames+2,-999.F);
        std::array<const float*,2> in{input[0].data(),input[1].data()}; std::array<float*,6> out{};
        for(unsigned c=0;c<6;++c) out[c]=output[c].data()+1;
        const mka::audio::AudioProcessContext io{in,std::span{out}.first(outputs),frames,frames,frames};
        std::vector<double> durations; durations.reserve(repetitions); double checksum=0; unsigned publications=0;
        probe::reset();
        for(unsigned i=0;i<repetitions+warmup;++i) {
            if(shape=="publish" && i%10==0) { require(sw.prepareAndPublish(scenario.graph,cfg)); ++publications; }
            const auto start=std::chrono::steady_clock::now(); probe::active=true;
            sw.exec(io);
            probe::active=false; const auto end=std::chrono::steady_clock::now();
            if(sw.handshakeState()==HandshakeState::Acknowledged) require(sw.retireAcknowledgedPlan());
            for(unsigned c=0;c<outputs;++c) {
                if(output[c].front()!=-999.F || output[c].back()!=-999.F) return 5;
                checksum+=out[c][frames-1];
            }
            if(i>=warmup) {
                const auto us=std::chrono::duration<double,std::micro>(end-start).count(); durations.push_back(us);
                std::fprintf(raw,"%s,%u,%u,%u,%u,%u,%.6f\n",shape.c_str(),branches,pdc,outputs,frames,i-warmup,us);
            }
        }
        std::sort(durations.begin(),durations.end()); const double deadline=1e6*frames/48000.;
        const auto quantile=[&](double q){return durations[static_cast<std::size_t>(std::ceil(q*durations.size()))-1];};
        const double mean=std::accumulate(durations.begin(),durations.end(),0.)/durations.size();
        const auto over=std::count_if(durations.begin(),durations.end(),[&](double t){return t>deadline;});
        std::fprintf(summary,"%s,%u,%u,%u,%u,48000,%zu,%zu,%u,%zu,%zu,%zu,%zu,%llu,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%zu,%llu,%llu,%llu,%llu,%llu,%u,%u,1,%.3f\n",
            shape.c_str(),branches,pdc,outputs,frames,nodes,tasks,slots,usage.slotBytes,usage.silenceBytes,usage.historyBytes,usage.totalBytes,
            (unsigned long long)latency,mean,quantile(.5),quantile(.95),quantile(.99),durations.back(),deadline,std::size_t(over),
            (unsigned long long)probe::allocations,(unsigned long long)probe::frees,(unsigned long long)probe::mutexes,(unsigned long long)probe::waits,(unsigned long long)probe::logs,
            sw.callbackDiagnostics(),publications,checksum);
        if(probe::allocations||probe::frees||probe::mutexes||probe::waits||probe::logs||sw.callbackDiagnostics()||!std::isfinite(checksum)) return 6;
        require(sw.shutdownAfterBackendStopped());
    }
    std::fclose(raw); std::fclose(summary);
}
