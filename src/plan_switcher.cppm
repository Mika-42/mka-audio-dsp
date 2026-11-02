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
            if (!gate_.is_lock_free()) return std::unexpected(Error{ErrorCode::PrepareFailed});
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
            const auto index=static_cast<std::uint8_t>(1-controlActive_);
            if (slots_[index].role!=PlanSlotState::Free)
                return std::unexpected(Error{ErrorCode::PlanBusy});
            return prepareSlot(index,graph,backend,limits);
        }

        /// Abandon an unpublished Ready plan on control. Never touches the active plan.
        [[nodiscard]] Result discardReadyPlan() noexcept {
            if (!initialized_) return std::unexpected(Error{ErrorCode::PrepareFailed});
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

        /** One bounded admission attempt; callbacks never acquire ownership or wait. */
        void exec(const mka::audio::AudioProcessContext& io) noexcept {
            auto expected=CallbackGate::Ready;
            if (!gate_.compare_exchange_strong(expected,CallbackGate::Executing,
                    std::memory_order_acquire,std::memory_order_relaxed)) {
                silenceRejected(io);
                return;
            }
            slots_[audioActive_].plan.exec(io);
            gate_.store(CallbackGate::Ready,std::memory_order_release);
        }

    private:
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
        // Each index has one writer. No control method reads audioActive_ while audio runs.
        std::uint8_t controlActive_=0,audioActive_=0;
        GenerationID generationCounter_=0;
        bool initialized_=false;
    };
}
