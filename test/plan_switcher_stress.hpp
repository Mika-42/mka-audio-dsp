/**
 * @file plan_switcher_stress.hpp
 * @brief Test fixtures for plan switcher stress.
 * @author Mika
 */
#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <thread>
#include "switcher_backend_fixture.hpp"
#include "plan_switcher_fixture.hpp"

namespace switcher_test {
    /// @brief Aggregates heap and mutex events observed inside measured callback windows.
    struct HeapTotals { std::size_t allocations=0 /**< Number of heap acquisitions recorded inside the measured window. */,
                            deallocations=0 /**< Number of measured releases of non-null allocated storage. */,
                            mutexAttempts=0 /**< Number of direct mutex acquisition attempts inside the measurement window. */; };
    /// @brief Optional measurement callbacks invoked around each stress-test audio execution.
    struct StressHooks {
        /// @brief Optional noexcept hook opening the thread-local measurement window.
        void (*before)() noexcept=nullptr;
        /// @brief Optional noexcept hook closing the window and accumulating its event counts.
        void (*after)(HeapTotals&) noexcept=nullptr;
    };
    /// @brief Summary of generation, output and resource checks from a threaded stress run.
    struct StressResult {
        unsigned swaps=0 /**< Swaps retained by the fixture. */,
            published=0 /**< Number of candidates published to the audio thread. */,
            canceled=0 /**< Number of Pending candidates canceled after backend quiescence. */,
            failedCandidates=0 /**< Number of deliberately refused candidate preparations. */,
            busyRefusals=0 /**< Number of plan transitions refused while the publication state was busy. */,
            stops=0 /**< Number of backend stop/join cycles exercised by the stress run. */,
            edits=0 /**< Number of desired-graph mutations performed during the stress run. */;
        unsigned failures=0 /**< Atomic count of mismatches reported by the audio-side oracle. */,
            errorStep=0 /**< Stress operation index retained to identify the first failure. */,
            created=0 /**< Number of processor instances constructed on control. */,
            destroyed=0 /**< Number of processor instances destroyed on control or during the measured scope. */,
            destroyedOnAudio=0 /**< Number of destructor events recorded inside an audio callback. */;
        std::uint64_t callbacks=0 /**< Callbacks retained by the fixture. */,
            samples=0 /**< Whole-sample latency or sample values retained by the record. */;
        /// @brief Counts of prepared plans grouped by the exercised delay lengths.
        std::array<unsigned,3> delayPlans{};
        /// @brief Aggregated heap and mutex measurements from callback windows.
        HeapTotals heap;
    };
    /// @brief Counts processor lifetime events during the two-thread publication stress.
    struct StressTrace : LifetimeTrace { /** Number of processor instances constructed on control. */ std::atomic<unsigned> created{0}; };
    /// @brief Numeric audio-side observation transferred to the stress-test oracle.
    struct CallbackObservation { GenerationID first=0 /**< Generation observed at the first processing observation in a callback. */,
                                     last=0 /**< Generation observed at the final processing observation in a callback. */; /** Whether a forbidden reentrant adoption attempt was observed inside processing. */ bool midSwap=false; };
    /// @brief Selects each processor's role in the publication stress signal path.
    enum class StressRole {
        /// @brief Produce the fixture's source signal.
        Source,
        /// @brief Consume a logical signal and expose physical output samples.
        Sink,
        /// @brief Provide an additional structural processor for edit/lifetime checks.
        Extra
    };

    /// @brief Renders a stress-test signal and records generation and lifetime observations.
    class StressNode final : public Node {
    public:
        /// @brief Select the processing role and borrow generation and lifetime observation state.
        StressNode(NodeID id,PlanSwitcher& switcher,StressRole role,CallbackObservation& observed,StressTrace& trace)
            : Node(id,role==StressRole::Source ? Config{.layoutOutput={0}} :
                      role==StressRole::Sink ? Config{.layoutInput={0}} : Config{}),
              switcher_(switcher),role_(role),observed_(observed),trace_(trace) { ++trace_.created; }
        /// @brief Record fixture destruction and release owned state on the calling thread.
        ~StressNode() override { ++trace_.destroyed; if (onAudio) ++trace_.destroyedOnAudio; }
        /// @brief Render the selected stress role and record generation, binding and lifetime observations.
        void process(const ProcessContext& ctx) noexcept override {
            if (role_==StressRole::Extra) return;
            const auto generation=switcher_.audioGeneration();
            if (role_==StressRole::Source) {
                observed_.first=generation;
                observed_.midSwap=observed_.midSwap || switcher_.trySwapAtBlockBoundary();
                for (FrameCount f=0;f<ctx.frames;++f) ctx.outputs[0].buffer[f]=ctx.inputs[0].buffer[f];
            } else {
                observed_.last=generation;
                for (FrameCount f=0;f<ctx.frames;++f) {
                    ctx.backendOutputs[0].buffer[f]=ctx.inputs[0].buffer[f];
                    ctx.backendOutputs[1].buffer[f]=static_cast<float>(generation);
                }
            }
        }
    private:
        /// @brief Borrowed switcher used to observe the current audio generation.
        PlanSwitcher& switcher_; /** Immutable processor role selecting this fixture's signal behavior. */ StressRole role_; /** Borrowed numeric observations updated by this processor. */ CallbackObservation& observed_; /** Borrowed lifetime or binding trace; it must outlive the fixture processor. */ StressTrace& trace_;
    };

    /**
     * Two persistent threads/owners; the fake driver may stop/join and restart.
     * Every production callback is bracketed by optional allocation hooks. Its
     * oracle, scheduling yield and diagnostics run outside that measured call.
     */
    inline StressResult runSwitcherStress(unsigned requiredSwaps=10000,const StressHooks& hooks={}) {
        constexpr std::size_t infoCapacity=20100;
        constexpr float guard=-6543.F;
        StressResult result;
        StressTrace trace;
        DAG graph; PlanSwitcher switcher;
        CallbackObservation observed;
        std::array<FrameCount,infoCapacity> delayOf{};
        /// @brief Owns sample buffers and callback views that remain stable for one render call.
        struct Bank {
            /// @brief Owned input sample storage used by the fixture.
            std::array<float,6> input{};
            /// @brief Owned output sample storage or selected output-node identity.
            std::array<std::array<float,6>,2> output{};
            /// @brief Input channel descriptors or physical sample pointers used by this state.
            std::array<const float*,1> inputs{input.data()+1};
            /// @brief Output channel descriptors or physical sample pointers used by this state.
            std::array<float*,2> outputs{output[0].data()+1,output[1].data()+1};
        };
        std::array<Bank,2> banks;
        /// @brief Tracks the expected generation and sample stream across callbacks.
        struct AudioOracle {
            /// @brief Monotonic identity of the prepared graph snapshot.
            GenerationID generation=0;
            /// @brief Persistent sample values retained by the independent stream oracle.
            std::array<float,256> history{};
            /// @brief Current sample cursor in the fixture or independent circular history.
            std::size_t position=0;
            /// @brief Atomic count of mismatches reported by the audio-side oracle.
            std::atomic<unsigned> failures{0};
            std::uint64_t callbacks=0 /**< Callbacks retained by the fixture. */,
                samples=0 /**< Whole-sample latency or sample values retained by the record. */;
            /// @brief Aggregated heap and mutex measurements from callback windows.
            HeapTotals heap;
        } oracle;
        CallbackThread backend;
        const auto source=graph.add<StressNode>(switcher,StressRole::Source,observed,trace);
        const auto sink=graph.add<StressNode>(switcher,StressRole::Sink,observed,trace);
        const auto check=[&](Result status,unsigned step) {
            if (status) return true;
            ++result.failures; result.errorStep=step; return false;
        };
        if (!source || !sink) { result.failures=1; result.errorStep=1; return result; }
        const Connection edge{encode(*source,PortType::Out,0),encode(*sink,PortType::In,0)};
        if (!check(graph.connect(edge.to,edge.from),2) || !check(graph.markAsOutput(*sink),3)) return result;
        std::array delays{ConnectionDelay{edge,0}};
        BackendConfig config{.maxFrames=4,.inputChannels=1,.outputChannels=2,.connectionDelays=delays};
        if (!check(switcher.initialize(graph,config),4)) return result;
        delayOf[1]=0;
        const auto callback=[&] {
            auto& bank=banks[oracle.callbacks%2];
            bank.input.fill(guard);
            for (FrameCount f=0;f<4;++f) bank.input[f+1]=static_cast<float>(oracle.callbacks%31)+f*0.25F;
            for (auto& out : bank.output) out.fill(guard);
            const auto before=bank.input;
            const FrameCount frames=oracle.callbacks%3==0 ? 0U : oracle.callbacks%3==1 ? 1U : 4U;
            const mka::audio::AudioProcessContext io{bank.inputs,bank.outputs,frames,4,4};
            observed={};
            if (hooks.before) hooks.before();
            onAudio=true;
            switcher.exec(io);
            onAudio=false;
            if (hooks.after) hooks.after(oracle.heap);
            const auto generation=switcher.audioGeneration();
            bool valid=generation>0 && generation<infoCapacity;
            valid=valid && observed.first==generation && observed.last==generation && !observed.midSwap;
            if (valid) {
                // delayOf[g] is written once before Pending release, read only
                // after exec's acquire/adoption; no polling of unrelated data.
                const auto delay=delayOf[generation];
                if (generation!=oracle.generation) {
                    oracle.generation=generation; oracle.history.fill(0.F); oracle.position=0;
                }
                for (FrameCount f=0;f<frames;++f) {
                    float expected=bank.input[f+1];
                    if (delay) {
                        expected=oracle.history[oracle.position];
                        oracle.history[oracle.position]=bank.input[f+1];
                        if (++oracle.position==delay) oracle.position=0;
                    }
                    valid=valid && bank.outputs[0][f]==expected && bank.outputs[1][f]==static_cast<float>(generation);
                    ++oracle.samples;
                }
            }
            for (unsigned c=0;c<2;++c) for (FrameCount f=0;f<6;++f)
                if (!(f>0 && f<=frames)) valid=valid && bank.output[c][f]==guard;
            valid=valid && bank.input==before;
            if (!valid) ++oracle.failures;
            ++oracle.callbacks;
            std::this_thread::yield(); // Fake driver scheduling, outside exec and the RT hooks.
        };
        backend.start(callback);
        std::optional<NodeID> desiredExtra;
        const auto edit=[&] {
            if (desiredExtra) {
                const auto status=graph.remove(*desiredExtra);
                if (!check(status,5)) return false;
                desiredExtra.reset();
            } else {
                const auto node=graph.add<StressNode>(switcher,StressRole::Extra,observed,trace);
                if (!node) { ++result.failures; result.errorStep=6; return false; }
                desiredExtra=*node;
            }
            ++result.edits; return true;
        };
        std::uint32_t seed=0x7342ABC1U;
        for (unsigned attempt=0;result.swaps<requiredSwaps && attempt<20000;++attempt) {
            if (oracle.failures.load(std::memory_order_relaxed)!=0) break;
            if (attempt%5==0 && !edit()) break;
            if (attempt%17==0) {
                auto rejected=config; rejected.memoryBudgetBytes=0;
                const auto status=switcher.prepareInactivePlan(graph,rejected);
                if (status || status.error().code!=ErrorCode::MemoryBudgetExceeded) {
                    ++result.failures; result.errorStep=7; break;
                }
                ++result.failedCandidates;
            }
            const bool forceUnacceptedStop=attempt%514==0;
            if (forceUnacceptedStop) backend.stop();
            seed=seed*1664525U+1013904223U;
            const auto delayChoice=seed%3;
            delays[0].samples=delayChoice==0 ? 0 : delayChoice==1 ? 3 : 256;
            if (!check(switcher.prepareInactivePlan(graph,config),8)) break;
            ++result.delayPlans[delayChoice];
            const auto candidate=switcher.readyGeneration();
            if (!candidate || *candidate>=infoCapacity) { ++result.failures; result.errorStep=9; break; }
            delayOf[*candidate]=static_cast<FrameCount>(delays[0].samples);
            if (!check(switcher.publishReadyPlan(),10)) break;
            ++result.published;
            const auto duplicate=switcher.publishReadyPlan();
            const auto reprepare=switcher.prepareInactivePlan(graph,config);
            if (duplicate || reprepare || duplicate.error().code!=ErrorCode::PlanBusy || reprepare.error().code!=ErrorCode::PlanBusy) {
                ++result.failures; result.errorStep=11; break;
            }
            result.busyRefusals+=2;
            if (attempt%7==0 && !edit()) break; // Edits after publication never mutate B.
            if (attempt%257==0) {
                backend.stop();
                if (!check(switcher.stopAfterBackendStopped(),12)) break;
                if (switcher.controlGeneration()==candidate) ++result.swaps;
                else ++result.canceled;
                ++result.stops;
                if (!check(switcher.resumeAfterBackendStopped(),13)) break;
                backend.start(callback);
            } else {
                const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(10);
                while (switcher.handshakeState()!=HandshakeState::Acknowledged &&
                       std::chrono::steady_clock::now()<deadline) std::this_thread::yield();
                if (!check(switcher.retireAcknowledgedPlan(),14)) break;
                ++result.swaps;
            }
            (void)graph.collectRetiredNodes();
        }
        backend.stop();
        if (!check(switcher.shutdownAfterBackendStopped(),15)) return result;
        (void)graph.collectRetiredNodes();
        if (desiredExtra) (void)check(graph.remove(*desiredExtra),16);
        (void)check(graph.remove(*source),17); (void)check(graph.remove(*sink),18);
        (void)graph.collectRetiredNodes();
        result.failures+=oracle.failures.load(std::memory_order_relaxed);
        result.callbacks=oracle.callbacks; result.samples=oracle.samples; result.heap=oracle.heap;
        result.created=trace.created.load(); result.destroyed=trace.destroyed.load();
        result.destroyedOnAudio=trace.destroyedOnAudio.load();
        if (result.swaps!=requiredSwaps || result.created!=result.destroyed || result.destroyedOnAudio!=0) ++result.failures;
        return result;
    }
}
