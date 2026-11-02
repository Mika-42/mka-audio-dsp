module;
#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <optional>
#include <utility>

export module mka.audio.dsp.plan_switcher;
export import mka.audio.dsp.dag;

export namespace mka::audio::dsp {
    using GenerationID = std::uint64_t;
    enum class HandshakeState : std::uint8_t { Idle, Pending, Acknowledged };
    /// Control-side slot roles; separate from the publication handshake.
    enum class PlanSlotState : std::uint8_t { Free, Ready, Pending, Active, Retired };

    /**
     * @brief Two persistent plans owned/retired on control, borrowed by one audio thread.
     * @details All control methods are externally serialized. The DAG and Node
     * owners outlive this switcher. Preparation only reads immutable Node Config;
     * it never invokes a DSP prepare/process hook on a Node shared with audio.
     * Slot object addresses never change. Transfer into a Free slot happens on
     * control before publication; ExecutionPlan's heap AudioState does not move.
     * Stop/join the backend before destruction; no future callback may enter.
     */
    class PlanSwitcher final {
        enum class CallbackGate : std::uint8_t { Stopped, Ready, Executing };
        static_assert(std::atomic<CallbackGate>::is_always_lock_free);
        static_assert(std::atomic<HandshakeState>::is_always_lock_free);
        struct Slot {
            ExecutionPlan plan;
            GenerationID generation=0;
            PlanSlotState role=PlanSlotState::Free;
        };

    public:
        PlanSwitcher() noexcept = default;
        PlanSwitcher(const PlanSwitcher&)=delete;
        PlanSwitcher& operator=(const PlanSwitcher&)=delete;
        PlanSwitcher(PlanSwitcher&&)=delete;
        PlanSwitcher& operator=(PlanSwitcher&&)=delete;
        ~PlanSwitcher() { assert(gate_.load(std::memory_order_relaxed)!=CallbackGate::Executing); }

        /** Install the initial generation before starting the backend. Control only. */
        [[nodiscard]] Result initialize(const DAG& graph,const BackendConfig& backend,
                                        const CompileConfig& limits={}) {
            if (initialized_) return std::unexpected(Error{ErrorCode::PlanBusy});
            if (!lockFreeAtomics()) return std::unexpected(Error{ErrorCode::PrepareFailed});
            if (auto result=prepareSlot(0,graph,backend,limits); !result) return result;
            controlActive_=audioActive_=0;
            slots_[0].role=PlanSlotState::Active;
            initialized_=true;
            gate_.store(CallbackGate::Ready,std::memory_order_release);
            return {};
        }

        /** Compile only into a genuinely Free inactive slot, transactionally. */
        [[nodiscard]] Result prepareInactivePlan(const DAG& graph,const BackendConfig& backend,
                                                const CompileConfig& limits={}) {
            if (!initialized_) return std::unexpected(Error{ErrorCode::PrepareFailed});
            if (state_.load(std::memory_order_acquire)!=HandshakeState::Idle)
                return std::unexpected(Error{ErrorCode::PlanBusy});
            const auto index=static_cast<std::uint8_t>(1-controlActive_);
            if (slots_[index].role!=PlanSlotState::Free)
                return std::unexpected(Error{ErrorCode::PlanBusy});
            return prepareSlot(index,graph,backend,limits);
        }

        /// Abandon an unpublished Ready plan on control. Never touches the active plan.
        [[nodiscard]] Result discardReadyPlan() noexcept {
            if (!initialized_) return std::unexpected(Error{ErrorCode::PrepareFailed});
            if (state_.load(std::memory_order_acquire)!=HandshakeState::Idle)
                return std::unexpected(Error{ErrorCode::PlanBusy});
            auto& slot=slots_[1-controlActive_];
            if (slot.role!=PlanSlotState::Ready) return std::unexpected(Error{ErrorCode::PlanBusy});
            clearSlot(slot);
            return {};
        }

        /** Control-only Ready view. Its borrow ends before publish/discard. */
        [[nodiscard]] const ExecutionPlan* readyPlan() const noexcept {
            if (!initialized_) return nullptr;
            const auto& slot=slots_[1-controlActive_];
            return slot.role==PlanSlotState::Ready ? &slot.plan : nullptr;
        }
        /// Stable identity only. Inspect contents exclusively with the appropriate ownership.
        [[nodiscard]] const ExecutionPlan* slotAddress(const unsigned index) const noexcept {
            assert(index<2); return &slots_[index].plan;
        }
        /// Control-owned role snapshot; audio never writes these fields.
        [[nodiscard]] PlanSlotState slotState(const unsigned index) const noexcept {
            assert(index<2); return slots_[index].role;
        }
        /// Audio-thread-only generation tag, including inside process().
        [[nodiscard]] GenerationID audioGeneration() const noexcept {
            return slots_[audioActive_].generation;
        }
        [[nodiscard]] std::optional<GenerationID> readyGeneration() const noexcept {
            if (!readyPlan()) return std::nullopt;
            return slots_[1-controlActive_].generation;
        }

        [[nodiscard]] bool lockFreeAtomics() const noexcept {
            return gate_.is_lock_free() && state_.is_lock_free() && diagnostics_.is_lock_free();
        }
        /// Sticky aggregate survives slot retirement; safe control observation during callbacks.
        [[nodiscard]] std::uint32_t callbackDiagnostics() const noexcept {
            return diagnostics_.load(std::memory_order_relaxed);
        }
        /// Drain numeric events on control. Concurrent later events remain for the next drain.
        [[nodiscard]] std::uint32_t takeCallbackDiagnostics() noexcept {
            return diagnostics_.exchange(0,std::memory_order_relaxed);
        }
        /// State alone is atomic; do not infer an independent active-index snapshot.
        [[nodiscard]] HandshakeState handshakeState() const noexcept {
            return state_.load(std::memory_order_acquire);
        }
        /** Coherent control observation. Pending deliberately has no active answer. */
        [[nodiscard]] std::optional<GenerationID> controlGeneration() const noexcept {
            if (!initialized_) return std::nullopt;
            const auto state=state_.load(std::memory_order_acquire);
            if (state==HandshakeState::Pending) return std::nullopt;
            return slots_[state==HandshakeState::Acknowledged ? pendingIndex_ : controlActive_].generation;
        }

        /** Publish fully linked B. Single control writer; never overwrite a Pending. */
        [[nodiscard]] Result publishReadyPlan() noexcept {
            if (!initialized_) return std::unexpected(Error{ErrorCode::PrepareFailed});
            if (gate_.load(std::memory_order_acquire)==CallbackGate::Stopped)
                return std::unexpected(Error{ErrorCode::PrepareFailed});
            if (state_.load(std::memory_order_acquire)!=HandshakeState::Idle)
                return std::unexpected(Error{ErrorCode::PlanBusy});
            const auto index=static_cast<std::uint8_t>(1-controlActive_);
            if (slots_[index].role!=PlanSlotState::Ready || !slots_[index].plan.isBackendPrepared())
                return std::unexpected(Error{ErrorCode::PrepareFailed});
            pendingIndex_=index;
            slots_[index].role=PlanSlotState::Pending;
            state_.store(HandshakeState::Pending,std::memory_order_release);
            return {};
        }

        /**
         * @brief Compile a coherent desired graph and publish it on control.
         * @details The caller serializes graph edits. A busy switcher rejects
         * before compilation; desired edits remain in DAG for the next attempt.
         * Compilation never mutates active/shared DSP state or consumes edits.
         * For a latency change, prepare a distinct immutable DSP Node on control,
         * edit desired edges, then compile/publish B. A keeps its old Node until
         * ACK. Reusing a shared Node requires the frozen latency/rate contract;
         * unsafe preparation must be refused, never performed from process.
         */
        [[nodiscard]] Result prepareAndPublish(const DAG& graph,const BackendConfig& backend,
                                               const CompileConfig& limits={}) {
            if (gate_.load(std::memory_order_acquire)==CallbackGate::Stopped)
                return std::unexpected(Error{ErrorCode::PrepareFailed});
            if (auto result=prepareInactivePlan(graph,backend,limits); !result) return result;
            return publishReadyPlan();
        }

        /** Audio only, at a block boundary. A call from inside process returns false. */
        [[nodiscard]] bool trySwapAtBlockBoundary() noexcept {
            auto expected=CallbackGate::Ready;
            if (!gate_.compare_exchange_strong(expected,CallbackGate::Executing,
                    std::memory_order_acquire,std::memory_order_relaxed)) return false;
            const bool swapped=adoptPending();
            gate_.store(CallbackGate::Ready,std::memory_order_release);
            return swapped;
        }

        /** Acquire ACK before reclaiming A. This and all destruction run on control. */
        [[nodiscard]] Result retireAcknowledgedPlan() noexcept {
            if (state_.load(std::memory_order_acquire)!=HandshakeState::Acknowledged)
                return std::unexpected(Error{ErrorCode::PlanBusy});
            const auto old=controlActive_;
            slots_[old].role=PlanSlotState::Retired;
            controlActive_=pendingIndex_;
            slots_[controlActive_].role=PlanSlotState::Active;
            clearSlot(slots_[old]);
            state_.store(HandshakeState::Idle,std::memory_order_release);
            return {};
        }

        /**
         * @brief Resolve publication AFTER the backend has stopped and joined.
         * @pre The driver guarantees no callback is in flight and no future
         * callback may enter until resume/restart. An idle flag alone is NOT
         * such a proof. The admission check detects an outstanding callback,
         * but cannot prove that an external driver will not call again.
         * @details Retain the last adopted active generation for resume. Cancel
         * unaccepted Pending, discard Ready, reclaim an ACKed predecessor, all
         * on control. No waiting for an ACK that can no longer arrive.
         */
        [[nodiscard]] Result stopAfterBackendStopped() noexcept {
            auto expected=CallbackGate::Ready;
            if (!gate_.compare_exchange_strong(expected,CallbackGate::Stopped,
                    std::memory_order_acq_rel,std::memory_order_acquire) && expected!=CallbackGate::Stopped)
                return std::unexpected(Error{ErrorCode::PlanBusy});
            if (!initialized_) return {};
            const auto state=state_.load(std::memory_order_acquire);
            if (state==HandshakeState::Acknowledged) {
                const auto result=retireAcknowledgedPlan();
                assert(result.has_value());
                (void)result;
            } else if (state==HandshakeState::Pending) {
                // The closed gate + external join prove this B was never adopted.
                clearSlot(slots_[pendingIndex_]);
                state_.store(HandshakeState::Idle,std::memory_order_release);
            }
            auto& inactive=slots_[1-controlActive_];
            if (inactive.role==PlanSlotState::Ready) clearSlot(inactive);
            audioActive_=controlActive_; // Legal only under backend quiescence.
            return {};
        }

        /** Reopen admission before restarting a stopped backend. Control only. */
        [[nodiscard]] Result resumeAfterBackendStopped() noexcept {
            if (!initialized_) return std::unexpected(Error{ErrorCode::PrepareFailed});
            if (gate_.load(std::memory_order_acquire)!=CallbackGate::Stopped ||
                state_.load(std::memory_order_acquire)!=HandshakeState::Idle)
                return std::unexpected(Error{ErrorCode::PlanBusy});
            audioActive_=controlActive_;
            gate_.store(CallbackGate::Ready,std::memory_order_release);
            return {};
        }

        /** Release all generations on control after the same backend stop/join proof. */
        [[nodiscard]] Result shutdownAfterBackendStopped() noexcept {
            if (auto result=stopAfterBackendStopped(); !result) return result;
            for (auto& slot : slots_) clearSlot(slot);
            initialized_=false;
            controlActive_=audioActive_=0;
            state_.store(HandshakeState::Idle,std::memory_order_release);
            return {};
        }

        /** One bounded admission attempt; callbacks never acquire ownership or wait. */
        void exec(const mka::audio::AudioProcessContext& io) noexcept {
            auto expected=CallbackGate::Ready;
            if (!gate_.compare_exchange_strong(expected,CallbackGate::Executing,
                    std::memory_order_acquire,std::memory_order_relaxed)) {
                diagnostics_.fetch_or(static_cast<std::uint32_t>(CallbackDiagnostic::CallbackAdmissionRejected),std::memory_order_relaxed);
                silenceRejected(io);
                return;
            }
            (void)adoptPending(); // Exactly once before selecting any processing task.
            slots_[audioActive_].plan.exec(io);
            const auto flags=slots_[audioActive_].plan.takeCallbackDiagnostics();
            if (flags) diagnostics_.fetch_or(flags,std::memory_order_relaxed);
            gate_.store(CallbackGate::Ready,std::memory_order_release);
        }

    private:
        bool adoptPending() noexcept {
            if (state_.load(std::memory_order_acquire)!=HandshakeState::Pending) return false;
            audioActive_=pendingIndex_;
            // No reference to the old slot survives this store. Control may
            // immediately reclaim it while audio executes the new generation.
            state_.store(HandshakeState::Acknowledged,std::memory_order_release);
            return true;
        }
        [[nodiscard]] Result prepareSlot(const std::uint8_t index,const DAG& graph,
                                         const BackendConfig& backend,const CompileConfig& limits) {
            if (generationCounter_==std::numeric_limits<GenerationID>::max())
                return std::unexpected(Error{ErrorCode::CapacityExceeded});
            ExecutionPlan candidate;
            if (auto result=graph.compile(candidate,limits); !result) return result;
            if (auto result=candidate.prepareBackend(backend,graph.connections(),graph.outputNodes()); !result) return result;
            auto& slot=slots_[index];
            slot.plan=std::move(candidate);
            slot.generation=++generationCounter_;
            slot.role=PlanSlotState::Ready;
            return {};
        }
        static void clearSlot(Slot& slot) noexcept {
            slot.plan=ExecutionPlan{};
            slot.generation=0;
            slot.role=PlanSlotState::Free;
        }
        static void silenceRejected(const mka::audio::AudioProcessContext& io) noexcept {
            const auto frames=std::min(io.frames,io.outputCapacityFrames);
            if (frames) for (auto* output : io.outputs) if (output) std::fill_n(output,frames,0.F);
        }

        std::array<Slot,2> slots_;
        std::atomic<CallbackGate> gate_{CallbackGate::Stopped};
        std::atomic<HandshakeState> state_{HandshakeState::Idle};
        std::atomic<std::uint32_t> diagnostics_{0};
        // Each index has one writer. No control method reads audioActive_ while audio runs.
        std::uint8_t controlActive_=0,audioActive_=0;
        // Published via Pending release; read by audio only before its ACK release.
        std::uint8_t pendingIndex_=1;
        GenerationID generationCounter_=0;
        bool initialized_=false;
    };
}
