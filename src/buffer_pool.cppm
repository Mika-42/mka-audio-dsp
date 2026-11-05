/**
 * @file buffer_pool.cppm
 * @brief Prepared mono sample slots and immutable silence storage.
 * @author Mika
 */
module;
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>

export module mka.audio.dsp.buffer_pool;
export import mka.audio.dsp.types;
export import mka.audio.dsp.memory_budget;

export namespace mka::audio::dsp {
    /// A mono temporary slot, unrelated to logical channels or backend identity.
    using BufferId = std::uint32_t;

    /**
     * @brief Plan-owned contiguous float32 slots, prepared only on control.
     * @details Nodes borrow samples, never ownership. A stereo signal uses two
     * IDs. Validated IDs address maxFrames() floats. No acquire/release occurs
     * in audio. Prepare/copy/destroy only after quiescence; copying allocates an
     * independent pool and requires rebinding descriptors. Never move a bound
     * pool. Preparation commits only after all allocations succeed.
     */
    class BufferPool final {
    public:
        /// @brief Construct an empty pool without allocating sample storage.
        BufferPool() noexcept = default;
        /// @brief Copy slot and silence samples into independent storage; callers must rebind borrowed views.
        BufferPool(const BufferPool&) = default;
        /// @brief Disable copying to preserve stable ownership and borrowed addresses.
        BufferPool& operator=(const BufferPool&) = delete;
        /// @brief Disable moving to preserve stable ownership and borrowed addresses.
        BufferPool(BufferPool&&) = delete;
        /// @brief Disable moving to preserve stable ownership and borrowed addresses.
        BufferPool& operator=(BufferPool&&) = delete;

        /// Requested slots + silence bytes, excluding allocator bookkeeping/owner object.
        [[nodiscard]] static std::expected<std::size_t, CompileError>
        storageBytes(const std::uint32_t slots, const FrameCount frames) noexcept {
            const auto usage = measurePlanMemory({.slotCount = slots, .maxFrames = frames});
            if (!usage) return std::unexpected(usage.error());
            return usage->totalBytes;
        }

        /** Prepare exact slot storage with a strong failure guarantee, off RT. */
        [[nodiscard]] std::expected<void, CompileError> prepare(
            const std::uint32_t slots, const FrameCount frames,
            const std::size_t budgetBytes = std::numeric_limits<std::size_t>::max()) try {
            const auto bytes = storageBytes(slots, frames);
            if (!bytes) return std::unexpected(bytes.error());
            const auto samples = static_cast<std::size_t>(slots) * frames;
            if (samples > storage_.max_size() || frames > silence_.max_size())
                return std::unexpected(CompileError{ErrorCode::CapacityExceeded});
            if (*bytes > budgetBytes)
                return std::unexpected(CompileError{ErrorCode::MemoryBudgetExceeded});
            std::vector<float> candidate(samples, 0.F);
            std::vector<float> silence(frames, 0.F);
            storage_.swap(candidate);
            silence_.swap(silence);
            slots_ = slots;
            frames_ = frames;
            return {};
        } catch (const std::bad_alloc&) {
            return std::unexpected(CompileError{ErrorCode::PrepareFailed});
        } catch (const std::length_error&) {
            return std::unexpected(CompileError{ErrorCode::CapacityExceeded});
        }

        /// Borrow writable samples; id has been validated on control. Zero frames: null.
        [[nodiscard]] float* buffer(const BufferId id) noexcept {
            assert(id < slots_);
            return frames_ == 0 ? nullptr : storage_.data() + static_cast<std::size_t>(id) * frames_;
        }
        /// Borrow samples for a connected In/Aux, never ownership.
        [[nodiscard]] const float* buffer(const BufferId id) const noexcept {
            assert(id < slots_);
            return frames_ == 0 ? nullptr : storage_.data() + static_cast<std::size_t>(id) * frames_;
        }
        /// @brief Return the number of prepared mono slots without accessing their samples.
        [[nodiscard]] std::uint32_t slotCapacity() const noexcept { return slots_; }
        /// @brief Return the prepared per-channel sample capacity.
        [[nodiscard]] FrameCount maxFrames() const noexcept { return frames_; }
        /// Separate immutable zero storage: no BufferId, writable view or recycling.
        [[nodiscard]] const float* silence() const noexcept {
            return frames_ == 0 ? nullptr : silence_.data();
        }
        /// @brief Return the retained slot and silence payload in bytes.
        [[nodiscard]] std::size_t allocatedBytes() const noexcept {
            return (storage_.size() + silence_.size()) * sizeof(float);
        }

    private:
        /// @brief Owned contiguous temporary mono-slot samples.
        std::vector<float> storage_;
        /// @brief Owned zero samples, kept outside the reusable slot pool.
        std::vector<float> silence_;
        /// @brief Number of prepared temporary mono sample slots.
        std::uint32_t slots_ = 0;
        /// @brief Prepared per-channel sample capacity.
        FrameCount frames_ = 0;
    };
}
