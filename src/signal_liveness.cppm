/**
 * @file signal_liveness.cppm
 * @brief Signal read/write lifetimes and reusable sample-slot assignments.
 * @author Mika
 */
module;
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <new>
#include <span>
#include <stdexcept>
#include <vector>

export module mka.audio.dsp.signal_liveness;
export import mka.audio.dsp.buffer_pool;

export namespace mka::audio::dsp {
    /// Dense control-side production identity; not a public ChannelID or history ID.
    using SignalID = std::size_t;
    /// @brief Distinguishes DSP calls, compensation delays and signal readback operations.
    enum class OperationKind {
        /// @brief Invoke a prepared DSP processor.
        Node,
        /// @brief Process a persistent logical-edge compensation history.
        Delay,
        /// @brief Retain a logical signal until the scheduled observation has consumed it.
        Readback
    };

    /** Final serial operation after delay insertion; lists every actual read/write. */
    struct SignalOperation {
        /// @brief Operation or fixture behavior selected for this record.
        OperationKind kind = OperationKind::Node;
        /// @brief Dense logical signals consumed by the operation.
        std::vector<SignalID> reads;
        /// @brief Dense logical signals produced by the operation.
        std::vector<SignalID> writes;
    };

    /// Inclusive interval; a production with no reader expires after its producer.
    struct SignalLifetime {
        /// @brief Operation index where this logical signal is written.
        std::size_t producedAt = 0;
        /// @brief Inclusive operation index of this signal's final reader.
        std::size_t lastUse = 0;
        /// @brief Compare all value fields for equality.
        bool operator==(const SignalLifetime&) const noexcept = default;
    };

    /// @brief Selects lifetime-based slot reuse or distinct slots for diagnostics.
    enum class SlotStrategy {
        /// @brief Recycle slots after the previous signal's final inclusive reader.
        Reuse,
        /// @brief Assign separate slots to signals for diagnostic comparison.
        Unique
    };

    /** Frozen control result; peakSlots is the physical capacity to prepare. */
    struct SlotAssignment {
        /// @brief Temporary slot assigned to each dense logical signal.
        std::vector<BufferId> slotOf;
        /// @brief Number of physical sample slots retained by this assignment.
        std::uint32_t peakSlots = 0;
        /// @brief Maximum number of simultaneously live logical signals.
        std::size_t peakLiveSlots = 0;
    };

    /**
     * @brief Compute exact lifetimes on the final Node/Delay/readback sequence.
     * @details Each dense ID has exactly one producer. Readers (In, Aux, fan-out,
     * delay inputs, terminal exports) must follow that producer. A readback is an
     * explicit consumption after processing, not an assumed backend reservation.
     * Persistent delay histories and silence have no SignalID. Control only;
     * nothing in this temporary table is inspected or changed in audio.
     */
    [[nodiscard]] std::expected<std::vector<SignalLifetime>, CompileError>
    calculateLifetimes(const std::span<const SignalOperation> operations,
                       const std::size_t signalCount) try {
        if (signalCount > std::vector<SignalLifetime>{}.max_size())
            return std::unexpected(CompileError{ErrorCode::CapacityExceeded});
        constexpr auto absent = std::numeric_limits<std::size_t>::max();
        std::vector<SignalLifetime> result(signalCount, {absent, absent});
        for (std::size_t i = 0; i < operations.size(); ++i) {
            for (const auto id : operations[i].reads) {
                if (id >= signalCount || result[id].producedAt == absent)
                    return std::unexpected(CompileError{ErrorCode::InvalidParameter});
                result[id].lastUse = i;
            }
            for (const auto id : operations[i].writes) {
                if (id >= signalCount || result[id].producedAt != absent)
                    return std::unexpected(CompileError{ErrorCode::InvalidParameter});
                result[id] = {i, i};
            }
        }
        if (std::ranges::any_of(result, [](auto life) { return life.producedAt == absent; }))
            return std::unexpected(CompileError{ErrorCode::InvalidParameter});
        return result;
    } catch (const std::bad_alloc&) {
        return std::unexpected(CompileError{ErrorCode::PrepareFailed});
    } catch (const std::length_error&) {
        return std::unexpected(CompileError{ErrorCode::CapacityExceeded});
    }

    /**
     * @brief Cross-check every mono Delay against final lifetimes and physical slots.
     * @details Control only, no allocation. A Delay has one already produced
     * input and one new output. The source must remain live through this exact
     * operation; the output starts here and has its own slot, even when this
     * is the source's last reader. Other later fan-out readers are permitted.
     * Global interval validation remains the responsibility of slot assignment.
     * Histories and silence are not productions and cannot enter this table.
     */
    [[nodiscard]] std::expected<void,CompileError> validateDelayLifetimes(
        const std::span<const SignalOperation> operations,
        const std::span<const SignalLifetime> lifetimes,const SlotAssignment& slots) noexcept {
        if (slots.slotOf.size()!=lifetimes.size())
            return std::unexpected(CompileError{ErrorCode::InvalidParameter});
        for (std::size_t i=0;i<operations.size();++i) {
            const auto& operation=operations[i];
            if (operation.kind!=OperationKind::Delay) continue;
            if (operation.reads.size()!=1 || operation.writes.size()!=1)
                return std::unexpected(CompileError{ErrorCode::InvalidParameter});
            const auto source=operation.reads[0],output=operation.writes[0];
            if (source>=lifetimes.size() || output>=lifetimes.size())
                return std::unexpected(CompileError{ErrorCode::InvalidParameter});
            if (lifetimes[source].producedAt>=i || lifetimes[source].lastUse<i ||
                lifetimes[output].producedAt!=i || lifetimes[output].lastUse<i ||
                slots.slotOf[source]>=slots.peakSlots || slots.slotOf[output]>=slots.peakSlots ||
                slots.slotOf[source]==slots.slotOf[output])
                return std::unexpected(CompileError{ErrorCode::InvalidParameter});
        }
        return {};
    }

    /**
     * @brief Verify the non-aliasing proof before binding any writable storage.
     * @details Includes logical outputs of marked backend writers: those are
     * ordinary productions, even with successors. Delay outputs are separate
     * productions from their source, so their inclusive lifetimes cannot alias.
     * Control only. Reject a malformed assignment rather than publish a plan B.
     */
    [[nodiscard]] std::expected<void, CompileError> validateSlotAssignment(
        const std::span<const SignalLifetime> lifetimes, const SlotAssignment& slots) try {
        if (slots.slotOf.size() != lifetimes.size())
            return std::unexpected(CompileError{ErrorCode::InvalidParameter});
        std::vector<SignalID> ordered;
        ordered.reserve(lifetimes.size());
        for (SignalID s = 0; s < lifetimes.size(); ++s) {
            if (lifetimes[s].producedAt > lifetimes[s].lastUse || slots.slotOf[s] >= slots.peakSlots)
                return std::unexpected(CompileError{ErrorCode::InvalidParameter});
            ordered.push_back(s);
        }
        std::ranges::sort(ordered, [&](SignalID a, SignalID b) {
            if (slots.slotOf[a] != slots.slotOf[b]) return slots.slotOf[a] < slots.slotOf[b];
            return lifetimes[a].producedAt < lifetimes[b].producedAt;
        });
        for (std::size_t i = 1; i < ordered.size(); ++i) {
            const auto a = ordered[i-1], b = ordered[i];
            if (slots.slotOf[a] == slots.slotOf[b] && lifetimes[a].lastUse >= lifetimes[b].producedAt)
                return std::unexpected(CompileError{ErrorCode::InvalidParameter});
        }
        return {};
    } catch (const std::bad_alloc&) {
        return std::unexpected(CompileError{ErrorCode::PrepareFailed});
    } catch (const std::length_error&) {
        return std::unexpected(CompileError{ErrorCode::CapacityExceeded});
    }

    /**
     * @brief Assign slots before producers and free only AFTER their last reader.
     * @details Inclusive intervals prohibit implicit in-place processing. Unique
     * allocates one slot per production for reference renders. Free lists and
     * lifetime buckets are temporary control data, never published to audio.
     * Silence and persistent delay histories cannot appear in this assignment.
     */
    [[nodiscard]] std::expected<SlotAssignment, CompileError> assignBufferSlots(
        const std::span<const SignalLifetime> lifetimes, const std::size_t operationCount,
        const SlotStrategy strategy = SlotStrategy::Reuse,
        const std::uint32_t capacity = std::numeric_limits<std::uint32_t>::max()) try {
        if (operationCount > std::vector<std::vector<SignalID>>{}.max_size() ||
            lifetimes.size() > std::vector<BufferId>{}.max_size())
            return std::unexpected(CompileError{ErrorCode::CapacityExceeded});
        std::vector<std::vector<SignalID>> starts(operationCount), ends(operationCount);
        for (SignalID s = 0; s < lifetimes.size(); ++s) {
            const auto life = lifetimes[s];
            if (life.producedAt > life.lastUse || life.lastUse >= operationCount)
                return std::unexpected(CompileError{ErrorCode::InvalidParameter});
            starts[life.producedAt].push_back(s);
            ends[life.lastUse].push_back(s);
        }
        SlotAssignment result;
        result.slotOf.resize(lifetimes.size());
        std::vector<BufferId> freeSlots;
        freeSlots.reserve(lifetimes.size());
        std::size_t liveSlots = 0;
        for (std::size_t i = 0; i < operationCount; ++i) {
            for (const auto s : starts[i]) {
                BufferId slot;
                if (strategy == SlotStrategy::Reuse && !freeSlots.empty()) {
                    slot = freeSlots.back();
                    freeSlots.pop_back();
                } else {
                    if (result.peakSlots == capacity)
                        return std::unexpected(CompileError{ErrorCode::CapacityExceeded});
                    slot = result.peakSlots++;
                }
                result.slotOf[s] = slot;
                ++liveSlots;
            }
            result.peakLiveSlots = std::max(result.peakLiveSlots, liveSlots);
            // In particular, a consumer's new output cannot reuse its last input.
            for (const auto s : ends[i]) {
                if (strategy == SlotStrategy::Reuse) freeSlots.push_back(result.slotOf[s]);
                --liveSlots;
            }
        }
        if (const auto valid = validateSlotAssignment(lifetimes, result); !valid)
            return std::unexpected(valid.error());
        return result;
    } catch (const std::bad_alloc&) {
        return std::unexpected(CompileError{ErrorCode::PrepareFailed});
    } catch (const std::length_error&) {
        return std::unexpected(CompileError{ErrorCode::CapacityExceeded});
    }
}
