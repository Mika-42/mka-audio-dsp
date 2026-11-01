module;
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <span>

export module mka.audio.dsp.memory_budget;
export import mka.audio.dsp.types;

export namespace mka::audio::dsp {
    /// Checked conversion and multiplication before any size_t arithmetic.
    [[nodiscard]] constexpr std::expected<std::size_t, CompileError>
    checkedSizeMultiply(const std::uint64_t a, const std::uint64_t b) noexcept {
        constexpr auto maximum = std::numeric_limits<std::size_t>::max();
        if (a > maximum || b > maximum || (b != 0 && a > maximum / b))
            return std::unexpected(CompileError{ErrorCode::ArithmeticOverflow});
        return static_cast<std::size_t>(a) * static_cast<std::size_t>(b);
    }

    [[nodiscard]] constexpr std::expected<std::size_t, CompileError>
    checkedSizeAdd(const std::size_t a, const std::size_t b) noexcept {
        if (a > std::numeric_limits<std::size_t>::max() - b)
            return std::unexpected(CompileError{ErrorCode::ArithmeticOverflow});
        return a + b;
    }

    /// Requested retained array payload and deterministic alignment allowance.
    struct MemoryArray {
        std::uint64_t count = 0;
        std::uint64_t elementBytes = 0;
        std::size_t alignment = 1;
    };

    struct PlanMemoryRequest {
        std::uint64_t slotCount = 0;
        std::uint64_t maxFrames = 0;
        std::span<const std::uint64_t> delaySamples;
        std::span<const MemoryArray> metadata;
    };

    /** Requested retained storage; excludes allocator bookkeeping and control temporaries. */
    struct PlanMemoryUsage {
        std::size_t slotBytes = 0;
        std::size_t silenceBytes = 0;
        std::size_t historyBytes = 0;
        std::size_t metadataBytes = 0;
        std::size_t paddingBytes = 0;
        std::size_t totalBytes = 0;
    };

    /**
     * @brief Account for all retained payload before sample/history allocation.
     * @details Slot/frame domains are checked separately from arithmetic. Budget
     * is inclusive and comes from compilation config; default adds no ceiling.
     * Counts PCM slots, separate silence, every persistent mono history, measured
     * metadata arrays and alignment padding in that order. No heap, RT budget
     * work or state mutation; allocation failures are translated at prepare.
     */
    [[nodiscard]] std::expected<PlanMemoryUsage, CompileError> measurePlanMemory(
        const PlanMemoryRequest& request,
        const std::size_t budgetBytes = std::numeric_limits<std::size_t>::max()) noexcept {
        if (request.slotCount > std::numeric_limits<std::uint32_t>::max() ||
            request.maxFrames > std::numeric_limits<FrameCount>::max())
            return std::unexpected(CompileError{ErrorCode::CapacityExceeded});
        const auto samples = checkedSizeMultiply(request.slotCount, request.maxFrames);
        if (!samples) return std::unexpected(samples.error());
        const auto slots = checkedSizeMultiply(*samples, sizeof(float));
        const auto silence = checkedSizeMultiply(request.maxFrames, sizeof(float));
        if (!slots) return std::unexpected(slots.error());
        if (!silence) return std::unexpected(silence.error());
        PlanMemoryUsage result;
        result.slotBytes = *slots;
        result.silenceBytes = *silence;
        const auto append = [&](std::size_t increment) noexcept -> Result {
            const auto total = checkedSizeAdd(result.totalBytes, increment);
            if (!total) return std::unexpected(total.error());
            result.totalBytes = *total;
            return {};
        };
        if (auto status = append(*slots); !status) return std::unexpected(status.error());
        if (auto status = append(*silence); !status) return std::unexpected(status.error());
        for (const auto count : request.delaySamples) {
            const auto bytes = checkedSizeMultiply(count, sizeof(float));
            if (!bytes) return std::unexpected(bytes.error());
            if (auto status = append(*bytes); !status) return std::unexpected(status.error());
            // historyBytes <= totalBytes, so the checked total proves this sum fits.
            result.historyBytes += *bytes;
        }
        for (const auto& array : request.metadata) {
            if (array.alignment == 0 || (array.alignment & (array.alignment - 1)) != 0)
                return std::unexpected(CompileError{ErrorCode::InvalidParameter});
            const auto bytes = checkedSizeMultiply(array.count, array.elementBytes);
            if (!bytes) return std::unexpected(bytes.error());
            if (*bytes == 0) continue;
            const auto remainder = result.totalBytes % array.alignment;
            const auto padding = remainder == 0 ? 0 : array.alignment - remainder;
            if (auto status = append(padding); !status) return std::unexpected(status.error());
            result.paddingBytes += padding;
            if (auto status = append(*bytes); !status) return std::unexpected(status.error());
            result.metadataBytes += *bytes;
        }
        if (result.totalBytes > budgetBytes)
            return std::unexpected(CompileError{ErrorCode::MemoryBudgetExceeded});
        return result;
    }
}
